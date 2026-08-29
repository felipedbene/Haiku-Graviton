/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Host-only crash-injection scaffolding for the BFS mount-time large auto-grow.
 * See GrowFault.h. Uses raw host libc only; must NOT include the fs_shell API
 * wrapper (it remaps dprintf/sync/... and would collide with libc).
 */

#include "GrowFault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


// write-log record kinds
#define GROW_LOG_WRITE		1
#define GROW_LOG_BARRIER	2


static FILE* sWriteLog = NULL;
static const char* sAbortAt = NULL;
static int sInitialized = 0;


static void
ensure_init()
{
	if (sInitialized)
		return;
	sInitialized = 1;
	sAbortAt = getenv("BFS_GROW_ABORT");
	const char* path = getenv("BFS_GROW_WRITELOG");
	if (path != NULL)
		sWriteLog = fopen(path, "wb");
}


extern "C" void
bfs_grow_fault_log_write(long long offset, const void* buffer,
	unsigned int length)
{
	ensure_init();
	if (sWriteLog == NULL)
		return;
	unsigned char kind = GROW_LOG_WRITE;
	unsigned long long off = (unsigned long long)offset;
	unsigned int len = length;
	fwrite(&kind, 1, 1, sWriteLog);
	fwrite(&off, sizeof(off), 1, sWriteLog);
	fwrite(&len, sizeof(len), 1, sWriteLog);
	fwrite(buffer, 1, length, sWriteLog);
	fflush(sWriteLog);
}


extern "C" void
bfs_grow_fault_checkpoint(const char* label)
{
	ensure_init();
	if (sWriteLog != NULL) {
		unsigned char kind = GROW_LOG_BARRIER;
		unsigned int len = (unsigned int)strlen(label);
		fwrite(&kind, 1, 1, sWriteLog);
		fwrite(&len, sizeof(len), 1, sWriteLog);
		fwrite(label, 1, len, sWriteLog);
		fflush(sWriteLog);
	}
	if (sAbortAt != NULL && strcmp(sAbortAt, label) == 0) {
		if (sWriteLog != NULL)
			fflush(sWriteLog);
		_exit(42);
	}
}


extern "C" int
bfs_grow_fault_abort_is(const char* label)
{
	ensure_init();
	return sAbortAt != NULL && strcmp(sAbortAt, label) == 0;
}
