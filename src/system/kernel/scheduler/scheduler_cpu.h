/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_CPU_H
#define KERNEL_SCHEDULER_CPU_H


#include <OS.h>

#include <smp.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/Heap.h>
#include <util/MinMaxHeap.h>

#include <cpufreq.h>

#include "RunQueue.h"
#include "scheduler_common.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"


namespace Scheduler {


class DebugDumper;

struct ThreadData;
class ThreadProcessing;

class CPUEntry;
class CoreEntry;
class PackageEntry;

// The run queues. Holds the threads ready to run ordered by priority.
// One queue per schedulable target per core. Additionally, each
// logical processor has its sPinnedRunQueues used for scheduling
// pinned threads.
class ThreadRunQueue : public RunQueue<ThreadData, THREAD_MAX_SET_PRIORITY> {
public:
						void			Dump() const;
};

class CPUEntry : public HeapLinkImpl<CPUEntry, int32> {
public:
										CPUEntry();

						void			Init(int32 id, CoreEntry* core);

	inline				int32			ID() const	{ return fCPUNumber; }
	inline				CoreEntry*		Core() const	{ return fCore; }

						void			Start();
						void			Stop();

	inline				void			EnterScheduler();
	inline				void			ExitScheduler();

	inline				void			LockScheduler();
	inline				void			UnlockScheduler();

	inline				void			LockRunQueue();
	inline				void			UnlockRunQueue();

						void			PushFront(ThreadData* thread,
											int32 priority);
						void			PushBack(ThreadData* thread,
											int32 priority);
						void			Remove(ThreadData* thread);
						ThreadData*		PeekThread() const;
						ThreadData*		PeekIdleThread() const;

						void			UpdatePriority(int32 priority);

	inline				int32			GetLoad() const	{ return fLoad; }
						void			ComputeLoad();

						ThreadData*		ChooseNextThread(ThreadData* oldThread,
											bool putAtBack);

						void			TrackActivity(ThreadData* oldThreadData,
											ThreadData* nextThreadData);

						void			StartQuantumTimer(ThreadData* thread,
											bool wasPreempted);

	static inline		CPUEntry*		GetCPU(int32 cpu);

private:
						void			_RequestPerformanceLevel(
											ThreadData* threadData);

	static				int32			_RescheduleEvent(timer* /* unused */);
	static				int32			_UpdateLoadEvent(timer* /* unused */);

						int32			fCPUNumber;
						CoreEntry*		fCore;

						rw_spinlock 	fSchedulerModeLock;

						ThreadRunQueue	fRunQueue;
						spinlock		fQueueLock;

						int32			fLoad;

						bigtime_t		fMeasureActiveTime;
						bigtime_t		fMeasureTime;

						bool			fUpdateLoadEvent;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

class CPUPriorityHeap : public Heap<CPUEntry, int32> {
public:
										CPUPriorityHeap() { }
										CPUPriorityHeap(int32 cpuCount);

						void			Dump();
};

class CoreEntry : public MinMaxHeapLinkImpl<CoreEntry, int32>,
	public DoublyLinkedListLinkImpl<CoreEntry> {
public:
										CoreEntry();

						void			Init(int32 id, PackageEntry* package);

	inline				int32			ID() const	{ return fCoreID; }
	inline				PackageEntry*	Package() const	{ return fPackage; }
	inline				int32			CPUCount() const
											{ return fCPUCount; }
	inline				const CPUSet&	CPUMask() const
											{ return fCPUSet; }

	inline				void			LockCPUHeap();
	inline				void			UnlockCPUHeap();

	inline				CPUPriorityHeap*	CPUHeap();

	inline				int32			ThreadCount() const;

	inline				void			LockRunQueue();
	inline				void			UnlockRunQueue();

						void			PushFront(ThreadData* thread,
											int32 priority);
						void			PushBack(ThreadData* thread,
											int32 priority);
						void			Remove(ThreadData* thread);
						ThreadData*		PeekThread() const;

	inline				bigtime_t		GetActiveTime() const;
	inline				void			IncreaseActiveTime(
											bigtime_t activeTime);

	inline				int32			GetLoad() const;
	inline				int32			GetUnclampedLoad() const;
	inline				uint32			LoadMeasurementEpoch() const
											{ return fLoadMeasurementEpoch; }

	inline				void			AddLoad(int32 load, uint32 epoch,
											bool updateLoad);
	inline				uint32			RemoveLoad(int32 load, bool force);
	inline				void			ChangeLoad(int32 delta);

	inline				void			CPUGoesIdle(CPUEntry* cpu);
	inline				void			CPUWakesUp(CPUEntry* cpu);

						void			AddCPU(CPUEntry* cpu);
						void			RemoveCPU(CPUEntry* cpu,
											ThreadProcessing&
												threadPostProcessing);

	static inline		CoreEntry*		GetCore(int32 cpu);

private:
						void			_UpdateLoad(bool forceUpdate = false);

	static				void			_UnassignThread(Thread* thread,
											void* core);

						int32			fCoreID;
						PackageEntry*	fPackage;

						int32			fCPUCount;
						CPUSet			fCPUSet;
						int32			fIdleCPUCount;
						CPUPriorityHeap	fCPUHeap;
						spinlock		fCPULock;

						int32			fThreadCount;
						ThreadRunQueue	fRunQueue;
						spinlock		fQueueLock;

						bigtime_t		fActiveTime;
	mutable				seqlock			fActiveTimeLock;

						int32			fLoad;
						int32			fCurrentLoad;
						uint32			fLoadMeasurementEpoch;
						bool			fHighLoad;
						bigtime_t		fLastLoadUpdate;
						rw_spinlock		fLoadLock;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

class CoreLoadHeap : public MinMaxHeap<CoreEntry, int32> {
public:
										CoreLoadHeap() { }
										CoreLoadHeap(int32 coreCount);

						void			Dump();

	// Least/most loaded core whose CPU mask permits the caller, or NULL if the
	// heap holds no permitted core. Pass mask == NULL for "no constraint", in
	// which case these degenerate to PeekMinimum()/PeekMaximum().
	//
	// These exist because the heap cannot answer "index-th extremum" cheaply:
	// its arrays are heaps, so only slot 0 is an extremum. Both mode
	// implementations used to walk PeekMinimum(index++) until the mask matched,
	// which enumerated the heap in array order and therefore returned the FIRST
	// permitted core rather than the LEAST LOADED one. Unconstrained threads
	// were unaffected (the walk exits on its first iteration, at the true
	// extremum); only threads with an affinity mask were mis-placed. Doing the
	// selection here keeps the scan O(core count) and the semantics honest.
	inline			CoreEntry*		PeekLeastLoaded(const CPUSet* mask) const;
	inline			CoreEntry*		PeekMostLoaded(const CPUSet* mask) const;
};

// gPackageEntries are used to decide which core should be woken up from the
// idle state. When aiming for performance we should use as many packages as
// possible with as little cores active in each package as possible (so that the
// package can enter any boost mode if it has one and the active core have more
// of the shared cache for themselves. If power saving is the main priority we
// should keep active cores on as little packages as possible (so that other
// packages can go to the deep state of sleep). The heap stores only packages
// with at least one core active and one core idle. The packages with all cores
// idle are stored in gPackageIdleList (in LIFO manner).
class PackageEntry : public DoublyLinkedListLinkImpl<PackageEntry> {
public:
											PackageEntry();

						void				Init(int32 id);

	inline				void				CoreGoesIdle(CoreEntry* core);
	inline				void				CoreWakesUp(CoreEntry* core);

	inline				CoreEntry*			GetIdleCore(int32 index = 0) const;
	inline				CoreEntry*			GetLeastClaimedIdleCore(
											const CPUSet* mask = NULL) const;

						void				AddIdleCore(CoreEntry* core);
						void				RemoveIdleCore(CoreEntry* core);

	static inline		PackageEntry*		GetMostIdlePackage();
	static inline		PackageEntry*		GetLeastIdlePackage();

private:
						int32				fPackageID;

						DoublyLinkedList<CoreEntry>	fIdleCores;
						int32				fIdleCoreCount;
						int32				fCoreCount;
						rw_spinlock			fCoreLock;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;
typedef DoublyLinkedList<PackageEntry> IdlePackageList;

extern CPUEntry* gCPUEntries;

extern CoreEntry* gCoreEntries;
extern CoreLoadHeap gCoreLoadHeap;
extern CoreLoadHeap gCoreHighLoadHeap;
extern rw_spinlock gCoreHeapsLock;
extern int32 gCoreCount;

extern PackageEntry* gPackageEntries;
extern IdlePackageList gIdlePackageList;
extern rw_spinlock gIdlePackageLock;
extern int32 gPackageCount;


inline void
CPUEntry::EnterScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_read_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::ExitScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	release_read_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::LockScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_write_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::UnlockScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	release_write_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::LockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fQueueLock);
}


inline void
CPUEntry::UnlockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fQueueLock);
}


/* static */ inline CPUEntry*
CPUEntry::GetCPU(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	return &gCPUEntries[cpu];
}


inline void
CoreEntry::LockCPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fCPULock);
}


inline void
CoreEntry::UnlockCPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fCPULock);
}


inline CPUPriorityHeap*
CoreEntry::CPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	return &fCPUHeap;
}


inline int32
CoreEntry::ThreadCount() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fThreadCount + fCPUCount - fIdleCPUCount;
}


inline void
CoreEntry::LockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fQueueLock);
}


inline void
CoreEntry::UnlockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fQueueLock);
}


inline void
CoreEntry::IncreaseActiveTime(bigtime_t activeTime)
{
	SCHEDULER_ENTER_FUNCTION();
	WriteSequentialLocker _(fActiveTimeLock);
	fActiveTime += activeTime;
}


inline bigtime_t
CoreEntry::GetActiveTime() const
{
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t activeTime;
	uint32 count;
	do {
		count = acquire_read_seqlock(&fActiveTimeLock);
		activeTime = fActiveTime;
	} while (!release_read_seqlock(&fActiveTimeLock, count));
	return activeTime;
}


inline int32
CoreEntry::GetLoad() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fCPUCount > 0);
	return std::min(fLoad / fCPUCount, kMaxLoad);
}


/*!	The same quantity as GetLoad(), demand per logical CPU, but without the clamp
	at kMaxLoad -- so an oversubscribed core reports 2000 rather than 1000 and is
	distinguishable from a merely saturated one.

	fLoad is the running sum of the fNeededLoad of the threads assigned to this
	core, and fNeededLoad is DEMAND, not supply: ThreadData's available-time
	accounting excludes time spent runnable in the run queue, so a CPU-bound
	thread reports kMaxLoad however little CPU it actually gets. The sum is
	therefore genuinely unbounded and meaningful above kMaxLoad, and the clamp in
	GetLoad() is what makes oversubscription invisible.

	Use this ONLY in the rebalance predicates. GetLoad() keeps its clamp
	deliberately, because its other consumers need a bounded ratio: in particular
	CPUEntry::_RequestPerformanceLevel() feeds it to the cpufreq interface behind
	an ASSERT_PRINT(load <= kMaxLoad), and KDEBUG is on in the checked-in build,
	so widening GetLoad() itself would be a live panic() on any machine that has a
	cpufreq module -- i.e. x86, where it could not be tested from here. The
	core load heap keys and the kHighLoad/kMediumLoad band decision also read
	GetLoad() and must keep their present meaning.
*/
inline int32
CoreEntry::GetUnclampedLoad() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(fCPUCount > 0);
	return fLoad / fCPUCount;
}


inline void
CoreEntry::AddLoad(int32 load, uint32 epoch, bool updateLoad)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	ReadSpinLocker locker(fLoadLock);
	atomic_add(&fCurrentLoad, load);
	if (fLoadMeasurementEpoch != epoch)
		atomic_add(&fLoad, load);
	locker.Unlock();

	if (updateLoad)
		_UpdateLoad(true);
}


inline uint32
CoreEntry::RemoveLoad(int32 load, bool force)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	ReadSpinLocker locker(fLoadLock);
	atomic_add(&fCurrentLoad, -load);
	if (force) {
		atomic_add(&fLoad, -load);
		locker.Unlock();

		_UpdateLoad(true);
	}
	return fLoadMeasurementEpoch;
}


inline void
CoreEntry::ChangeLoad(int32 delta)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(delta >= -kMaxLoad && delta <= kMaxLoad);

	if (delta != 0) {
		ReadSpinLocker locker(fLoadLock);
		atomic_add(&fCurrentLoad, delta);
		atomic_add(&fLoad, delta);
	}

	_UpdateLoad();
}


/* PackageEntry::CoreGoesIdle and PackageEntry::CoreWakesUp have to be defined
   before CoreEntry::CPUGoesIdle and CoreEntry::CPUWakesUp. If they weren't
   GCC2 wouldn't inline them as, apparently, it doesn't do enough optimization
   passes.
*/
inline void
PackageEntry::CoreGoesIdle(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();

	WriteSpinLocker _(fCoreLock);

	ASSERT(fIdleCoreCount >= 0);
	ASSERT(fIdleCoreCount < fCoreCount);

	fIdleCoreCount++;
	fIdleCores.Add(core);

	if (fIdleCoreCount == fCoreCount) {
		// package goes idle
		WriteSpinLocker _(gIdlePackageLock);
		gIdlePackageList.Add(this);
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();

	WriteSpinLocker _(fCoreLock);

	ASSERT(fIdleCoreCount > 0);
	ASSERT(fIdleCoreCount <= fCoreCount);

	fIdleCoreCount--;
	fIdleCores.Remove(core);

	if (fIdleCoreCount + 1 == fCoreCount) {
		// package wakes up
		WriteSpinLocker _(gIdlePackageLock);
		gIdlePackageList.Remove(this);
	}
}


inline void
CoreEntry::CPUGoesIdle(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;

	ASSERT(fIdleCPUCount < fCPUCount);
	if (++fIdleCPUCount == fCPUCount)
		fPackage->CoreGoesIdle(this);
}


inline void
CoreEntry::CPUWakesUp(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;

	ASSERT(fIdleCPUCount > 0);
	if (fIdleCPUCount-- == fCPUCount)
		fPackage->CoreWakesUp(this);
}


/* static */ inline CoreEntry*
CoreEntry::GetCore(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	return gCPUEntries[cpu].Core();
}


inline CoreEntry*
PackageEntry::GetIdleCore(int32 index) const
{
	SCHEDULER_ENTER_FUNCTION();
	CoreEntry* element = fIdleCores.Last();
	for (int32 i = 0; element != NULL && i < index; i++)
		element = fIdleCores.GetPrevious(element);

	return element;
}


/*!	Returns the idle core with the fewest threads already assigned to it, or NULL
	if this package has no idle core matching \a mask (NULL matches any).

	choose_core() must not simply take GetIdleCore(0). A core leaves fIdleCores
	only when one of its CPUs actually reschedules onto a thread -- CPUWakesUp(),
	reached from CPUEntry::UpdatePriority() -- but enqueue() merely asks that CPU
	to reschedule, with an asynchronous ICI. For the length of that lag the core
	is still advertised as idle, and because CoreGoesIdle() appends while
	GetIdleCore(0) returns fIdleCores.Last(), a burst of placements is handed the
	SAME core over and over. Measured on a 16-CPU Graviton: eight threads spawned
	back to back land on seven cores, leaving one core doubled and one idle, and
	separating the spawns by as little as 5 us -- ICI plus reschedule latency --
	makes it correct in 40 runs out of 40.

	ThreadCount() is the tie-breaker because it is the only measure here with no
	lag: CoreEntry::PushBack() does atomic_add(&fThreadCount, 1) from within
	ThreadData::Enqueue(), which completes before the next placement's
	choose_core() call, so a core claimed a moment ago already reports 1. Core
	load cannot serve that purpose -- a new thread inherits its parent's
	fNeededLoad (ThreadData::Init()), which is ~0 for a parent that is about to
	block, so placing a thread need not move the core's load at all.

	Note this walks from Last(), so a genuinely free core is returned on the first
	iteration and the existing LIFO preference (and its cache locality) is kept
	intact; the walk only continues when the head of the list is already claimed.
*/
inline CoreEntry*
PackageEntry::GetLeastClaimedIdleCore(const CPUSet* mask) const
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* best = NULL;
	int32 bestCount = 0;

	for (CoreEntry* core = fIdleCores.Last(); core != NULL;
			core = fIdleCores.GetPrevious(core)) {
		if (mask != NULL && !core->CPUMask().Matches(*mask))
			continue;

		int32 count = core->ThreadCount();
		if (count <= 0)
			return core;

		if (best == NULL || count < bestCount) {
			best = core;
			bestCount = count;
		}
	}

	return best;
}


/*!	Least loaded core in this heap that the given mask permits, or NULL.

	Selects on the key rather than trusting position, because only slot 0 of a
	heap array is an extremum -- see MinMaxHeap::PeekUnordered(). The fast path
	is unchanged for the common unconstrained case: with mask == NULL this is
	PeekMinimum() and touches one element.

	Callers hold gCoreHeapsLock (read is enough); this neither takes nor drops it.
*/
inline CoreEntry*
CoreLoadHeap::PeekLeastLoaded(const CPUSet* mask) const
{
	SCHEDULER_ENTER_FUNCTION();

	if (mask == NULL)
		return PeekMinimum();

	CoreEntry* best = NULL;
	int32 bestLoad = 0;

	const int32 count = CountElements();
	for (int32 i = 0; i < count; i++) {
		CoreEntry* core = PeekUnordered(i);
		if (core == NULL)
			break;
		if (!core->CPUMask().Matches(*mask))
			continue;

		const int32 load = GetKey(core);
		if (best == NULL || load < bestLoad) {
			best = core;
			bestLoad = load;
		}
	}

	return best;
}


/*!	Most loaded core in this heap that the given mask permits, or NULL.

	The PeekLeastLoaded() counterpart; see it for why the key is compared
	explicitly instead of indexing the heap array.
*/
inline CoreEntry*
CoreLoadHeap::PeekMostLoaded(const CPUSet* mask) const
{
	SCHEDULER_ENTER_FUNCTION();

	if (mask == NULL)
		return PeekMaximum();

	CoreEntry* best = NULL;
	int32 bestLoad = 0;

	const int32 count = CountElements();
	for (int32 i = 0; i < count; i++) {
		CoreEntry* core = PeekUnordered(i);
		if (core == NULL)
			break;
		if (!core->CPUMask().Matches(*mask))
			continue;

		const int32 load = GetKey(core);
		if (best == NULL || load > bestLoad) {
			best = core;
			bestLoad = load;
		}
	}

	return best;
}


/* static */ inline PackageEntry*
PackageEntry::GetMostIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* current = &gPackageEntries[0];
	for (int32 i = 1; i < gPackageCount; i++) {
		if (gPackageEntries[i].fIdleCoreCount > current->fIdleCoreCount)
			current = &gPackageEntries[i];
	}

	if (current->fIdleCoreCount == 0)
		return NULL;

	return current;
}


/* static */ inline PackageEntry*
PackageEntry::GetLeastIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();

	PackageEntry* package = NULL;

	for (int32 i = 0; i < gPackageCount; i++) {
		PackageEntry* current = &gPackageEntries[i];

		int32 currentIdleCoreCount = current->fIdleCoreCount;
		if (currentIdleCoreCount != 0 && (package == NULL
				|| currentIdleCoreCount < package->fIdleCoreCount)) {
			package = current;
		}
	}

	return package;
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_CPU_H

