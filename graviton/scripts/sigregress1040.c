/*
 * sigregress1040.c -- ordinary signal-handling regression checks for #1040
 * (arm64 sigreturn SPSR/ELR validation). Every check is normal, well-defined
 * POSIX/arm64 use of signals; run it unchanged on the trunk kernel and on the
 * candidate and compare. Exit status is the number of failed checks.
 *
 *   gcc -O2 -Wall -o sigregress1040 sigregress1040.c && ./sigregress1040
 */
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#ifndef __HAIKU__
#include <ucontext.h>
#endif
#include <unistd.h>

static int sPass, sFail;

static void
check(int ok, const char* name, const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, buf);
	fflush(stdout);
	if (ok)
		sPass++;
	else
		sFail++;
}

static void
install(int sig, void* handler, int flags, const sigset_t* mask)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	if (flags & SA_SIGINFO)
		sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))handler;
	else
		sa.sa_handler = (void (*)(int))handler;
	sa.sa_flags = flags;
	if (mask != NULL)
		sa.sa_mask = *mask;
	else
		sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, NULL) != 0) {
		perror("sigaction");
		exit(100);
	}
}

static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}


/* 1. plain handler (no SA_SIGINFO) */
static volatile sig_atomic_t sPlainSig, sPlainCount;
static void plain_handler(int sig) { sPlainSig = sig; sPlainCount++; }

static void
test_plain(void)
{
	install(SIGUSR1, plain_handler, 0, NULL);
	sPlainSig = 0; sPlainCount = 0;
	int r = raise(SIGUSR1);
	check(r == 0 && sPlainSig == SIGUSR1 && sPlainCount == 1, "sigaction-plain-raise",
		"raise=%d sig=%d count=%d", r, (int)sPlainSig, (int)sPlainCount);
	r = kill(getpid(), SIGUSR1);
	check(r == 0 && sPlainCount == 2, "sigaction-plain-kill", "kill=%d count=%d", r,
		(int)sPlainCount);
}


/* 2. SA_SIGINFO handler: si_signo / si_code / si_pid / si_value, ucontext */
static volatile int sInfoSigno, sInfoCode, sInfoValue, sInfoHaveUc;
static volatile pid_t sInfoPid;
static volatile uint64_t sInfoSpsr;

static void
info_handler(int sig, siginfo_t* info, void* ctx)
{
	ucontext_t* uc = ctx;
	sInfoSigno = info->si_signo;
	sInfoCode = info->si_code;
	sInfoPid = info->si_pid;
	sInfoValue = info->si_value.sival_int;
	sInfoHaveUc = uc != NULL;
	if (uc != NULL)
		sInfoSpsr = uc->uc_mcontext.spsr;
}

static void
test_siginfo(void)
{
	install(SIGUSR2, info_handler, SA_SIGINFO, NULL);
	sInfoSigno = 0;
	kill(getpid(), SIGUSR2);
	check(sInfoSigno == SIGUSR2 && sInfoCode == SI_USER && sInfoPid == getpid()
			&& sInfoHaveUc, "sigaction-siginfo-kill",
		"signo=%d code=%d pid=%d(self %d) uc=%d", sInfoSigno, sInfoCode,
		(int)sInfoPid, (int)getpid(), sInfoHaveUc);
	/* The interrupted context is a userland one: M[3:0] == 0 (EL0t), nRW = 0. */
	check((sInfoSpsr & 0x1f) == 0, "siginfo-ucontext-spsr-el0t",
		"spsr=%#llx", (unsigned long long)sInfoSpsr);

	union sigval v;
	v.sival_int = 0x1040;
	sInfoSigno = 0;
	int r = sigqueue(getpid(), SIGUSR2, v);
	check(r == 0 && sInfoSigno == SIGUSR2 && sInfoCode == SI_QUEUE
			&& sInfoValue == 0x1040, "sigaction-siginfo-sigqueue",
		"sigqueue=%d signo=%d code=%d value=%#x", r, sInfoSigno, sInfoCode,
		sInfoValue);
}


/* 3. SA_RESTART on a blocking read() interrupted by SIGALRM */
static volatile sig_atomic_t sAlarmCount;
static void alarm_handler(int sig) { (void)sig; sAlarmCount++; }

struct writer_args { int fd; double delay; };

static void*
delayed_writer(void* arg)
{
	struct writer_args* w = arg;
	usleep((useconds_t)(w->delay * 1e6));
	write(w->fd, "x", 1);
	return NULL;
}

static void
read_interrupted(int restart, ssize_t* _n, int* _err, double* _elapsed, int* _alarms)
{
	int p[2];
	pipe(p);
	install(SIGALRM, alarm_handler, restart ? SA_RESTART : 0, NULL);
	sAlarmCount = 0;
	/* SIGALRM is process-directed: create the writer with it blocked so it
	 * can only land on the thread blocked in read(). */
	sigset_t al, old;
	sigemptyset(&al);
	sigaddset(&al, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &al, &old);
	pthread_t t;
	struct writer_args w = { p[1], 0.6 };
	pthread_create(&t, NULL, delayed_writer, &w);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	struct itimerval it;
	memset(&it, 0, sizeof(it));
	it.it_value.tv_usec = 200000;
	setitimer(ITIMER_REAL, &it, NULL);
	char c;
	double s = now();
	errno = 0;
	ssize_t n = read(p[0], &c, 1);
	*_err = errno;
	*_elapsed = now() - s;
	*_n = n;
	*_alarms = sAlarmCount;
	pthread_join(t, NULL);
	close(p[0]);
	close(p[1]);
}

static void
test_restart(void)
{
	ssize_t n; int err, alarms; double el;
	read_interrupted(1, &n, &err, &el, &alarms);
	check(n == 1 && alarms == 1 && el > 0.45, "sa_restart-read-restarted",
		"read=%zd errno=%d alarms=%d elapsed=%.2fs", n, err, alarms, el);
	read_interrupted(0, &n, &err, &el, &alarms);
	check(n == -1 && err == EINTR && alarms == 1 && el < 0.45,
		"no-sa_restart-read-eintr", "read=%zd errno=%d alarms=%d elapsed=%.2fs",
		n, err, alarms, el);
}


/* 4. sigaltstack */
static char* sAltBase;
static size_t sAltSize;
static volatile int sAltOnStack, sAltFlagOnStack, sAltCount;

static void
alt_handler(int sig)
{
	(void)sig;
	char local;
	stack_t cur;
	sAltOnStack = &local >= sAltBase && &local < sAltBase + sAltSize;
	sigaltstack(NULL, &cur);
	sAltFlagOnStack = (cur.ss_flags & SS_ONSTACK) != 0;
	sAltCount++;
}

static void
test_altstack(void)
{
	sAltSize = 64 * 1024;
	sAltBase = malloc(sAltSize);
	stack_t ss;
	ss.ss_sp = sAltBase;
	ss.ss_size = sAltSize;
	ss.ss_flags = 0;
	int r = sigaltstack(&ss, NULL);
	install(SIGUSR1, alt_handler, SA_ONSTACK, NULL);
	sAltCount = 0;
	for (int i = 0; i < 1000; i++)
		raise(SIGUSR1);
	check(r == 0 && sAltCount == 1000 && sAltOnStack && sAltFlagOnStack,
		"sigaltstack-handler", "sigaltstack=%d count=%d on-alt-stack=%d SS_ONSTACK=%d",
		r, sAltCount, sAltOnStack, sAltFlagOnStack);
	stack_t after;
	sigaltstack(NULL, &after);
	check(!(after.ss_flags & SS_ONSTACK), "sigaltstack-left-after-return",
		"ss_flags=%#x", after.ss_flags);
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, NULL);
	install(SIGUSR1, plain_handler, 0, NULL);
}


/* 5. nested signals */
static volatile int sDepth, sMaxDepth, sOrder[4], sOrderN;

static void
inner_handler(int sig)
{
	(void)sig;
	sDepth++;
	if (sDepth > sMaxDepth)
		sMaxDepth = sDepth;
	sOrder[sOrderN++ & 3] = 2;
	sDepth--;
}

static void
outer_handler(int sig)
{
	(void)sig;
	sDepth++;
	if (sDepth > sMaxDepth)
		sMaxDepth = sDepth;
	sOrder[sOrderN++ & 3] = 1;
	raise(SIGUSR2);
	sOrder[sOrderN++ & 3] = 3;
	sDepth--;
}

static void
test_nested(void)
{
	install(SIGUSR2, inner_handler, 0, NULL);
	install(SIGUSR1, outer_handler, 0, NULL);
	sDepth = sMaxDepth = sOrderN = 0;
	raise(SIGUSR1);
	check(sMaxDepth == 2 && sOrderN == 3 && sOrder[0] == 1 && sOrder[1] == 2
			&& sOrder[2] == 3 && sDepth == 0, "nested-unblocked",
		"maxdepth=%d order=%d,%d,%d", sMaxDepth, sOrder[0], sOrder[1], sOrder[2]);

	/* With SIGUSR2 in sa_mask, the inner one is deferred until return. */
	sigset_t m;
	sigemptyset(&m);
	sigaddset(&m, SIGUSR2);
	install(SIGUSR1, outer_handler, 0, &m);
	sDepth = sMaxDepth = sOrderN = 0;
	raise(SIGUSR1);
	check(sMaxDepth == 1 && sOrderN == 3 && sOrder[0] == 1 && sOrder[1] == 3
			&& sOrder[2] == 2 && sDepth == 0, "nested-masked-deferred",
		"maxdepth=%d order=%d,%d,%d", sMaxDepth, sOrder[0], sOrder[1], sOrder[2]);
	install(SIGUSR2, plain_handler, 0, NULL);
	install(SIGUSR1, plain_handler, 0, NULL);
}


/* 6. raise/kill ping-pong between two threads, 10k round trips */
#define PINGPONG 10000
static pthread_t sPingThread, sPongThread;
static volatile sig_atomic_t sGotPing, sGotPong;
static volatile int sPings, sPongs;
static void ping_handler(int sig) { (void)sig; sGotPing = 1; sPings++; }
static void pong_handler(int sig) { (void)sig; sGotPong = 1; sPongs++; }

static void*
pong_thread(void* arg)
{
	(void)arg;
	sigset_t wait;
	pthread_sigmask(SIG_BLOCK, NULL, &wait);
	sigdelset(&wait, SIGUSR1);
	for (int i = 0; i < PINGPONG; i++) {
		while (!sGotPing)
			sigsuspend(&wait);
		sGotPing = 0;
		pthread_kill(sPingThread, SIGUSR2);
	}
	return NULL;
}

static void
test_pingpong(void)
{
	install(SIGUSR1, ping_handler, 0, NULL);
	install(SIGUSR2, pong_handler, 0, NULL);
	sigset_t both, old;
	sigemptyset(&both);
	sigaddset(&both, SIGUSR1);
	sigaddset(&both, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &both, &old);
	sPingThread = pthread_self();
	sPings = sPongs = 0;
	sGotPing = sGotPong = 0;
	pthread_create(&sPongThread, NULL, pong_thread, NULL);
	sigset_t w2;
	pthread_sigmask(SIG_BLOCK, NULL, &w2);
	sigdelset(&w2, SIGUSR2);
	double s = now();
	for (int i = 0; i < PINGPONG; i++) {
		pthread_kill(sPongThread, SIGUSR1);
		while (!sGotPong)
			sigsuspend(&w2);
		sGotPong = 0;
	}
	double el = now() - s;
	pthread_join(sPongThread, NULL);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	check(sPings == PINGPONG && sPongs == PINGPONG, "pingpong-threads-10k",
		"pings=%d pongs=%d %.2fs (%.1f us/round trip)", sPings, sPongs, el,
		el * 1e6 / PINGPONG);

	/* raise() 10k times, single thread */
	install(SIGUSR1, plain_handler, 0, NULL);
	sPlainCount = 0;
	s = now();
	for (int i = 0; i < PINGPONG; i++)
		raise(SIGUSR1);
	el = now() - s;
	check(sPlainCount == PINGPONG, "raise-10k", "count=%d %.2fs", (int)sPlainCount, el);
	install(SIGUSR2, plain_handler, 0, NULL);
}


/* 7. a handler that edits ordinary registers in its ucontext */
#define X19_SENTINEL	0x5a5a1040abcd0001ULL
#define X19_MAGIC		0x0123456789abcdefULL
#define X19_SENTINEL2	0x5a5a1040abcd0002ULL
#define X19_MAGIC2		0x0fedcba987654321ULL
static volatile int sX19Hits;
static volatile uint64_t sX19Spsr;

static void
x19_async_handler(int sig, siginfo_t* info, void* ctx)
{
	(void)sig; (void)info;
	ucontext_t* uc = ctx;
	if (uc->uc_mcontext.x[19] == X19_SENTINEL) {
		uc->uc_mcontext.x[19] = X19_MAGIC;
		sX19Spsr = uc->uc_mcontext.spsr;
		sX19Hits++;
	}
}

static volatile int sKickerStop;
static void*
kicker(void* arg)
{
	pthread_t target = *(pthread_t*)arg;
	while (!sKickerStop) {
		usleep(2000);
		pthread_kill(target, SIGUSR1);
	}
	return NULL;
}

static void
test_ucontext_async(void)
{
	install(SIGUSR1, x19_async_handler, SA_SIGINFO, NULL);
	sX19Hits = 0;
	sKickerStop = 0;
	pthread_t self = pthread_self(), k;
	pthread_create(&k, NULL, kicker, &self);
	uint64_t o19, o20;
	/* Spin until x19 changes under us; x20 bounds the loop (~seconds). */
	__asm__ __volatile__(
		"mov x19, %2\n"
		"mov x20, %3\n"
		"1: cmp x19, %2\n"
		"b.ne 2f\n"
		"subs x20, x20, #1\n"
		"b.ne 1b\n"
		"2: mov %0, x19\n"
		"mov %1, x20\n"
		: "=&r"(o19), "=&r"(o20)
		: "r"(X19_SENTINEL), "r"(0x200000000ULL)
		: "x19", "x20", "cc", "memory");
	sKickerStop = 1;
	pthread_join(k, NULL);
	check(o19 == X19_MAGIC && o20 != 0 && sX19Hits == 1,
		"ucontext-x19-async-edit", "x19=%#llx loop-left=%#llx hits=%d spsr=%#llx",
		(unsigned long long)o19, (unsigned long long)o20, sX19Hits,
		(unsigned long long)sX19Spsr);
	install(SIGUSR1, plain_handler, 0, NULL);
}

static volatile int sIllHits;
static volatile uint64_t sIllSpsr;

static void
ill_handler(int sig, siginfo_t* info, void* ctx)
{
	(void)sig; (void)info;
	ucontext_t* uc = ctx;
	sIllHits++;
	sIllSpsr = uc->uc_mcontext.spsr;
	if (uc->uc_mcontext.x[19] == X19_SENTINEL2)
		uc->uc_mcontext.x[19] = X19_MAGIC2;
	/* Skip the faulting instruction, and set N and V (Z and C are already set
	 * by the cmp before it): a handler may legitimately change the flags. */
	uc->uc_mcontext.elr += 4;
	uc->uc_mcontext.spsr |= 0x90000000ULL;
}

static void
test_ucontext_sync(void)
{
	install(SIGILL, ill_handler, SA_SIGINFO, NULL);
	sIllHits = 0;
	uint64_t o19, nzcv;
	__asm__ __volatile__(
		"mov x19, %2\n"
		"cmp x19, x19\n"		/* Z=1 C=1 N=0 V=0 */
		".inst 0x00000000\n"	/* udf #0 -> SIGILL */
		"mov %0, x19\n"
		"mrs %1, nzcv\n"
		: "=&r"(o19), "=&r"(nzcv)
		: "r"(X19_SENTINEL2)
		: "x19", "cc", "memory");
	check(sIllHits == 1 && o19 == X19_MAGIC2, "ucontext-x19-sync-edit",
		"hits=%d x19=%#llx", sIllHits, (unsigned long long)o19);
	check(sIllHits == 1 && (sIllSpsr & 0xf0000000ULL) == 0x60000000ULL
			&& nzcv == 0xf0000000ULL, "ucontext-nzcv-edit",
		"spsr-in-handler=%#llx nzcv-after=%#llx", (unsigned long long)sIllSpsr,
		(unsigned long long)nzcv);
	signal(SIGILL, SIG_DFL);
}


/* 8. FP/integer state survives a storm of async signals whose handler
 * clobbers FP registers */
static volatile double sSink;
static void
fp_clobber_handler(int sig)
{
	(void)sig;
	double a = 1.0;
	for (int i = 0; i < 64; i++)
		a = a * 1.0000001 + 0.5;
	sSink = a;
	sPlainCount++;
}

static void
fp_work(uint64_t iters, double* _d, uint64_t* _u)
{
	double d = 0.0;
	uint64_t u = 0x9e3779b97f4a7c15ULL;
	for (uint64_t i = 0; i < iters; i++) {
		d = d * 0.999999 + (double)(i & 1023) * 1e-3;
		u ^= u << 13; u ^= u >> 7; u ^= u << 17;
	}
	*_d = d;
	*_u = u;
}

static void
test_fp_storm(void)
{
	const uint64_t iters = 200000000ULL;
	double refD, d;
	uint64_t refU, u;
	fp_work(iters, &refD, &refU);
	install(SIGUSR1, fp_clobber_handler, 0, NULL);
	sPlainCount = 0;
	sKickerStop = 0;
	pthread_t self = pthread_self(), k;
	pthread_create(&k, NULL, kicker, &self);
	fp_work(iters, &d, &u);
	sKickerStop = 1;
	pthread_join(k, NULL);
	check(memcmp(&d, &refD, sizeof(d)) == 0 && u == refU && sPlainCount > 10,
		"fp-int-state-under-signals", "signals=%d result %s", (int)sPlainCount,
		memcmp(&d, &refD, sizeof(d)) == 0 && u == refU ? "identical" : "DIFFERS");
	install(SIGUSR1, plain_handler, 0, NULL);
}


/* 9. SIGSEGV from a real NULL dereference, handler siglongjmps out */
static sigjmp_buf sJmp;
static volatile int sSegvHits;
static volatile void* sSegvAddr;
static volatile int sSegvCode;

static void
segv_handler(int sig, siginfo_t* info, void* ctx)
{
	(void)sig; (void)ctx;
	sSegvHits++;
	sSegvAddr = info->si_addr;
	sSegvCode = info->si_code;
	siglongjmp(sJmp, 1);
}

static void
test_segv(void)
{
	install(SIGSEGV, segv_handler, SA_SIGINFO, NULL);
	int ok = 1;
	sSegvHits = 0;
	for (int i = 0; i < 100; i++) {
		if (sigsetjmp(sJmp, 1) == 0) {
			volatile int* p = NULL;
			*p = i;
			ok = 0;	/* not reached */
		}
	}
	sigset_t cur;
	pthread_sigmask(SIG_BLOCK, NULL, &cur);
	int blocked = sigismember(&cur, SIGSEGV);
	check(ok && sSegvHits == 100 && !blocked, "sigsegv-null-siglongjmp-x100",
		"hits=%d si_addr=%p si_code=%d segv-blocked-after=%d", sSegvHits,
		sSegvAddr, sSegvCode, blocked);
	signal(SIGSEGV, SIG_DFL);
}


int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	test_plain();
	test_siginfo();
	test_restart();
	test_altstack();
	test_nested();
	test_pingpong();
	test_ucontext_async();
	test_ucontext_sync();
	test_fp_storm();
	test_segv();
	printf("RESULT pass=%d fail=%d\n", sPass, sFail);
	return sFail;
}
