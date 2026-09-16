/*
 * Copyright 2026, Haiku/DeBeOS Graviton work.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_placement_trace.h"

#include <OS.h>

#include <debug.h>
#include <KernelExport.h>
#include <util/atomic.h>


#ifdef SCHEDULER_TRACE_PLACEMENT

namespace Scheduler {


// A whole smpscale run places at most a few dozen threads, so this never wraps
// in the experiment it was written for. It wraps rather than stops so that a
// runaway does not hide the beginning of the burst.
#define kPlacementTraceEntries	512


struct placement_entry {
	bigtime_t	time;
	int32		event;
	int32		threadID;
	int32		coreID;
	int32		coreLoad;
	int32		aux;
};


static placement_entry sEntries[kPlacementTraceEntries];
static int32 sNextEntry;
static int32 sDeclines;
static int32 sDeclinesPeakReducible;
static int32 sMigrations;


static int
dump_placement_trace(int /* argc */, char** /* argv */)
{
	trace_placement_dump();
	return 0;
}


void
trace_placement_init()
{
	add_debugger_command_etc("sched_placement", &dump_placement_trace,
		"Dump the scheduler placement trace",
		"\nDump where choose_core() placed each thread and how often\n"
		"rebalance() declined to move one.\n", 0);
}


void
trace_placement(placement_event event, int32 threadID, int32 coreID,
	int32 coreLoad, int32 aux)
{
	if (event == PLACEMENT_MIGRATE)
		atomic_add(&sMigrations, 1);

	// Claim a slot without a lock. Wrapping is deliberate; see above.
	int32 index = atomic_add(&sNextEntry, 1);
	if (index < 0)
		return;
	placement_entry& entry = sEntries[index % kPlacementTraceEntries];

	entry.time = system_time();
	entry.event = event;
	entry.threadID = threadID;
	entry.coreID = coreID;
	entry.coreLoad = coreLoad;
	entry.aux = aux;
}


void
trace_placement_decline(bool peakReducible)
{
	atomic_add(&sDeclines, 1);
	// peakReducible is true only when migrating the thread would have moved the
	// destination core strictly below the source's current load -- an actual
	// reduction of the pair's peak, not merely "a less loaded core existed". The
	// latter is true of every busy core under oversubscription; see #115 and the
	// argument in low_latency.cpp's rebalance().
	if (peakReducible)
		atomic_add(&sDeclinesPeakReducible, 1);
}


void
trace_placement_dump()
{
	int32 total = atomic_get_and_set(&sNextEntry, 0);
	int32 declines = atomic_get_and_set(&sDeclines, 0);
	int32 peakReducible = atomic_get_and_set(&sDeclinesPeakReducible, 0);
	int32 migrations = atomic_get_and_set(&sMigrations, 0);

	static const char* const kEventNames[] = {
		"idle-core-list", "load-heap", "high-load-heap", "MIGRATE"
	};

	dprintf("sched_placement: %" B_PRId32 " events, %" B_PRId32 " migrations, "
		"%" B_PRId32 " rebalance declines (%" B_PRId32 " of them where migrating "
		"would have lowered the pair's peak load)\n", total, migrations, declines,
		peakReducible);

	if (total == 0)
		return;

	int32 count = total;
	int32 first = 0;
	if (count > kPlacementTraceEntries) {
		first = count - kPlacementTraceEntries;
		count = kPlacementTraceEntries;
		dprintf("sched_placement: wrapped, showing the last %d\n",
			kPlacementTraceEntries);
	}

	dprintf("sched_placement: %10s %6s %5s %6s %8s %s\n",
		"time_us", "thread", "core", "load", "aux", "path");
	for (int32 i = 0; i < count; i++) {
		placement_entry& entry = sEntries[(first + i) % kPlacementTraceEntries];
		const char* name = entry.event >= 0 && entry.event <= PLACEMENT_MIGRATE
			? kEventNames[entry.event] : "?";
		// Relative to the first shown event: the absolute value is noise, the
		// spacing is the whole point.
		dprintf("sched_placement: %10" B_PRId64 " %6" B_PRId32 " %5" B_PRId32
			" %6" B_PRId32 " %8" B_PRId32 " %s\n",
			entry.time - sEntries[first % kPlacementTraceEntries].time,
			entry.threadID, entry.coreID, entry.coreLoad, entry.aux, name);
	}
}


}	// namespace Scheduler

#endif	// SCHEDULER_TRACE_PLACEMENT
