/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * A general kernel liveness watchdog for device drivers. See
 * headers/private/kernel/device_watchdog.h for the contract and rationale.
 *
 * This generalizes the ENA network driver's watchdog (documented in
 * src/add-ons/kernel/drivers/network/ether/ena/docs/watchdog-design.md). The
 * two structural decisions carried over verbatim, because both were paid for on
 * that driver:
 *
 *   - A kernel thread, not add_timer(): the action a watchdog must be able to
 *     take -- the recovery callback -- issues device commands, takes locks and
 *     blocks for tens of milliseconds, none of which is legal in the interrupt
 *     context an add_timer() hook runs in.
 *   - Observing is separated from deciding: device_watchdog_pet() only stores a
 *     timestamp (lock-free, interrupt-safe) and never acts. Everything that can
 *     block lives on this thread.
 */


#include <device_watchdog.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <condition_variable.h>
#include <elf.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>


//#define TRACE_DEVICE_WATCHDOG
#ifdef TRACE_DEVICE_WATCHDOG
#	define TRACE(x)	dprintf x
#else
#	define TRACE(x) ;
#endif


struct device_watchdog : public DoublyLinkedListLinkImpl<device_watchdog> {
	const char*						name;
	bigtime_t						timeout;
	bigtime_t						interval;
	int32							missesBeforeRecover;
	device_watchdog_recover_func	recover;
	device_watchdog_probe_func		probe;
	void*							cookie;

	// Read lock-free from any context, including interrupt handlers.
	int64							lastPet;

	// The remaining fields are touched only under sWatchdogLock.
	bool							enabled;
	bool							dead;
		// Latched after a recovery callback fails: the device is left inert and
		// no longer watched, so a device that cannot recover is not reset in a
		// loop (which on a console-less host is strictly worse than a wedge).
	bool							busy;
		// A callback for this watchdog is running with the lock dropped;
		// unregister waits this out so the handle cannot be freed underneath it.
	bool							removePending;
	int32							misses;
	bigtime_t						nextCheck;
};

typedef DoublyLinkedList<device_watchdog> WatchdogList;


static mutex sWatchdogLock = MUTEX_INITIALIZER("device watchdog");
static WatchdogList sWatchdogs;
static ConditionVariable sWatchdogCondition;
static sem_id sWatchdogWake = -1;
static bool sThreadRunning = false;


/*!	Runs the per-tick checks over every registered watchdog. sWatchdogLock is
	held on entry and on exit, but is dropped around each probe()/recover()
	callback -- neither may run under the list lock, since recover() blocks and
	could itself want to register or unregister a watchdog. Iteration survives
	that drop via the marker/swap technique (the list may be mutated by an
	unregister while a callback runs).
*/
static void
check_watchdogs()
{
	if (sWatchdogs.IsEmpty())
		return;

	// A placeholder node walked through the list; its callback fields are never
	// invoked, so leaving them uninitialized is safe (as in the low resource
	// manager's call_handlers()).
	device_watchdog marker;
	sWatchdogs.Insert(&marker, false);

	while (device_watchdog* watchdog = sWatchdogs.GetNext(&marker)) {
		// Advance the marker past this node so the walk survives an unlock.
		sWatchdogs.Swap(&marker, watchdog);

		if (watchdog->removePending || watchdog->dead)
			continue;
		if (!watchdog->enabled) {
			// Carrying misses across a disabled window would let two unrelated
			// samples add up to a recovery.
			watchdog->misses = 0;
			continue;
		}

		bigtime_t now = system_time();
		if (now < watchdog->nextCheck)
			continue;
		watchdog->nextCheck = now + watchdog->interval;

		// A positive probe is direct evidence of progress and counts as a fresh
		// heartbeat -- the generalization of ENA granting a late keep-alive
		// patience while the datapath is demonstrably moving frames.
		if (watchdog->probe != NULL) {
			watchdog->busy = true;
			mutex_unlock(&sWatchdogLock);
			bool alive = watchdog->probe(watchdog->cookie);
			mutex_lock(&sWatchdogLock);
			watchdog->busy = false;
			sWatchdogCondition.NotifyAll();

			if (watchdog->removePending)
				continue;
			if (alive) {
				atomic_set64(&watchdog->lastPet, system_time());
				if (watchdog->misses != 0) {
					dprintf("device_watchdog: %s recovered after %" B_PRId32
						" missed deadline(s)\n", watchdog->name,
						watchdog->misses);
					watchdog->misses = 0;
				}
				continue;
			}
		}

		bigtime_t age = system_time() - atomic_get64(&watchdog->lastPet);
		if (age <= watchdog->timeout) {
			// Within the deadline. A run of misses that never reached recovery
			// is logged as recovered and cleared -- a fix that is silent when it
			// works cannot be told apart from a broken watchdog.
			if (watchdog->misses != 0) {
				dprintf("device_watchdog: %s heartbeat recovered after %" B_PRId32
					" missed deadline(s)\n", watchdog->name, watchdog->misses);
				watchdog->misses = 0;
			}
			continue;
		}

		watchdog->misses++;
		if (watchdog->misses < watchdog->missesBeforeRecover) {
			// A single stale sample never acts: a merely-late heartbeat is
			// transient and the next tick clears it. Non-final misses are logged
			// so a cadence that starts creeping is visible before it becomes a
			// recovery.
			dprintf("device_watchdog: %s missed heartbeat %" B_PRId32 "/%" B_PRId32
				" (%" B_PRId64 " ms stale)\n", watchdog->name, watchdog->misses,
				watchdog->missesBeforeRecover, age / 1000);
			continue;
		}

		dprintf("device_watchdog: %s stale %" B_PRId64 " ms across %" B_PRId32
			" checks -- attempting recovery\n", watchdog->name, age / 1000,
			watchdog->misses);

		watchdog->busy = true;
		mutex_unlock(&sWatchdogLock);
		status_t status = watchdog->recover(watchdog->cookie);
		mutex_lock(&sWatchdogLock);
		watchdog->busy = false;
		sWatchdogCondition.NotifyAll();

		if (watchdog->removePending)
			continue;

		if (status == B_OK) {
			watchdog->misses = 0;
			atomic_set64(&watchdog->lastPet, system_time());
			watchdog->nextCheck = system_time() + watchdog->interval;
			dprintf("device_watchdog: %s recovered\n", watchdog->name);
		} else {
			watchdog->dead = true;
			dprintf("device_watchdog: %s recovery failed (%s) -- device left "
				"inert, watchdog disabled\n", watchdog->name, strerror(status));
		}
	}

	sWatchdogs.Remove(&marker);
}


/*!	Computes how long to sleep before the next due check. sWatchdogLock held. */
static bigtime_t
compute_sleep()
{
	bigtime_t now = system_time();
	bigtime_t earliest = now + DEVICE_WATCHDOG_DEFAULT_INTERVAL;

	WatchdogList::Iterator iterator = sWatchdogs.GetIterator();
	while (device_watchdog* watchdog = iterator.Next()) {
		if (!watchdog->enabled || watchdog->dead || watchdog->removePending)
			continue;
		if (watchdog->nextCheck < earliest)
			earliest = watchdog->nextCheck;
	}

	bigtime_t sleep = earliest - now;
	if (sleep < 1000)
		sleep = 1000;
	return sleep;
}


static status_t
device_watchdog_thread(void*)
{
	bigtime_t sleep = DEVICE_WATCHDOG_DEFAULT_INTERVAL;
	while (true) {
		// Sleep on a semaphore rather than snooze() so that a registration
		// change can wake the thread at once to recompute its cadence.
		acquire_sem_etc(sWatchdogWake, 1, B_RELATIVE_TIMEOUT, sleep);

		MutexLocker locker(&sWatchdogLock);
		check_watchdogs();
		sleep = compute_sleep();
	}
	return 0;
}


static int
dump_watchdogs(int argc, char** argv)
{
	kprintf("name              timeout   interval  miss  state       stale(ms)  "
		"recover\n");

	bigtime_t now = system_time();
	WatchdogList::Iterator iterator = sWatchdogs.GetIterator();
	while (device_watchdog* watchdog = iterator.Next()) {
		const char* state = watchdog->dead ? "dead"
			: !watchdog->enabled ? "disabled"
			: watchdog->busy ? "recovering"
			: watchdog->misses != 0 ? "missing" : "ok";

		const char* symbol = NULL;
		elf_debug_lookup_symbol_address((addr_t)watchdog->recover, NULL, &symbol,
			NULL, NULL);

		kprintf("%-16s  %8" B_PRId64 "  %8" B_PRId64 "  %2" B_PRId32 "/%-2" B_PRId32
			"  %-10s  %8" B_PRId64 "  %s\n", watchdog->name, watchdog->timeout,
			watchdog->interval, watchdog->misses, watchdog->missesBeforeRecover,
			state, (now - atomic_get64(&watchdog->lastPet)) / 1000,
			symbol != NULL ? symbol : "?");
	}

	return 0;
}


//	#pragma mark - private kernel API


status_t
device_watchdog_init(void)
{
	new(&sWatchdogs) WatchdogList;
		// Static constructors do not run in the kernel; do it by hand.
	sWatchdogCondition.Init(NULL, "device watchdog");
	return B_OK;
}


status_t
device_watchdog_init_post_thread(void)
{
	sWatchdogWake = create_sem(0, "device watchdog wake");
	if (sWatchdogWake < B_OK)
		return sWatchdogWake;

	// Elevated priority: a watchdog that is itself scheduled late measures
	// scheduler latency and calls it device death.
	thread_id thread = spawn_kernel_thread(&device_watchdog_thread,
		"device watchdog", B_URGENT_DISPLAY_PRIORITY, NULL);
	if (thread < B_OK) {
		delete_sem(sWatchdogWake);
		sWatchdogWake = -1;
		return thread;
	}

	sThreadRunning = true;
	resume_thread(thread);

	add_debugger_command("device_watchdogs", &dump_watchdogs,
		"Dump the list of registered device watchdogs");
	return B_OK;
}


device_watchdog*
register_device_watchdog(const device_watchdog_parameters* parameters)
{
	if (parameters == NULL || parameters->name == NULL
		|| parameters->recover == NULL || parameters->timeout <= 0) {
		return NULL;
	}

	device_watchdog* watchdog = new(std::nothrow) device_watchdog;
	if (watchdog == NULL)
		return NULL;

	watchdog->name = parameters->name;
	watchdog->timeout = parameters->timeout;
	watchdog->interval = parameters->interval > 0
		? parameters->interval : DEVICE_WATCHDOG_DEFAULT_INTERVAL;
	watchdog->missesBeforeRecover = parameters->misses_before_recover > 0
		? parameters->misses_before_recover : DEVICE_WATCHDOG_DEFAULT_MISSES;
	watchdog->recover = parameters->recover;
	watchdog->probe = parameters->probe;
	watchdog->cookie = parameters->cookie;

	bigtime_t now = system_time();
	// Seed the heartbeat to now, not the epoch: the deadline is measured from
	// registration. A zero here would reset a healthy device the first interval
	// after attach.
	watchdog->lastPet = now;
	watchdog->enabled = true;
	watchdog->dead = false;
	watchdog->busy = false;
	watchdog->removePending = false;
	watchdog->misses = 0;
	watchdog->nextCheck = now + watchdog->interval;

	MutexLocker locker(&sWatchdogLock);
	sWatchdogs.Add(watchdog);
	locker.Unlock();

	// Wake the thread so a shorter interval than the current sleep takes effect.
	if (sWatchdogWake >= 0)
		release_sem(sWatchdogWake);

	TRACE(("register_device_watchdog(%s): timeout %lld us, interval %lld us, "
		"%ld misses\n", watchdog->name, watchdog->timeout, watchdog->interval,
		watchdog->missesBeforeRecover));
	return watchdog;
}


void
unregister_device_watchdog(device_watchdog* watchdog)
{
	if (watchdog == NULL)
		return;

	mutex_lock(&sWatchdogLock);

	watchdog->removePending = true;

	// Wait out any callback in flight for this device before freeing it. The
	// checking thread sets busy under the lock before dropping it around the
	// callback, so once busy is clear no callback can touch this object.
	// Wait(mutex*) queues the entry before releasing the lock, so the thread's
	// NotifyAll cannot slip through the gap.
	while (watchdog->busy)
		sWatchdogCondition.Wait(&sWatchdogLock);

	sWatchdogs.Remove(watchdog);
	mutex_unlock(&sWatchdogLock);

	delete watchdog;
}


void
device_watchdog_pet(device_watchdog* watchdog)
{
	if (watchdog == NULL)
		return;

	// The whole observe half: record and return. Lock-free so it is safe from
	// interrupt context, which is where a driver's healthy datapath lives.
	atomic_set64(&watchdog->lastPet, system_time());
}


void
device_watchdog_set_enabled(device_watchdog* watchdog, bool enabled)
{
	if (watchdog == NULL)
		return;

	MutexLocker locker(&sWatchdogLock);

	if (enabled && !watchdog->enabled) {
		// Re-enabling: seed a fresh heartbeat and drop any stale miss run so a
		// long disabled window is not counted against the device.
		bigtime_t now = system_time();
		atomic_set64(&watchdog->lastPet, now);
		watchdog->misses = 0;
		watchdog->nextCheck = now + watchdog->interval;
	}
	watchdog->enabled = enabled;

	locker.Unlock();

	if (enabled && sWatchdogWake >= 0)
		release_sem(sWatchdogWake);
}
