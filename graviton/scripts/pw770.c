/*
 * pw770 -- how long can one getpwnam() be made to wait?
 *
 * The probe issue 770 asks for. sshd calls getpwnam() pre-auth for every
 * incoming connection, and on Haiku that call is a synchronous round trip to a
 * SINGLE registrar thread with NO reply timeout:
 *
 *   src/system/libroot/posix/user_group_common.cpp:78-87  KMessage::SendTo(...)
 *   headers/private/kernel/util/KMessage.h:167-171        replyTimeout = -1
 *   src/kits/.../KMessage.cpp:576-577, :614-618           negative -> read_port()
 *   src/servers/registrar/AuthenticationManager.cpp:797   one _RequestThread
 *
 * That same thread also writes the on-disk databases (`fGroupDB->WriteToDisk()`
 * at :1199 and :1245, `fUserDB->WriteToDisk()` at :1082, each an fopen("w") at
 * :578). So a lookup is serialized behind a disk write, and if the write can
 * ever block indefinitely then every process that looks up a user is locked out
 * uninterruptibly -- which is exactly the shape of 770's "accepts TCP, never
 * sends a banner".
 *
 * This measures the serialization on a HEALTHY box, which is the cheap half:
 * it establishes how long a WriteToDisk actually holds the thread, so a later
 * full-volume run has a baseline to be compared against. It does not prove
 * anything about the full-volume case on its own.
 *
 *     gcc -o pw770 pw770.c && ./pw770 [user] [iterations]
 *
 * Two phases, same loop, so the numbers are comparable:
 *   idle  -- nothing else is talking to the registrar
 *   churn -- a second thread drives groupadd/groupdel in a tight loop, which is
 *            the least invasive way to force WriteToDisk from userland (it
 *            creates and removes one throwaway group and touches no real user)
 */

#include <OS.h>

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int sChurnStop;
static volatile int sChurnRounds;

static int32
churn(void *arg)
{
	(void)arg;
	while (!sChurnStop) {
		/*
		 * groupadd then groupdel: each is one registrar request that ends in
		 * fGroupDB->WriteToDisk(). Output is discarded -- a failure (e.g. the
		 * group already exists from a previous round) still exercises the
		 * request path, and the lookup latency is what is being measured, not
		 * groupadd's exit status.
		 */
		if (system("groupadd probe770 >/dev/null 2>&1") == -1)
			break;
		if (system("groupdel probe770 >/dev/null 2>&1") == -1)
			break;
		sChurnRounds++;
	}
	return 0;
}

/*
 * One timing pass. Reports the worst case rather than only the mean: a lockout
 * is a tail-latency bug, and a mean over thousands of fast calls hides it.
 */
static int
measure(const char *label, const char *who, int n)
{
	bigtime_t worst = 0;
	bigtime_t total = 0;
	int worstIter = -1;
	int over1ms = 0;
	int over10ms = 0;
	int over100ms = 0;
	int i;

	for (i = 0; i < n; i++) {
		bigtime_t t0 = system_time();
		struct passwd *pw = getpwnam(who);
		bigtime_t dt = system_time() - t0;

		if (pw == NULL) {
			fprintf(stderr, "pw770: getpwnam(\"%s\") failed on iteration %d\n",
				who, i);
			return -1;
		}

		total += dt;
		if (dt > worst) {
			worst = dt;
			worstIter = i;
		}
		if (dt > 1000)
			over1ms++;
		if (dt > 10000)
			over10ms++;
		if (dt > 100000)
			over100ms++;
	}

	printf("%-6s n=%d  mean=%" B_PRIdBIGTIME "us  worst=%" B_PRIdBIGTIME "us"
		" (iter %d)  >1ms=%d  >10ms=%d  >100ms=%d\n",
		label, n, total / n, worst, worstIter, over1ms, over10ms, over100ms);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *who = argc > 1 ? argv[1] : "baron";
	int n = argc > 2 ? atoi(argv[2]) : 2000;
	thread_id churner;
	status_t ignored;

	if (n < 1)
		n = 1;

	printf("pw770: getpwnam(\"%s\") x %d per phase\n\n", who, n);

	if (measure("idle", who, n) != 0)
		return 1;

	sChurnStop = 0;
	sChurnRounds = 0;
	churner = spawn_thread(churn, "pw770 churn", B_NORMAL_PRIORITY, NULL);
	if (churner < 0) {
		fprintf(stderr, "pw770: spawn_thread: %s\n", strerror(churner));
		return 1;
	}
	resume_thread(churner);

	/* Let the churn get going so the first iterations are not measuring an
	   idle registrar. */
	snooze(200000);

	if (measure("churn", who, n) != 0) {
		sChurnStop = 1;
		wait_for_thread(churner, &ignored);
		return 1;
	}

	sChurnStop = 1;
	wait_for_thread(churner, &ignored);

	printf("\nchurn rounds completed: %d (each = one groupadd + one groupdel,"
		" i.e. two WriteToDisk)\n", sChurnRounds);
	return 0;
}
