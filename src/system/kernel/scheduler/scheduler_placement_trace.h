/*
 * Copyright 2026, Haiku/DeBeOS Graviton work.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_PLACEMENT_TRACE_H
#define KERNEL_SCHEDULER_PLACEMENT_TRACE_H


#include <SupportDefs.h>


/*!	Temporary instrumentation to settle ONE question: is the SMP imbalance
	created when a thread is placed, or by a later failure to rebalance? The two
	have entirely different fixes and the efficiency ladder cannot tell them
	apart, because either one alone reads as 1.000.

	THIS IS NOT FOR MERGE. It exists to make one expensive image bake yield the
	fact that decides the shape of the fix.

	Why a ring buffer and not dprintf: dprintf() reaches the serial console
	through arch_debug_serial_puts(), which writes the UART one character at a
	time, synchronously (arch/arm64/arch_debug_console.cpp:70). One line is
	milliseconds. The defect under investigation DISAPPEARS once thread spawns
	are staggered by about 1 ms, so a dprintf in choose_core() would erase the
	very phenomenon it is meant to observe -- a textbook Heisenbug. Recording is
	therefore a handful of stores plus one atomic_add, with all formatting and
	all I/O deferred until after the burst is over.
*/

#define SCHEDULER_TRACE_PLACEMENT 1


namespace Scheduler {


enum placement_event {
	// choose_core() returned a core from the package idle-core list.
	PLACEMENT_IDLE_CORE			= 0,
	// choose_core() fell through to gCoreLoadHeap.PeekMinimum().
	PLACEMENT_LOAD_HEAP			= 1,
	// choose_core() fell through again to gCoreHighLoadHeap.PeekMinimum().
	PLACEMENT_HIGH_LOAD_HEAP	= 2,
	// rebalance() actually returned a different core, i.e. a real migration.
	PLACEMENT_MIGRATE			= 3,
};


#if SCHEDULER_TRACE_PLACEMENT

void trace_placement_init();

/*!	Called from the scheduler with locks held. Must not allocate, block or do
	I/O, and must be cheap enough not to perturb a sub-millisecond spawn burst.
*/
void trace_placement(placement_event event, int32 threadID, int32 coreID,
	int32 coreLoad, int32 aux);

/*!	rebalance() was consulted and declined to move the thread. Counted rather
	than recorded: this happens thousands of times a second and would swamp the
	ring buffer, while the count alone answers "was rebalance() even live?".
*/
void trace_placement_decline(bool sawIdlerCore);

/*!	Format and dprintf everything recorded so far, then reset. Must be called
	from ordinary thread context -- never from the scheduler -- because it does
	blocking serial I/O.
*/
void trace_placement_dump();

#else

static inline void trace_placement_init() {}
static inline void trace_placement(placement_event, int32, int32, int32, int32)
	{}
static inline void trace_placement_decline(bool) {}
static inline void trace_placement_dump() {}

#endif	// SCHEDULER_TRACE_PLACEMENT


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_PLACEMENT_TRACE_H
