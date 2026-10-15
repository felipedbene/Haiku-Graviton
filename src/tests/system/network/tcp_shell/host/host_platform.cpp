/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	The Haiku userland primitives the kernel-land emulation sits on, for a
	host (non-Haiku) build.

	The emulation in src/tests/add-ons/kernel/kernelland_emu reimplements the
	kernel's locks, condition variables and slab on top of *userland* Haiku:
	threads, semaphores, system_time(), and the thread-parking pair
	_kern_block_thread()/_kern_unblock_thread() with the per-thread wait_status
	they hand values through. On Haiku those come from libroot; on the host
	they come from here, on pthreads.

	libroot_build is deliberately not used for this: its semaphores state up
	front that they assume a single-threaded build (acquire_sem() with no
	timeout calls debugger()) and its thread functions all return B_BAD_VALUE.
	The TCP harness is genuinely concurrent -- a receiver thread per endpoint
	plus the stack's timer thread -- so it needs real ones.

	Deliberate simplifications, all safe for a test harness:
	  - thread and semaphore records are never recycled, so a long-running
		process would grow; a scripted test run is bounded.
	  - priorities are ignored: the host scheduler is not Haiku's, and no test
		here asserts a scheduling order.
	  - timeouts use CLOCK_MONOTONIC, the same clock system_time() reports, so
		the absolute timeouts the stack computes as system_time() + delay mean
		what they say.
*/

#include <OS.h>
#include <driver_settings.h>
#include <syscalls.h>
#include <thread.h>
#include <user_thread.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>


static const bigtime_t kMicrosecondsPerSecond = 1000000;


/*!	Is \a result -- an error number straight out of pthreads or libc -- the host
	spelling of the Haiku error \a haikuError?

	This has to be asked, rather than written as "result == ETIMEDOUT", because
	the build <Errors.h> redefines every E* macro to Haiku's value: errno is
	funnelled through _haiku_build_errno(), which translates, so that host code
	comparing against EINVAL and friends behaves. pthread return values never
	went through that translation, so comparing one against the macro silently
	never matches -- which is a timeout that is never noticed, i.e. a wait that
	never ends.
*/
static inline bool
is_host_error(int result, int haikuError)
{
	return result != 0 && result == _haiku_to_host_error(haikuError);
}


// #pragma mark - time


bigtime_t
system_time(void)
{
	timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (bigtime_t)time.tv_sec * kMicrosecondsPerSecond
		+ time.tv_nsec / 1000;
}


nanotime_t
system_time_nsecs(void)
{
	timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (nanotime_t)time.tv_sec * 1000000000LL + time.tv_nsec;
}


unsigned long
real_time_clock(void)
{
	timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	return (unsigned long)time.tv_sec;
}


bigtime_t
real_time_clock_usecs(void)
{
	timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	return (bigtime_t)time.tv_sec * kMicrosecondsPerSecond
		+ time.tv_nsec / 1000;
}


/*!	Absolute CLOCK_MONOTONIC deadline for a Haiku timeout.

	\a flags is the usual B_RELATIVE_TIMEOUT / B_ABSOLUTE_TIMEOUT pair; with
	neither set there is no deadline and the caller waits indefinitely.
*/
static bool
deadline_for(uint32 flags, bigtime_t timeout, timespec& deadline)
{
	if ((flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) == 0
		|| timeout >= B_INFINITE_TIMEOUT)
		return false;

	bigtime_t absolute = timeout;
	if ((flags & B_RELATIVE_TIMEOUT) != 0)
		absolute += system_time();

	if (absolute < 0)
		absolute = 0;

	deadline.tv_sec = absolute / kMicrosecondsPerSecond;
	deadline.tv_nsec = (absolute % kMicrosecondsPerSecond) * 1000;
	return true;
}


status_t
snooze_etc(bigtime_t amount, int timeBase, uint32 flags)
{
	timespec deadline;
	if (!deadline_for(flags | B_RELATIVE_TIMEOUT, amount, deadline))
		return B_BAD_VALUE;

	while (is_host_error(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&deadline, NULL), EINTR)) {
	}

	return B_OK;
}


status_t
snooze(bigtime_t amount)
{
	return snooze_etc(amount, B_SYSTEM_TIMEBASE, B_RELATIVE_TIMEOUT);
}


status_t
snooze_until(bigtime_t time, int timeBase)
{
	return snooze_etc(time, timeBase, B_ABSOLUTE_TIMEOUT);
}


// #pragma mark - threads


struct host_thread {
	thread_id			id;
	pthread_t			pthread;
	char				name[B_OS_NAME_LENGTH];
	thread_func			entry;
	void*				data;
	bool				resumed;
	bool				exited;
	status_t			exitStatus;
	pthread_mutex_t		lock;
	pthread_cond_t		condition;
	user_thread			userThread;
};


static std::vector<host_thread*> sThreads;
static pthread_mutex_t sThreadsLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t sCurrentThreadKey;
static pthread_once_t sCurrentThreadKeyOnce = PTHREAD_ONCE_INIT;
static thread_id sNextThreadID = 1;


static void
init_current_thread_key()
{
	pthread_key_create(&sCurrentThreadKey, NULL);
}


static host_thread*
create_host_thread(const char* name)
{
	host_thread* thread = new host_thread;
	thread->pthread = pthread_self();
	strlcpy(thread->name, name != NULL ? name : "thread", sizeof(thread->name));
	thread->entry = NULL;
	thread->data = NULL;
	thread->resumed = false;
	thread->exited = false;
	thread->exitStatus = B_OK;

	// The condition variable has to use the same clock as system_time(), since
	// Haiku's absolute timeouts are expressed in that time base.
	pthread_condattr_t attributes;
	pthread_condattr_init(&attributes);
	pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	pthread_mutex_init(&thread->lock, NULL);
	pthread_cond_init(&thread->condition, &attributes);
	pthread_condattr_destroy(&attributes);

	memset(&thread->userThread, 0, sizeof(thread->userThread));
	thread->userThread.pthread = thread->pthread;

	pthread_mutex_lock(&sThreadsLock);
	thread->id = sNextThreadID++;
	sThreads.push_back(thread);
	pthread_mutex_unlock(&sThreadsLock);

	return thread;
}


static host_thread*
find_host_thread(thread_id id)
{
	pthread_mutex_lock(&sThreadsLock);

	host_thread* found = NULL;
	for (size_t i = 0; i < sThreads.size(); i++) {
		if (sThreads[i]->id == id) {
			found = sThreads[i];
			break;
		}
	}

	pthread_mutex_unlock(&sThreadsLock);
	return found;
}


/*!	The host_thread of the calling thread, registering it if it is one we did
	not spawn (the main thread, which still blocks on locks like any other).
*/
static host_thread*
current_host_thread()
{
	pthread_once(&sCurrentThreadKeyOnce, init_current_thread_key);

	host_thread* thread = (host_thread*)pthread_getspecific(sCurrentThreadKey);
	if (thread == NULL) {
		thread = create_host_thread("main");
		thread->resumed = true;
		pthread_setspecific(sCurrentThreadKey, thread);
	}

	return thread;
}


static void*
host_thread_entry(void* data)
{
	host_thread* thread = (host_thread*)data;

	pthread_once(&sCurrentThreadKeyOnce, init_current_thread_key);
	pthread_setspecific(sCurrentThreadKey, thread);

	// spawn_thread() creates the thread suspended; wait for resume_thread().
	pthread_mutex_lock(&thread->lock);
	while (!thread->resumed)
		pthread_cond_wait(&thread->condition, &thread->lock);
	pthread_mutex_unlock(&thread->lock);

	status_t result = thread->entry(thread->data);

	pthread_mutex_lock(&thread->lock);
	thread->exited = true;
	thread->exitStatus = result;
	pthread_cond_broadcast(&thread->condition);
	pthread_mutex_unlock(&thread->lock);

	return NULL;
}


thread_id
spawn_thread(thread_func func, const char* name, int32 priority, void* data)
{
	if (func == NULL)
		return B_BAD_VALUE;

	host_thread* thread = create_host_thread(name);
	thread->entry = func;
	thread->data = data;

	if (pthread_create(&thread->pthread, NULL, host_thread_entry, thread) != 0)
		return B_NO_MORE_THREADS;

	thread->userThread.pthread = thread->pthread;
	return thread->id;
}


status_t
resume_thread(thread_id id)
{
	host_thread* thread = find_host_thread(id);
	if (thread == NULL)
		return B_BAD_THREAD_ID;

	pthread_mutex_lock(&thread->lock);
	thread->resumed = true;
	pthread_cond_broadcast(&thread->condition);
	pthread_mutex_unlock(&thread->lock);

	return B_OK;
}


status_t
wait_for_thread(thread_id id, status_t* _returnCode)
{
	host_thread* thread = find_host_thread(id);
	if (thread == NULL)
		return B_BAD_THREAD_ID;

	pthread_join(thread->pthread, NULL);

	if (_returnCode != NULL)
		*_returnCode = thread->exitStatus;

	return B_OK;
}


status_t
wait_for_thread_etc(thread_id id, uint32 flags, bigtime_t timeout,
	status_t* _returnCode)
{
	// Nothing in the harness waits with a timeout; failing loudly beats
	// pretending a timed join happened.
	if ((flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) != 0
		&& timeout < B_INFINITE_TIMEOUT)
		return B_NOT_SUPPORTED;

	return wait_for_thread(id, _returnCode);
}


thread_id
find_thread(const char* name)
{
	if (name == NULL)
		return current_host_thread()->id;

	pthread_mutex_lock(&sThreadsLock);

	thread_id found = B_NAME_NOT_FOUND;
	for (size_t i = 0; i < sThreads.size(); i++) {
		if (strcmp(sThreads[i]->name, name) == 0) {
			found = sThreads[i]->id;
			break;
		}
	}

	pthread_mutex_unlock(&sThreadsLock);
	return found;
}


status_t
rename_thread(thread_id id, const char* name)
{
	host_thread* thread = find_host_thread(id);
	if (thread == NULL)
		return B_BAD_THREAD_ID;

	strlcpy(thread->name, name, sizeof(thread->name));
	return B_OK;
}


status_t
set_thread_priority(thread_id id, int32 priority)
{
	return find_host_thread(id) != NULL ? B_OK : B_BAD_THREAD_ID;
}


status_t
suspend_thread(thread_id id)
{
	return B_NOT_SUPPORTED;
}


status_t
kill_thread(thread_id id)
{
	return B_NOT_SUPPORTED;
}


/*!	Signals are not emulated.

	The emulation's send_signal_etc() forwards here, and on Haiku a signal is one
	of the ways a blocked thread is woken. Nothing in this harness uses that --
	the stack's threads are woken by releasing or deleting their semaphore -- so
	rather than fake a delivery that would silently do nothing, say so.
*/
int
send_signal(thread_id id, unsigned int signal)
{
	return B_NOT_SUPPORTED;
}


void
exit_thread(status_t status)
{
	host_thread* thread = current_host_thread();

	pthread_mutex_lock(&thread->lock);
	thread->exited = true;
	thread->exitStatus = status;
	pthread_cond_broadcast(&thread->condition);
	pthread_mutex_unlock(&thread->lock);

	pthread_exit(NULL);
}


// #pragma mark - thread blocking


extern "C" struct user_thread*
_haiku_build_get_user_thread(void)
{
	return &current_host_thread()->userThread;
}


/*!	Park the calling thread until someone hands it a status.

	Mirrors the kernel call the emulation expects: the waiter stores
	wait_status = 1 while holding whatever lock publishes it as a waiter, then
	blocks here; the waker calls _kern_unblock_thread() with the status the
	waiter should see. A wake that lands between the store and this call is
	not lost, because wait_status is re-read under the same mutex the waker
	writes it under.
*/
extern "C" status_t
_kern_block_thread(uint32 flags, bigtime_t timeout)
{
	host_thread* thread = current_host_thread();

	timespec deadline;
	bool hasDeadline = deadline_for(flags, timeout, deadline);

	pthread_mutex_lock(&thread->lock);

	while (thread->userThread.wait_status > 0) {
		if (hasDeadline) {
			if (is_host_error(pthread_cond_timedwait(&thread->condition,
					&thread->lock, &deadline), ETIMEDOUT)) {
				thread->userThread.wait_status = B_TIMED_OUT;
				break;
			}
		} else
			pthread_cond_wait(&thread->condition, &thread->lock);
	}

	status_t status = thread->userThread.wait_status;

	pthread_mutex_unlock(&thread->lock);
	return status;
}


extern "C" status_t
_kern_unblock_thread(thread_id id, status_t status)
{
	host_thread* thread = find_host_thread(id);
	if (thread == NULL)
		return B_BAD_THREAD_ID;

	pthread_mutex_lock(&thread->lock);

	if (thread->userThread.wait_status > 0) {
		thread->userThread.wait_status = status;
		pthread_cond_broadcast(&thread->condition);
	}

	pthread_mutex_unlock(&thread->lock);
	return B_OK;
}


// #pragma mark - semaphores


struct host_sem {
	bool				used;
	bool				deleted;
	int32				count;
	char				name[B_OS_NAME_LENGTH];
	pthread_mutex_t		lock;
	pthread_cond_t		condition;
};


static std::vector<host_sem*> sSems;
static pthread_mutex_t sSemsLock = PTHREAD_MUTEX_INITIALIZER;


static host_sem*
find_sem(sem_id id)
{
	pthread_mutex_lock(&sSemsLock);

	host_sem* sem = NULL;
	if (id > 0 && (size_t)id <= sSems.size() && !sSems[id - 1]->deleted)
		sem = sSems[id - 1];

	pthread_mutex_unlock(&sSemsLock);
	return sem;
}


sem_id
create_sem(int32 count, const char* name)
{
	host_sem* sem = new host_sem;
	sem->used = true;
	sem->deleted = false;
	sem->count = count;
	strlcpy(sem->name, name != NULL ? name : "sem", sizeof(sem->name));

	pthread_condattr_t attributes;
	pthread_condattr_init(&attributes);
	pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	pthread_mutex_init(&sem->lock, NULL);
	pthread_cond_init(&sem->condition, &attributes);
	pthread_condattr_destroy(&attributes);

	pthread_mutex_lock(&sSemsLock);
	sSems.push_back(sem);
	sem_id id = (sem_id)sSems.size();
	pthread_mutex_unlock(&sSemsLock);

	return id;
}


status_t
delete_sem(sem_id id)
{
	host_sem* sem = find_sem(id);
	if (sem == NULL)
		return B_BAD_SEM_ID;

	// Waiters have to come out with B_BAD_SEM_ID: that is how the stack's
	// timer thread and the harness' receiver threads are told to quit.
	pthread_mutex_lock(&sem->lock);
	sem->deleted = true;
	pthread_cond_broadcast(&sem->condition);
	pthread_mutex_unlock(&sem->lock);

	return B_OK;
}


status_t
acquire_sem_etc(sem_id id, int32 count, uint32 flags, bigtime_t timeout)
{
	if (count <= 0)
		return B_BAD_VALUE;

	host_sem* sem = find_sem(id);
	if (sem == NULL)
		return B_BAD_SEM_ID;

	timespec deadline;
	bool hasDeadline = deadline_for(flags, timeout, deadline);
	if (hasDeadline && timeout == 0 && (flags & B_RELATIVE_TIMEOUT) != 0) {
		// B_RELATIVE_TIMEOUT with a zero timeout is a poll
		pthread_mutex_lock(&sem->lock);
		status_t status = B_WOULD_BLOCK;
		if (sem->deleted)
			status = B_BAD_SEM_ID;
		else if (sem->count >= count) {
			sem->count -= count;
			status = B_OK;
		}
		pthread_mutex_unlock(&sem->lock);
		return status;
	}

	pthread_mutex_lock(&sem->lock);

	status_t status = B_OK;
	while (true) {
		if (sem->deleted) {
			status = B_BAD_SEM_ID;
			break;
		}

		if (sem->count >= count) {
			sem->count -= count;
			break;
		}

		if (hasDeadline) {
			if (is_host_error(pthread_cond_timedwait(&sem->condition,
					&sem->lock, &deadline), ETIMEDOUT)) {
				status = B_TIMED_OUT;
				break;
			}
		} else
			pthread_cond_wait(&sem->condition, &sem->lock);
	}

	pthread_mutex_unlock(&sem->lock);
	return status;
}


status_t
acquire_sem(sem_id id)
{
	return acquire_sem_etc(id, 1, 0, 0);
}


status_t
release_sem_etc(sem_id id, int32 count, uint32 flags)
{
	if (count <= 0)
		return B_BAD_VALUE;

	host_sem* sem = find_sem(id);
	if (sem == NULL)
		return B_BAD_SEM_ID;

	pthread_mutex_lock(&sem->lock);
	sem->count += count;
	pthread_cond_broadcast(&sem->condition);
	pthread_mutex_unlock(&sem->lock);

	return B_OK;
}


status_t
release_sem(sem_id id)
{
	return release_sem_etc(id, 1, 0);
}


status_t
get_sem_count(sem_id id, int32* _count)
{
	host_sem* sem = find_sem(id);
	if (sem == NULL)
		return B_BAD_SEM_ID;

	if (_count == NULL)
		return B_BAD_VALUE;

	pthread_mutex_lock(&sem->lock);
	*_count = sem->count;
	pthread_mutex_unlock(&sem->lock);

	return B_OK;
}


status_t
set_sem_owner(sem_id id, team_id team)
{
	return find_sem(id) != NULL ? B_OK : B_BAD_SEM_ID;
}


status_t
switch_sem(sem_id semToBeReleased, sem_id id)
{
	if (semToBeReleased >= 0)
		release_sem(semToBeReleased);

	return acquire_sem(id);
}


status_t
switch_sem_etc(sem_id semToBeReleased, sem_id id, int32 count, uint32 flags,
	bigtime_t timeout)
{
	if (semToBeReleased >= 0)
		release_sem(semToBeReleased);

	return acquire_sem_etc(id, count, flags, timeout);
}


// #pragma mark - kernel odds and ends


/*!	A zeroed Thread per host thread.

	The network stack's syscall-restart helpers are the only host-built users
	of thread_get_current_thread(), and they read just Thread::flags and
	Thread::syscall_restart (see the host <thread.h>). Zeroed flags mean "not
	called via a syscall", which is exactly true here, so is_syscall() and
	is_restarted_syscall() answer false and no restart timeout is ever
	consulted.
*/
extern "C" Thread*
thread_get_current_thread(void)
{
	static __thread Thread sThread;

	return &sThread;
}


extern "C" thread_id
thread_get_current_thread_id(void)
{
	return find_thread(NULL);
}


extern "C" team_id
team_get_kernel_team_id(void)
{
	return 0;
}


extern "C" team_id
team_get_current_team_id(void)
{
	return 0;
}


extern "C" int32
smp_get_current_cpu(void)
{
	return 0;
}


/*!	No driver settings exist on the host.

	The real implementation reads /boot/home/config/settings/kernel/drivers/<x>,
	a path with no host equivalent, and its kernel build also wants the boot
	device and the kernel-startup flag. Reporting "no settings file" is the same
	answer a Haiku machine with no such file gives, so every setting keeps its
	compiled-in default -- for the TCP module that means ECN stays off (see
	tcp_init()). A host test that wants a non-default has to set the global.
*/
void*
load_driver_settings(const char* name)
{
	return NULL;
}


status_t
unload_driver_settings(void* handle)
{
	return handle == NULL ? B_BAD_VALUE : B_OK;
}


bool
get_driver_boolean_parameter(void* handle, const char* key, bool unknownValue,
	bool noArgValue)
{
	// Unreachable: load_driver_settings() never hands out a handle.
	return unknownValue;
}


void
debugger(const char* message)
{
	fprintf(stderr, "tcp_shell: debugger(): %s\n", message);
	abort();
}


// #pragma mark - strings

#if !defined(__GLIBC__) || !__GLIBC_PREREQ(2, 38)

/*	glibc only grew strlcpy()/strlcat() in 2.38, and kernel code uses them
	everywhere. Declared for the host in headers/build/posix/string.h. */

extern "C" size_t
strlcpy(char* dest, const char* source, size_t length)
{
	size_t sourceLength = strlen(source);

	if (length > 0) {
		size_t toCopy = sourceLength < length - 1 ? sourceLength : length - 1;
		memcpy(dest, source, toCopy);
		dest[toCopy] = '\0';
	}

	return sourceLength;
}


extern "C" size_t
strlcat(char* dest, const char* source, size_t length)
{
	size_t destLength = strnlen(dest, length);
	size_t sourceLength = strlen(source);

	if (destLength < length) {
		size_t space = length - destLength - 1;
		size_t toCopy = sourceLength < space ? sourceLength : space;
		memcpy(dest + destLength, source, toCopy);
		dest[destLength + toCopy] = '\0';
	}

	return destLength + sourceLength;
}

#endif	// glibc without strlcpy()
