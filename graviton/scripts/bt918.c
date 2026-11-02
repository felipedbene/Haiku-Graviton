/*
 * bt918 -- print every thread of a running team with its state and a
 * frame-pointer backtrace, for diagnosing a hung process on an image that
 * ships no Debugger or gdb (issue 918).
 *
 * The thread list (state, the semaphore it waits on) is taken before
 * attaching, so it shows what the team was doing undisturbed. Each thread is
 * then stopped with debug_thread(); a thread that does not stop within the
 * timeout is in an uninterruptible kernel wait, which is itself reported.
 * The stopped threads are continued and the debugger detached afterwards.
 *
 * Build on Haiku: gcc -O1 -o bt918 bt918.c -ldebug
 * Usage: bt918 <team-id> [max-frames]
 */

#include <OS.h>
#include <debugger.h>
#include <image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libdebug's debug_support API (headers/private/debug/debug_support.h). The
 * private header is not installed with haiku_devel, so declare what is used. */
typedef struct debug_context {
	team_id	team;
	port_id	nub_port;
	port_id	reply_port;
} debug_context;
typedef struct debug_symbol_lookup_context debug_symbol_lookup_context;

status_t init_debug_context(debug_context*, team_id, port_id);
void destroy_debug_context(debug_context*);
ssize_t debug_read_memory(debug_context*, const void*, void*, size_t);
status_t debug_get_cpu_state(debug_context*, thread_id,
	debug_debugger_message*, debug_cpu_state*);
status_t debug_create_symbol_lookup_context(debug_context*, image_id,
	debug_symbol_lookup_context**);
void debug_delete_symbol_lookup_context(debug_symbol_lookup_context*);
status_t debug_lookup_symbol_address(debug_symbol_lookup_context*,
	const void*, void**, char*, int32, char*, int32, bool*);

#define MAX_THREADS 1024

static const char*
state_name(thread_state state)
{
	switch (state) {
		case B_THREAD_RUNNING:		return "running";
		case B_THREAD_READY:		return "ready";
		case B_THREAD_RECEIVING:	return "receiving";
		case B_THREAD_ASLEEP:		return "asleep";
		case B_THREAD_SUSPENDED:	return "suspended";
		case B_THREAD_WAITING:		return "waiting";
		default:					return "?";
	}
}


static void
print_address(debug_symbol_lookup_context* lookup, const char* tag,
	void* address)
{
	char symbol[256] = "";
	char image[B_PATH_NAME_LENGTH] = "";
	void* base = NULL;
	bool exact = false;
	if (lookup != NULL && debug_lookup_symbol_address(lookup, address, &base,
			symbol, sizeof(symbol), image, sizeof(image), &exact) == B_OK) {
		const char* imageBase = strrchr(image, '/');
		printf("    %s %p  %s + 0x%lx  (%s)\n", tag, address,
			symbol[0] != '\0' ? symbol : "?",
			(unsigned long)((char*)address - (char*)base),
			imageBase != NULL ? imageBase + 1 : image);
	} else
		printf("    %s %p  ?\n", tag, address);
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <team-id> [max-frames]\n", argv[0]);
		return 2;
	}
	team_id team = atoi(argv[1]);
	int maxFrames = argc > 2 ? atoi(argv[2]) : 40;

	static thread_info threads[MAX_THREADS];
	int count = 0;
	int32 cookie = 0;
	while (count < MAX_THREADS
		&& get_next_thread_info(team, &cookie, &threads[count]) == B_OK)
		count++;

	printf("team %d: %d threads (state before attaching)\n", (int)team, count);
	for (int i = 0; i < count; i++) {
		char semName[B_OS_NAME_LENGTH] = "";
		sem_info semInfo;
		if (threads[i].sem >= 0 && get_sem_info(threads[i].sem, &semInfo) == B_OK)
			strlcpy(semName, semInfo.name, sizeof(semName));
		printf("  %6d %-32s %-9s sem %d %s\n", (int)threads[i].thread,
			threads[i].name, state_name(threads[i].state),
			(int)threads[i].sem, semName);
	}

	port_id debuggerPort = create_port(64, "bt918 debugger");
	port_id nubPort = install_team_debugger(team, debuggerPort);
	if (nubPort < 0) {
		fprintf(stderr, "install_team_debugger: %s\n", strerror(nubPort));
		return 1;
	}
	debug_context context;
	status_t error = init_debug_context(&context, team, nubPort);
	if (error != B_OK) {
		fprintf(stderr, "init_debug_context: %s\n", strerror(error));
		return 1;
	}

	static bool stopped[MAX_THREADS];
	for (int i = 0; i < count; i++)
		debug_thread(threads[i].thread);

	/* Collect the stop notifications. Every debugger message starts with a
	 * debug_origin, and every synchronous one means its thread is stopped. */
	int stoppedCount = 0;
	while (stoppedCount < count) {
		int32 code;
		debug_debugger_message_data message;
		ssize_t size = read_port_etc(debuggerPort, &code, &message,
			sizeof(message), B_RELATIVE_TIMEOUT, 3000000);
		if (size < 0)
			break;
		for (int i = 0; i < count; i++) {
			if (threads[i].thread == message.origin.thread && !stopped[i]) {
				stopped[i] = true;
				stoppedCount++;
			}
		}
	}

	debug_symbol_lookup_context* lookup = NULL;
	error = debug_create_symbol_lookup_context(&context, -1, &lookup);
	if (error != B_OK) {
		fprintf(stderr, "symbol lookup: %s\n", strerror(error));
		lookup = NULL;
	}

	for (int i = 0; i < count; i++) {
		printf("\nthread %d \"%s\" (was %s)\n", (int)threads[i].thread,
			threads[i].name, state_name(threads[i].state));
		if (!stopped[i]) {
			printf("    did not stop within 3 s (uninterruptible wait)\n");
			continue;
		}
		debug_cpu_state cpu;
		error = debug_get_cpu_state(&context, threads[i].thread, NULL, &cpu);
		if (error != B_OK) {
			printf("    get_cpu_state: %s\n", strerror(error));
			continue;
		}
		print_address(lookup, "pc", (void*)cpu.elr);
		print_address(lookup, "lr", (void*)cpu.lr);
		void* frame = (void*)cpu.x[29];
		for (int f = 0; f < maxFrames && frame != NULL; f++) {
			void* pair[2];
			if (debug_read_memory(&context, frame, pair, sizeof(pair))
					!= sizeof(pair))
				break;
			if (pair[1] == NULL)
				break;
			print_address(lookup, "  ", pair[1]);
			if ((char*)pair[0] <= (char*)frame)
				break;
			frame = pair[0];
		}
		printf("    x0 %#lx x1 %#lx x2 %#lx\n", cpu.x[0], cpu.x[1], cpu.x[2]);
	}

	for (int i = 0; i < count; i++) {
		if (!stopped[i])
			continue;
		debug_nub_continue_thread message;
		message.thread = threads[i].thread;
		message.handle_event = B_THREAD_DEBUG_HANDLE_EVENT;
		message.single_step = false;
		write_port(nubPort, B_DEBUG_MESSAGE_CONTINUE_THREAD, &message,
			sizeof(message));
	}
	if (lookup != NULL)
		debug_delete_symbol_lookup_context(lookup);
	destroy_debug_context(&context);
	remove_team_debugger(team);
	return 0;
}
