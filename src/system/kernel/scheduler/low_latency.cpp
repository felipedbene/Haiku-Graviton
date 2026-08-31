/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_placement_trace.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


const bigtime_t kCacheExpire = 100000;


static void
switch_to_mode()
{
}


static void
set_cpu_enabled(int32 /* cpu */, bool /* enabled */)
{
}


static bool
has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData->WentSleepActive() == 0)
		return false;
	CoreEntry* core = threadData->Core();
	bigtime_t activeTime = core->GetActiveTime();
	return activeTime - threadData->WentSleepActive() > kCacheExpire;
}


static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	// wake new package
	PackageEntry* package = gIdlePackageList.Last();
	if (package == NULL) {
		// wake new core
		package = PackageEntry::GetMostIdlePackage();
	}

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	placement_event event = PLACEMENT_IDLE_CORE;
	CoreEntry* core = NULL;
	if (package != NULL) {
		// Not GetIdleCore(0): during a burst that keeps returning the same core,
		// because a core is only removed from the idle list once its CPU actually
		// reschedules. See PackageEntry::GetLeastClaimedIdleCore().
		core = package->GetLeastClaimedIdleCore(useMask ? &mask : NULL);
	}
	if (core == NULL) {
		ReadSpinLocker coreLocker(gCoreHeapsLock);
		// no idle cores, use least occupied core
		core = gCoreLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
		event = PLACEMENT_LOAD_HEAP;
		if (core == NULL) {
			core = gCoreHighLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
			event = PLACEMENT_HIGH_LOAD_HEAP;
		}
	}

	ASSERT(core != NULL);

	// Which of the three paths placed the thread, and on which core, is the one
	// fact that decides whether the fix belongs in placement or in rebalancing.
	trace_placement(event, threadData->GetThread()->id, core->ID(),
		core->GetLoad(), threadData->GetLoad());

	return core;
}


static CoreEntry*
rebalance(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* core = threadData->Core();
	ASSERT(core != NULL);

	// Get the least loaded core.
	ReadSpinLocker coreLocker(gCoreHeapsLock);
	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* other = gCoreLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
	if (other != NULL && useMask && other->CPUMask().IsEmpty())
		panic("other->CPUMask().IsEmpty()\n");

	if (other == NULL)
		other = gCoreHighLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
	coreLocker.Unlock();
	ASSERT(other != NULL);

	// Check if the least loaded core is significantly less loaded than
	// the current one.
	//
	// Unclamped, or this test cannot pass. GetLoad() saturates at kMaxLoad, so
	// coreLoad <= kMaxLoad and otherLoad >= 0 bound the difference below at
	// kMaxLoad - kLoadDifference == 0.8 * kMaxLoad, while threadLoad for a
	// CPU-bound thread on a non-SMT core is kMaxLoad. 0.8 * kMaxLoad >= kMaxLoad
	// is false for every value of kMaxLoad: ANY thread above 80% duty could never
	// be migrated, however idle the machine. It only works on x86 because SMT
	// makes CPUCount() 2 and halves threadLoad below.
	int32 coreLoad = core->GetUnclampedLoad();
	int32 otherLoad = other->GetUnclampedLoad();
	if (other == core || otherLoad + kLoadDifference >= coreLoad) {
		// Record whether a genuinely less loaded core existed at this moment.
		// If declines happen in their thousands while such a core exists, the
		// migration predicate is the problem; if they happen while every core
		// looks identically loaded, the load metric is.
		trace_placement_decline(other != core && otherLoad < coreLoad);
		return core;
	}

	// Check whether migrating the current thread would result in both core
	// loads become closer to the average.
	int32 difference = coreLoad - otherLoad - kLoadDifference;
	ASSERT(difference > 0);

	int32 threadLoad = threadData->GetLoad() / core->CPUCount();
	if (difference < threadLoad) {
		trace_placement_decline(true);
		return core;
	}

	trace_placement(PLACEMENT_MIGRATE, threadData->GetThread()->id,
		other->ID(), coreLoad, otherLoad);
	return other;
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle)
		return;

	// Architecture can't route IRQs to a chosen CPU: rebalancing is pure
	// irqs_lock churn that re-parents nothing. Stop once that is known.
	if (!interrupt_affinity_supported())
		return;

	cpu_ent* cpu = get_cpu_struct();
	SpinLocker locker(cpu->irqs_lock);

	irq_assignment* chosen = NULL;
	irq_assignment* irq = cpu->irqs.First();

	int32 totalLoad = 0;
	while (irq != NULL) {
		if (chosen == NULL || chosen->load < irq->load)
			chosen = irq;
		totalLoad += irq->load;
		irq = cpu->irqs.GetNext(irq);
	}

	locker.Unlock();

	if (chosen == NULL || totalLoad < kLowLoad)
		return;

	ReadSpinLocker coreLocker(gCoreHeapsLock);
	CoreEntry* other = gCoreLoadHeap.PeekMinimum();
	if (other == NULL)
		other = gCoreHighLoadHeap.PeekMinimum();
	coreLocker.Unlock();

	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();

	ASSERT(other != NULL);

	CoreEntry* core = CoreEntry::GetCore(cpu->cpu_num);
	if (other == core)
		return;
	if (other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	assign_io_interrupt_to_cpu(chosen->irq, newCPU);
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",

	1000,
	100,
	{ 2, 5 },

	5000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};

