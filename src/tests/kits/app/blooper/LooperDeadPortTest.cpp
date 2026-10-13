/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	A looper whose message port has been deleted must keep dispatching the
	messages sent to it from inside its team, keep honouring Quit(), and must
	not busy-spin a CPU (DeBeOS #655: R Chromium's BApplication thread at 100%,
	strace showing an endless port_count()/port_buffer_size_etc() =
	B_BAD_PORT_ID loop).

	  LooperDeadPortTest                    plain BLooper
	  LooperDeadPortTest --app              BApplication (needs a running
	                                        registrar and app_server; a failed
	                                        constructor is a FAIL, not a skip)
	  LooperDeadPortTest --mutate-dispatch  hold the looper lock while posting,
	                                        so nothing is dispatched: check (1)
	                                        must go red
	  LooperDeadPortTest --mutate-quit      QuitRequested() refuses: check (3)
	                                        must go red

	Checks, all exact or with fixed bounds:
	  (0) before the port is deleted, 10 messages are dispatched (sanity)
	  (1) after it is deleted, 1000 in-team messages are all dispatched, in
	      order, within 3 s
	  (2) the looper thread uses less than kIdleBudget of CPU during 1 s idle
	      (the spinning looper uses the whole second)
	  (3) Quit() ends the looper thread within 3 s
*/


#include <Application.h>
#include <Looper.h>
#include <Message.h>
#include <OS.h>

#include <stdio.h>
#include <string.h>


static const uint32 kSeq = 'seq ';
static const int32 kBefore = 10;
static const int32 kAfter = 1000;
static const bigtime_t kIdleBudget = 50000;	// 50 ms of CPU per idle second

static int sFailures = 0;
static bool sMutateDispatch = false;
static bool sMutateQuit = false;


static void
check(const char* what, bool ok, const char* detailFormat, long long a,
	long long b)
{
	printf("%s %s (", ok ? "ok  " : "FAIL", what);
	printf(detailFormat, a, b);
	printf(")\n");
	if (!ok)
		sFailures++;
}


class Counter {
public:
	Counter() : fCount(0), fNext(0), fOutOfOrder(0) {}

	void Received(BMessage* message)
	{
		int32 index;
		if (message->FindInt32("index", &index) != B_OK)
			return;
		if (index != fNext)
			atomic_add(&fOutOfOrder, 1);
		fNext = index + 1;
		atomic_add(&fCount, 1);
	}

	int32 Count() { return atomic_get(&fCount); }
	int32 OutOfOrder() { return atomic_get(&fOutOfOrder); }

private:
	int32 fCount;
	int32 fNext;
	int32 fOutOfOrder;
};


class TestLooper : public BLooper {
public:
	TestLooper() : BLooper("LooperDeadPortTest looper") {}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kSeq)
			counter.Received(message);
		else
			BLooper::MessageReceived(message);
	}

	virtual bool QuitRequested() { return !sMutateQuit; }

	Counter counter;
};


class TestApp : public BApplication {
public:
	TestApp() : BApplication("application/x-vnd.DeBeOS-LooperDeadPortTest") {}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kSeq)
			counter.Received(message);
		else
			BApplication::MessageReceived(message);
	}

	Counter counter;
};


static port_id
find_port(const char* name)
{
	thread_info thread;
	get_thread_info(find_thread(NULL), &thread);
	port_info info;
	int32 cookie = 0;
	port_id found = -1;
	while (get_next_port_info(thread.team, &cookie, &info) == B_OK) {
		if (strcmp(info.name, name) == 0)
			found = info.port;
	}
	return found;
}


static bigtime_t
cpu_time(thread_id thread)
{
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK)
		return -1;
	return info.user_time + info.kernel_time;
}


static bool
wait_for_count(Counter& counter, int32 expected, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (counter.Count() < expected && system_time() < end)
		snooze(1000);
	return counter.Count() == expected;
}


/*!	The body shared by both variants: \a target posts to the looper under test,
	whose thread is \a thread and whose port is named \a portName.
*/
static void
exercise(BLooper* target, Counter& counter, thread_id thread,
	const char* portName)
{
	for (int32 i = 0; i < kBefore; i++) {
		BMessage message(kSeq);
		message.AddInt32("index", i);
		target->PostMessage(&message);
	}
	check("(0) dispatched before the port is deleted",
		wait_for_count(counter, kBefore, 3000000),
		"%lld of %lld", counter.Count(), kBefore);

	port_id port = find_port(portName);
	check("looper port found", port >= 0, "port %lld, %lld", port, 0);
	check("looper port deleted", delete_port(port) == B_OK,
		"port %lld, %lld", port, 0);
	snooze(300000);
		// let the looper notice

	bigtime_t before = cpu_time(thread);
	snooze(1000000);
	bigtime_t idle = cpu_time(thread) - before;
	check("(2) looper CPU during 1 s idle after the port is gone",
		before >= 0 && idle < kIdleBudget, "%lld us, budget %lld us", idle,
		kIdleBudget);

	if (sMutateDispatch)
		target->Lock();
	for (int32 i = kBefore; i < kBefore + kAfter; i++) {
		BMessage message(kSeq);
		message.AddInt32("index", i);
		target->PostMessage(&message);
	}
	bool all = wait_for_count(counter, kBefore + kAfter, 3000000);
	if (sMutateDispatch)
		target->Unlock();
	check("(1) in-team messages dispatched after the port is gone", all,
		"%lld of %lld", counter.Count(), kBefore + kAfter);
	check("(1) ... in order", counter.OutOfOrder() == 0,
		"%lld out of order, %lld", counter.OutOfOrder(), 0);
}


static int
run_looper()
{
	TestLooper* looper = new TestLooper;
	thread_id thread = looper->Run();
	check("looper running", thread >= 0, "thread %lld, %lld", thread, 0);

	exercise(looper, looper->counter, thread, "LooperDeadPortTest looper");

	looper->PostMessage(B_QUIT_REQUESTED);
	status_t exitValue;
	status_t status = wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 3000000,
		&exitValue);
	check("(3) Quit() ends the looper thread", status == B_OK,
		"wait_for_thread_etc %lld, %lld", status, 0);
	return 0;
}


struct AppControl {
	TestApp* app;
	thread_id appThread;
};


static status_t
app_controller(void* data)
{
	AppControl* control = (AppControl*)data;
	snooze(200000);
		// let Run() get going
	exercise(control->app, control->app->counter, control->appThread,
		"AppLooperPort");
	control->app->PostMessage(B_QUIT_REQUESTED);
	return B_OK;
}


static int
run_app()
{
	TestApp* app = new TestApp;
	check("BApplication constructed", app->InitCheck() == B_OK,
		"InitCheck %lld, %lld", app->InitCheck(), 0);
	if (app->InitCheck() != B_OK)
		return 1;

	AppControl control = { app, find_thread(NULL) };
	thread_id controller = spawn_thread(app_controller, "controller",
		B_NORMAL_PRIORITY, &control);
	resume_thread(controller);

	bigtime_t start = system_time();
	app->Run();
	bigtime_t ran = system_time() - start;
	status_t exitValue;
	wait_for_thread(controller, &exitValue);
	check("(3) Quit() ends BApplication::Run()", ran < 30000000,
		"after %lld us, limit %lld us", ran, 30000000);
	delete app;
	return 0;
}


int
main(int argc, char** argv)
{
	bool app = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--app") == 0)
			app = true;
		else if (strcmp(argv[i], "--mutate-dispatch") == 0)
			sMutateDispatch = true;
		else if (strcmp(argv[i], "--mutate-quit") == 0)
			sMutateQuit = true;
	}

	if (app)
		run_app();
	else
		run_looper();

	printf("LOOPER_DEAD_PORT_TEST %s failures=%d\n",
		sFailures == 0 ? "PASS" : "FAIL", sFailures);
	return sFailures == 0 ? 0 : 1;
}
