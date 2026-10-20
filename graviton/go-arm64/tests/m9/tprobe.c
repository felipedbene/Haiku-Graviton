/* tprobe <thread> [samples] [events] -- stop a thread via the team debugger,
 * dump its user CPU state + code/stack words, and log signal events.
 *
 * Diagnostic used for issue 843. When the thread is stopped in Go's
 * runtime.sigtrampgo it also decodes the handler's info/ctx arguments (spilled
 * at 144/152(sp) by that function's prologue) and prints the interrupted
 * context, which is where the clobbered x28 shows up. The canonical image has
 * no compiler; cross-build it against this tree's headers and libroot. */
#include <OS.h>
#include <debugger.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static port_id gPort, gNub, gReply;
static int gFailed;
	/* set when talking to the nub fails; main exits non-zero so a broken
	 * probe is not mistaken for a clean run */

static void portfail(const char* what, status_t err)
{
	fprintf(stderr, "tprobe: %s: %s\n", what, strerror(err));
	gFailed = 1;
}

/* Returns the number of bytes read, or -1. The nub may return fewer bytes
 * than asked (the read stops at the first unmapped page), so callers that
 * decode a fixed layout must use readfull(). */
static int readmem(void* addr, void* buf, int size)
{
	debug_nub_read_memory m; m.reply_port = gReply; m.address = addr; m.size = size;
	status_t err = write_port(gNub, B_DEBUG_MESSAGE_READ_MEMORY, &m, sizeof(m));
	if (err != B_OK) { portfail("write_port(READ_MEMORY)", err); return -1; }
	debug_nub_read_memory_reply r; int32 code;
	memset(&r, 0, sizeof(r));
	ssize_t n = read_port(gReply, &code, &r, sizeof(r));
	if (n < 0) { portfail("read_port(READ_MEMORY reply)", n); return -1; }
	if (n < (ssize_t)offsetof(debug_nub_read_memory_reply, data)) {
		portfail("short READ_MEMORY reply", B_BAD_DATA); return -1;
	}
	if (r.error != B_OK) return -1;
	int got = r.size;
	if (got < 0 || got > size
		|| got > n - (ssize_t)offsetof(debug_nub_read_memory_reply, data)) {
		portfail("bogus READ_MEMORY reply size", B_BAD_DATA); return -1;
	}
	memcpy(buf, r.data, got); return got;
}

static bool readfull(void* addr, void* buf, int size)
{
	return readmem(addr, buf, size) == size;
}

static void dumpstate(thread_id t)
{
	debug_nub_get_cpu_state m; m.reply_port = gReply; m.thread = t;
	status_t err = write_port(gNub, B_DEBUG_MESSAGE_GET_CPU_STATE, &m, sizeof(m));
	if (err != B_OK) { portfail("write_port(GET_CPU_STATE)", err); return; }
	debug_nub_get_cpu_state_reply r; int32 code;
	memset(&r, 0, sizeof(r));
	ssize_t n = read_port(gReply, &code, &r, sizeof(r));
	if (n < 0) { portfail("read_port(GET_CPU_STATE reply)", n); return; }
	if (n != (ssize_t)sizeof(r)) {
		fprintf(stderr, "tprobe: GET_CPU_STATE reply is %zd bytes, want %zu\n",
			n, sizeof(r));
		gFailed = 1; return;
	}
	if (r.error != B_OK) { printf("get_cpu_state: %s\n", strerror(r.error)); return; }
	debug_cpu_state* s = &r.cpu_state;
	printf("pc=%#lx lr=%#lx sp=%#lx spsr=%#x\n", s->elr, s->lr, s->sp, s->spsr);
	for (int i = 0; i < 30; i += 4)
		printf("  x%-2d %#18lx %#18lx %#18lx %#18lx\n", i, s->x[i], i+1<30?s->x[i+1]:0,
			i+2<30?s->x[i+2]:0, i+3<30?s->x[i+3]:0);
	uint32 ins[8];
	if (readfull((void*)(s->elr - 16), ins, sizeof(ins)))
		printf("  code@pc-16: %08x %08x %08x %08x [%08x] %08x %08x %08x\n",
			ins[0],ins[1],ins[2],ins[3],ins[4],ins[5],ins[6],ins[7]);
	else printf("  code@pc unreadable\n");
	uint64 st[8];
	if (readfull((void*)s->sp, st, sizeof(st)))
		printf("  stack: %#lx %#lx %#lx %#lx %#lx %#lx %#lx %#lx\n",
			st[0],st[1],st[2],st[3],st[4],st[5],st[6],st[7]);
	else printf("  stack unreadable\n");
	/* Go sigtrampgo frame: info at 144(sp), ctx at 152(sp) */
	uint64 args[2];
	if (readfull((void*)(s->sp + 144), args, sizeof(args))) {
		int32 si[2]; uint64 mc[34];
		if (readfull((void*)args[0], si, sizeof(si)))
			printf("  siginfo@%#lx signo=%d code=%d\n", args[0], si[0], si[1]);
		if (readfull((void*)(args[1] + 48), mc, sizeof(mc))) {
			printf("  ctx@%#lx interrupted: pc=%#lx lr=%#lx sp=%#lx spsr=%#lx x28=%#lx x27=%#lx x0=%#lx x1=%#lx\n",
				args[1], mc[32], mc[30], mc[31], mc[33], mc[28], mc[27], mc[0], mc[1]);
			uint32 ci[8];
			if (readfull((void*)(mc[32] - 16), ci, sizeof(ci)))
				printf("  code@ipc-16: %08x %08x %08x %08x [%08x] %08x %08x %08x\n",
					ci[0],ci[1],ci[2],ci[3],ci[4],ci[5],ci[6],ci[7]);
		}
	}
}

static void cont(thread_id t)
{
	debug_nub_continue_thread c; c.thread = t;
	c.handle_event = B_THREAD_DEBUG_HANDLE_EVENT; c.single_step = false;
	status_t err = write_port(gNub, B_DEBUG_MESSAGE_CONTINUE_THREAD, &c, sizeof(c));
	if (err != B_OK) portfail("write_port(CONTINUE_THREAD)", err);
}

static int usage(const char* argv0)
{
	fprintf(stderr, "usage: %s <thread> [samples] [events]\n", argv0);
	return 2;
}

static bool parseint(const char* str, long min, long* out)
{
	char* end;
	long v = strtol(str, &end, 10);
	if (end == str || *end != '\0' || v < min || v > INT32_MAX) return false;
	*out = v; return true;
}

int main(int argc, char** argv)
{
	long tidArg, samplesArg = 3, eventsArg = 50;
	if (argc < 2 || argc > 4 || !parseint(argv[1], 1, &tidArg)
		|| (argc > 2 && !parseint(argv[2], 1, &samplesArg))
		|| (argc > 3 && !parseint(argv[3], 1, &eventsArg)))
		return usage(argv[0]);
	thread_id tid = (thread_id)tidArg;
	int samples = (int)samplesArg;
	int maxEvents = (int)eventsArg;
	thread_info ti;
	if (get_thread_info(tid, &ti) != B_OK) { printf("no thread\n"); return 1; }
	gPort = create_port(100, "tprobe"); gReply = create_port(10, "tprobe reply");
	if (gPort < 0 || gReply < 0) {
		portfail("create_port", gPort < 0 ? gPort : gReply); return 1;
	}
	gNub = install_team_debugger(ti.team, gPort);
	if (gNub < 0) { printf("install: %s\n", strerror(gNub)); return 1; }
	debug_nub_set_team_flags f; f.flags = B_TEAM_DEBUG_SIGNALS;
	status_t err = write_port(gNub, B_DEBUG_MESSAGE_SET_TEAM_FLAGS, &f, sizeof(f));
	if (err != B_OK) {
		portfail("write_port(SET_TEAM_FLAGS)", err);
		remove_team_debugger(ti.team); return 1;
	}
	int got = 0, events = 0;
	debug_thread(tid);
	bigtime_t deadline = system_time() + 10000000;
	while (got < samples && events < maxEvents && system_time() < deadline) {
		debug_debugger_message_data msg; int32 code;
		ssize_t n = read_port_etc(gPort, &code, &msg, sizeof(msg), B_RELATIVE_TIMEOUT, 3000000);
		if (n < 0) { printf("timeout waiting (%s)\n", strerror(n)); break; }
		thread_id from = msg.origin.thread;
		events++;
		if (code == B_DEBUGGER_MESSAGE_THREAD_DEBUGGED) {
			printf("[%lld] THREAD_DEBUGGED t=%d\n", (long long)system_time(), from);
			if (from == tid) { dumpstate(tid); got++; }
			cont(from);
			if (from == tid && got < samples) { snooze(200000); debug_thread(tid); }
		} else if (code == B_DEBUGGER_MESSAGE_SIGNAL_RECEIVED) {
			printf("[%lld] SIGNAL t=%d sig=%d code=%d addr=%p deadly=%d handler=%p flags=%#x\n",
				(long long)system_time(), from, msg.signal_received.signal, msg.signal_received.info.si_code,
				msg.signal_received.info.si_addr, msg.signal_received.deadly,
				(void*)msg.signal_received.handler.sa_handler, msg.signal_received.handler.sa_flags);
			if (from == tid) dumpstate(tid);
			cont(from);
		} else {
			printf("event %d from t=%d\n", (int)code, from);
			if (msg.origin.nub_port >= 0) cont(from);
		}
	}
	remove_team_debugger(ti.team);
	return gFailed ? 1 : 0;
}
