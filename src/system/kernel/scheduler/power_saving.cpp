/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include <util/atomic.h>
#include <util/AutoLock.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


const bigtime_t kCacheExpire = 100000;

static CoreEntry* sSmallTaskCore;


static void
switch_to_mode()
{
	sSmallTaskCore = NULL;
}


static void
set_cpu_enabled(int32 cpu, bool enabled)
{
	if (!enabled)
		sSmallTaskCore = NULL;
}


static bool
has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData->WentSleep() == 0)
		return false;
	return system_time() - threadData->WentSleep() > kCacheExpire;
}


static CoreEntry*
choose_small_task_core()
{
	SCHEDULER_ENTER_FUNCTION();

	ReadSpinLocker coreLocker(gCoreHeapsLock);
	CoreEntry* core = gCoreLoadHeap.PeekMaximum();
	if (core == NULL)
		return sSmallTaskCore;

	CoreEntry* smallTaskCore
		= atomic_pointer_test_and_set(&sSmallTaskCore, core, (CoreEntry*)NULL);
	if (smallTaskCore == NULL)
		return core;
	return smallTaskCore;
}


static CoreEntry*
choose_idle_core()
{
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* package = PackageEntry::GetLeastIdlePackage();

	if (package == NULL)
		package = gIdlePackageList.Last();

	if (package != NULL) {
		// Same reasoning as in low_latency's choose_core(): a core stays in the
		// idle list until its CPU reschedules, so taking the head of the list
		// hands a burst the same core repeatedly.
		return package->GetLeastClaimedIdleCore();
	}
	return NULL;
}


static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* core = NULL;

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	// try to pack all threads on one core
	core = choose_small_task_core();
	if (core != NULL && (useMask && !core->CPUMask().Matches(mask)))
		core = NULL;

	if (core == NULL || core->GetLoad() + threadData->GetLoad() >= kHighLoad) {
		ReadSpinLocker coreLocker(gCoreHeapsLock);

		// run immediately on already woken core
		core = gCoreLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
		if (core == NULL) {
			coreLocker.Unlock();

			core = choose_idle_core();
			// choose_idle_core() returns NULL when no idle package/core is
			// available (all cores loaded). Guard the deref like the
			// choose_small_task_core() path above (line 100); the ASSERT below
			// is after this point and is compiled out in release.
			if (core != NULL && (useMask && !core->CPUMask().Matches(mask)))
				core = NULL;

			if (core == NULL) {
				coreLocker.Lock();
				core = gCoreHighLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
			}
		}
	}

	ASSERT(core != NULL);
	return core;
}


static CoreEntry*
rebalance(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gSingleCore);

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* core = threadData->Core();

	// Unclamped: see CoreEntry::GetUnclampedLoad(). Without it an oversubscribed
	// core is indistinguishable from a merely saturated one and none of the tests
	// below can tell that threads are being delayed.
	int32 coreLoad = core->GetUnclampedLoad();
	int32 threadLoad = threadData->GetLoad() / core->CPUCount();
	if (coreLoad > kHighLoad) {
		if (sSmallTaskCore == core) {
			sSmallTaskCore = NULL;
			CoreEntry* smallTaskCore = choose_small_task_core();

			if (threadLoad > coreLoad / 3 || smallTaskCore == NULL
					|| (useMask && !smallTaskCore->CPUMask().Matches(mask))) {
				return core;
			}
			return coreLoad > kVeryHighLoad ? smallTaskCore : core;
		}

		// Packing threads onto few cores is the point of this mode, and the guard
		// below expresses that: if this thread is at least half the core's load,
		// moving it just moves the problem. But that stops being true once the
		// core is oversubscribed. With two saturated threads on a non-SMT core it
		// reads 1000 >= 2000/2, true, so it declines -- and for EXACTLY two
		// threads, the commonest case, that is the wrong answer: moving one to an
		// idle core takes the peak from 2000 to 1000. Pack up to saturation, not
		// past it.
		if (coreLoad <= kMaxLoad && threadLoad >= coreLoad / 2)
			return core;

		ReadSpinLocker coreLocker(gCoreHeapsLock);
		CoreEntry* other = gCoreLoadHeap.PeekMostLoaded(useMask ? &mask : NULL);
		if (other == NULL)
			other = gCoreHighLoadHeap.PeekLeastLoaded(useMask ? &mask : NULL);
		coreLocker.Unlock();
		ASSERT(other != NULL);

		int32 coreNewLoad = coreLoad - threadLoad;
		int32 otherNewLoad = other->GetUnclampedLoad() + threadLoad;
		if (coreLoad > kMaxLoad) {
			// Oversubscribed: make the move whenever it strictly lowers the peak,
			// even if it leaves the two cores equal. The packing criterion below
			// wants this core to stay meaningfully busier than the target
			// afterwards, which rejects precisely the balancing move that unwinds
			// a doubled core -- 2000 and 0 becomes 1000 and 1000, a difference of
			// zero. Still declines when every core is busy, since otherNewLoad
			// then reaches coreLoad and moving only moves the collision.
			return otherNewLoad < coreLoad ? other : core;
		}
		return coreNewLoad - otherNewLoad >= kLoadDifference / 2 ? other : core;
	}

	if (coreLoad >= kMediumLoad)
		return core;

	CoreEntry* smallTaskCore = choose_small_task_core();
	if (smallTaskCore == NULL || (useMask && !smallTaskCore->CPUMask().Matches(mask)))
		return core;
	return smallTaskCore->GetLoad() + threadLoad < kHighLoad
		? smallTaskCore : core;
}


static inline void
pack_irqs()
{
	SCHEDULER_ENTER_FUNCTION();

	// If the architecture can't route IRQs, assign_io_interrupt_to_cpu() is a
	// no-op that leaves each vector on its current CPU. The drain loop below
	// would then never make progress, so bail out early.
	if (!interrupt_affinity_supported())
		return;

	CoreEntry* smallTaskCore = atomic_pointer_get(&sSmallTaskCore);
	if (smallTaskCore == NULL)
		return;

	cpu_ent* cpu = get_cpu_struct();
	CoreEntry* thisCore = CoreEntry::GetCore(cpu->cpu_num);
	if (smallTaskCore == thisCore)
		return;

	// Avoid packing IRQs if it's not really going to change much.
	if (thisCore->GetLoad() >= smallTaskCore->GetLoad()
			|| (smallTaskCore->GetLoad() - thisCore->GetLoad()) < kLoadDifference) {
		return;
	}

	SpinLocker locker(cpu->irqs_lock);
	while (cpu->irqs.First() != NULL) {
		irq_assignment* irq = cpu->irqs.First();
		locker.Unlock();

		int32 newCPU = smallTaskCore->CPUHeap()->PeekRoot()->ID();

		if (newCPU != cpu->cpu_num)
			assign_io_interrupt_to_cpu(irq->irq, newCPU);

		locker.Lock();
	}
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle && sSmallTaskCore != NULL) {
		pack_irqs();
		return;
	}

	if (idle || sSmallTaskCore != NULL)
		return;

	// Architecture can't route IRQs to a chosen CPU: rebalancing is pure
	// irqs_lock churn that re-parents nothing. Stop once that is known.
	if (!interrupt_affinity_supported())
		return;

	cpu_ent* cpu = get_cpu_struct();
	SpinLocker locker(cpu->irqs_lock);

	irq_assignment* chosen = NULL;
	irq_assignment* irq = cpu->irqs.First();

	while (irq != NULL) {
		if (chosen == NULL || chosen->load < irq->load)
			chosen = irq;
		irq = cpu->irqs.GetNext(irq);
	}

	locker.Unlock();

	if (chosen == NULL || chosen->load < kLowLoad)
		return;

	ReadSpinLocker coreLocker(gCoreHeapsLock);
	CoreEntry* other = gCoreLoadHeap.PeekMinimum();
	coreLocker.Unlock();
	if (other == NULL)
		return;
	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();

	CoreEntry* core = CoreEntry::GetCore(smp_get_current_cpu());
	if (other == core)
		return;
	if (other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	assign_io_interrupt_to_cpu(chosen->irq, newCPU);
}


scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",

	2000,
	500,
	{ 3, 10 },

	20000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};

