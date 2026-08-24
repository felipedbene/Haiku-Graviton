/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MODIFIED_PAGE_QUEUE_H
#define MODIFIED_PAGE_QUEUE_H


#include <Referenceable.h>
#include <util/BinarySemaphore.h>

#include "VMPageQueue.h"


// How long a thread dirtying pages will wait for the page writer to get the
// modified queue back under quota before giving up and going over it.
//
// There used to be no bound at all: file_cache.cpp passed timeout 0 with no
// timeout flag, which makes ConditionVariableEntry::Wait() an indefinite wait.
// A writer therefore blocked until the page writer said otherwise, and if the
// writer never caught up it blocked forever -- observed as a machine that
// answered ICMP and accepted TCP connections on :22 while no userland process
// could make progress, with no panic, no KDL and nothing in the syslog.
//
// Five seconds is chosen to be far longer than any healthy wait (normally
// microseconds to milliseconds) so that back-pressure still works, while
// converting "hang forever" into "one slow write". Progress beats a quota that
// is best-effort anyway.
#define PAGES_FLUSH_QUOTA_WAIT_TIMEOUT		(5 * 1000 * 1000)


struct ModifiedPageQueue : public BReferenceable, public VMPageQueue {
public:
	static	int64				GlobalModifiedCount()
									{ return atomic_get64(&sGlobalModifiedCount); }

	virtual						~ModifiedPageQueue();

			status_t			StartWriter(const char* name);
			void				NotifyWriter() { fPageWriterCondition.WakeUp(); }

			bool				IsOverQuota(page_num_t additionalPages = 0);

			// KDL only; see the page_writer_quota debugger command.
			// DumpAllQuotaStates() walks every queue, because there is one per
			// disk device and the per-device write-duration estimate is the
			// interesting number -- dumping only the default queue reports the
			// state of whichever queue happens not to be the one under load.
	static	void				DumpAllQuotaStates();
			void				DumpQuotaState();

			// Returns B_TIMED_OUT if the wait was bounded and expired. Callers
			// must treat that as permission to proceed over quota, NOT as a
			// failed write: refusing the write instead would turn a slow disk
			// into an I/O error visible to applications.
			status_t			WaitIfOverQuota(page_num_t additionalPages,
									bigtime_t timeout, uint32 flags);

private:
	static	status_t			_WriterThreadEntry(void* _this);
			status_t			_PageWriter();
			void				_RecordQuotaWait(bigtime_t waitStart,
									bool timedOut);

private:
			thread_id			fWriterThread;
			BinarySemaphore		fPageWriterCondition;
			ConditionVariable	fUnderQuotaCondition;

			// An exponentially weighted moving average of the time to write one
			// page, decayed while the queue is idle. It was previously the most
			// recent sample despite the name, which let one slow round -- or one
			// old sample on a device that had gone quiet -- set the throttling
			// threshold for every writer on that device.
			bigtime_t			fAveragePageWriteDuration;

			// Identifies this queue in the KDL dump: the device path for a disk
			// queue, "default" for the anonymous one. Copied rather than
			// referenced, because KDiskDevice passes a string it owns.
			char				fName[64];

			// Registry of every live queue, for DumpAllQuotaStates(). A plain
			// list rather than anything cleverer: it is mutated only when a disk
			// appears or goes away, and it is read from KDL with the rest of the
			// machine stopped.
			ModifiedPageQueue*	fNextQueue = NULL;
	static	ModifiedPageQueue*	sQueues;

private:
	static	int64				sGlobalModifiedCount;
			int64				fLastReportedModifiedCount = 0;

	static	bigtime_t			sGlobalEstimatedWriteDuration;
			bigtime_t			fLastReportedEstimatedWriteDuration = 0;

			// Kept so that bounding the wait does not hide the decay that made
			// the wait necessary. A machine that no longer hangs but times out
			// here thousands of times is still broken, and these are what say
			// so.
	static	int64				sQuotaWaits;
	static	int64				sQuotaTimeouts;
	static	bigtime_t			sQuotaWaitTime;
	static	bigtime_t			sQuotaWaitMax;
};


ModifiedPageQueue* vm_page_default_modified_queue();


#endif	// MODIFIED_PAGE_QUEUE_H
