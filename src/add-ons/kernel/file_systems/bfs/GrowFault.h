/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Host-only (bfs_shell) crash-injection scaffolding for the BFS mount-time
 * large auto-grow. Declarations only -- the implementation (GrowFault.cpp)
 * uses raw host libc and is deliberately kept out of the fs_shell API wrapper
 * translation unit so it can touch the real host filesystem for the write log.
 * Compiled only when BFS_GROW_FAULT_INJECTION is defined (never in the kernel
 * add-on).
 */
#ifndef BFS_GROW_FAULT_H
#define BFS_GROW_FAULT_H


#ifdef __cplusplus
extern "C" {
#endif

// Records a flush-barrier marker in the write log and, if BFS_GROW_ABORT names
// this label, _exit()s to simulate a power loss at that boundary.
void bfs_grow_fault_checkpoint(const char* label);

// True if BFS_GROW_ABORT names this label (used for the mid-bitmap and
// torn-commit synthetic faults).
int bfs_grow_fault_abort_is(const char* label);

// Appends a raw write record (offset, length, bytes) to the write log so an
// external harness can replay arbitrary persisted subsets between barriers.
void bfs_grow_fault_log_write(long long offset, const void* buffer,
	unsigned int length);

#ifdef __cplusplus
}
#endif


#endif	// BFS_GROW_FAULT_H
