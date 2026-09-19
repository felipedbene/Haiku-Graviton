/*
 * Copyright 2001-2025, Axel Dörfler, axeld@pinc-software.de.
 * This file may be used under the terms of the MIT License.
 */


//! Transaction and logging


#include <StackOrHeapArray.h>

#include "Journal.h"

#include "Debug.h"
#include "Inode.h"

#ifdef BFS_GROW_FAULT_INJECTION
#	include "GrowFault.h"
#endif


struct run_array {
	int32		count;
	int32		max_runs;
	block_run	runs[0];

	void Init(int32 blockSize);
	void Insert(block_run& run);

	int32 CountRuns() const { return BFS_ENDIAN_TO_HOST_INT32(count); }
	int32 MaxRuns() const { return BFS_ENDIAN_TO_HOST_INT32(max_runs) - 1; }
		// that -1 accounts for an off-by-one error in Be's BFS implementation
	const block_run& RunAt(int32 i) const { return runs[i]; }

	static int32 MaxRuns(int32 blockSize);

private:
	static int _Compare(block_run& a, block_run& b);
	int32 _FindInsertionIndex(block_run& run);
};


/*!	DeBeOS (#91 Gap 2): the per-entry integrity trailer, stored in the last
	8 bytes of each run_array index block. Those bytes are always free: BFS uses
	at most run_array::MaxRuns()-1 == 126 runs, but the smallest block (1024
	bytes) has room for 127 physical run_array slots, so the final slot is never
	a run. Larger blocks leave even more slack. Only written/read when the
	superblock's BFS_JOURNAL_FORMAT_CHECKSUM feature bit is set; a legacy volume
	never touches these bytes, which is what keeps the format backward
	compatible.

	\a sequence is the low 32 bits of the volume's monotonic commit sequence;
	\a checksum is the CRC32 over the whole entry (this index block with the
	checksum field taken as zero, followed by every data block of the entry, in
	log order). The sequence lets replay reject a stale-but-internally-valid run
	array left in a log slot from an earlier wrap (its seq is old); the checksum
	rejects a torn/half-written body. Together they let replay discard an
	incomplete tail while still failing on real mid-log corruption. */
struct run_array_trailer {
	uint32		sequence;
	uint32		checksum;
};

class RunArrays {
public:
							RunArrays(Journal* journal);
							~RunArrays();

			status_t		Insert(off_t blockNumber);

			run_array*		ArrayAt(int32 i) { return fArrays.Array()[i]; }
			int32			CountArrays() const { return fArrays.CountItems(); }

			uint32			CountBlocks() const { return fBlockCount; }
			uint32			LogEntryLength() const
								{ return CountBlocks() + CountArrays(); }

			int32			MaxArrayLength();

private:
			status_t		_AddArray();
			bool			_ContainsRun(block_run& run);
			bool			_AddRun(block_run& run);

			Journal*		fJournal;
			uint32			fBlockCount;
			Stack<run_array*> fArrays;
			run_array*		fLastArray;
};

class LogEntry : public DoublyLinkedListLinkImpl<LogEntry> {
public:
							LogEntry(Journal* journal, uint32 logStart,
								uint32 length);
							~LogEntry();

			uint32			Start() const { return fStart; }
			uint32			Length() const { return fLength; }

#ifdef BFS_DEBUGGER_COMMANDS
			void			SetTransactionID(int32 id) { fTransactionID = id; }
			int32			TransactionID() const { return fTransactionID; }
#endif

			Journal*		GetJournal() { return fJournal; }

private:
			Journal*		fJournal;
			uint32			fStart;
			uint32			fLength;
#ifdef BFS_DEBUGGER_COMMANDS
			int32			fTransactionID;
#endif
};


#if BFS_TRACING && !defined(FS_SHELL) && !defined(_BOOT_MODE)
namespace BFSJournalTracing {

class LogEntry : public AbstractTraceEntry {
public:
	LogEntry(::LogEntry* entry, off_t logPosition, bool started)
		:
		fEntry(entry),
#ifdef BFS_DEBUGGER_COMMANDS
		fTransactionID(entry->TransactionID()),
#endif
		fStart(entry->Start()),
		fLength(entry->Length()),
		fLogPosition(logPosition),
		fStarted(started)
	{
		Initialized();
	}

	virtual void AddDump(TraceOutput& out)
	{
#ifdef BFS_DEBUGGER_COMMANDS
		out.Print("bfs:j:%s entry %p id %ld, start %lu, length %lu, log %s "
			"%lu\n", fStarted ? "Started" : "Written", fEntry,
			fTransactionID, fStart, fLength,
			fStarted ? "end" : "start", fLogPosition);
#else
		out.Print("bfs:j:%s entry %p start %lu, length %lu, log %s %lu\n",
			fStarted ? "Started" : "Written", fEntry, fStart, fLength,
			fStarted ? "end" : "start", fLogPosition);
#endif
	}

private:
	::LogEntry*	fEntry;
#ifdef BFS_DEBUGGER_COMMANDS
	int32		fTransactionID;
#endif
	uint32		fStart;
	uint32		fLength;
	uint32		fLogPosition;
	bool		fStarted;
};

}	// namespace BFSJournalTracing

#	define T(x) new(std::nothrow) BFSJournalTracing::x;
#else
#	define T(x) ;
#endif


//	#pragma mark -


static void
add_to_iovec(iovec* vecs, int32& index, int32 max, const void* address,
	size_t size)
{
	if (index > 0 && (addr_t)vecs[index - 1].iov_base
			+ vecs[index - 1].iov_len == (addr_t)address) {
		// the iovec can be combined with the previous one
		vecs[index - 1].iov_len += size;
		return;
	}

	if (index == max)
		panic("no more space for iovecs!");

	// we need to start a new iovec
	vecs[index].iov_base = const_cast<void*>(address);
	vecs[index].iov_len = size;
	index++;
}


//	#pragma mark - journal checksum (DeBeOS #91 Gap 2)


/*!	Standard reflected CRC32 (polynomial 0xedb88320), table built lazily. The
	table fill is idempotent, so the benign race between two volumes' journals
	initializing it concurrently is harmless (both write identical values). A
	CRC32 is strong enough that a torn or stale log body matching a recorded
	checksum by chance is a ~2^-32 event -- which is what lets replay treat a
	checksum match as proof an entry landed intact and a mismatch as proof it did
	not. */
static uint32 sCRCTable[256];
static bool sCRCTableReady = false;


static void
build_crc_table()
{
	for (uint32 i = 0; i < 256; i++) {
		uint32 c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (0xedb88320 ^ (c >> 1)) : (c >> 1);
		sCRCTable[i] = c;
	}
	sCRCTableReady = true;
}


static uint32
crc32_update(uint32 crc, const void* data, size_t length)
{
	if (!sCRCTableReady)
		build_crc_table();

	const uint8* bytes = (const uint8*)data;
	for (size_t i = 0; i < length; i++)
		crc = sCRCTable[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);

	return crc;
}


/*!	Feeds the run_array index block into the running CRC with its 4-byte
	checksum field forced to zero, so that the write side (which fills the field
	afterwards) and the replay side (which must ignore whatever is stored there)
	compute over identical bytes. The sequence field, which precedes it, IS
	covered. */
static uint32
checksum_index_block(uint32 crc, const run_array* array, int32 blockSize)
{
	static const uint8 kZeroField[sizeof(uint32)] = { 0, 0, 0, 0 };
	crc = crc32_update(crc, array, blockSize - sizeof(uint32));
	return crc32_update(crc, kZeroField, sizeof(uint32));
}


static run_array_trailer*
array_trailer(run_array* array, int32 blockSize)
{
	return (run_array_trailer*)((uint8*)array + blockSize
		- sizeof(run_array_trailer));
}


static const run_array_trailer*
array_trailer(const run_array* array, int32 blockSize)
{
	return (const run_array_trailer*)((const uint8*)array + blockSize
		- sizeof(run_array_trailer));
}


//	#pragma mark - LogEntry


LogEntry::LogEntry(Journal* journal, uint32 start, uint32 length)
	:
	fJournal(journal),
	fStart(start),
	fLength(length)
{
}


LogEntry::~LogEntry()
{
}


//	#pragma mark - run_array


/*!	The run_array's size equals the block size of the BFS volume, so we
	cannot use a (non-overridden) new.
	This makes a freshly allocated run_array ready to run.
*/
void
run_array::Init(int32 blockSize)
{
	memset(this, 0, blockSize);
	count = 0;
	max_runs = HOST_ENDIAN_TO_BFS_INT32(MaxRuns(blockSize));
}


/*!	Inserts the block_run into the array. You will have to make sure the
	array is large enough to contain the entry before calling this function.
*/
void
run_array::Insert(block_run& run)
{
	int32 index = _FindInsertionIndex(run);
	if (index == -1) {
		// add to the end
		runs[CountRuns()] = run;
	} else {
		// insert at index
		memmove(&runs[index + 1], &runs[index],
			(CountRuns() - index) * sizeof(off_t));
		runs[index] = run;
	}

	count = HOST_ENDIAN_TO_BFS_INT32(CountRuns() + 1);
}


/*static*/ int32
run_array::MaxRuns(int32 blockSize)
{
	// For whatever reason, BFS restricts the maximum array size
	uint32 maxCount = (blockSize - sizeof(run_array)) / sizeof(block_run);
	if (maxCount < 128)
		return maxCount;

	return 127;
}


/*static*/ int
run_array::_Compare(block_run& a, block_run& b)
{
	int cmp = a.AllocationGroup() - b.AllocationGroup();
	if (cmp == 0)
		return a.Start() - b.Start();

	return cmp;
}


int32
run_array::_FindInsertionIndex(block_run& run)
{
	int32 min = 0, max = CountRuns() - 1;
	int32 i = 0;
	if (max >= 8) {
		while (min <= max) {
			i = (min + max) / 2;

			int cmp = _Compare(runs[i], run);
			if (cmp < 0)
				min = i + 1;
			else if (cmp > 0)
				max = i - 1;
			else
				return -1;
		}

		if (_Compare(runs[i], run) < 0)
			i++;
	} else {
		for (; i <= max; i++) {
			if (_Compare(runs[i], run) > 0)
				break;
		}
		if (i == count)
			return -1;
	}

	return i;
}


//	#pragma mark - RunArrays


RunArrays::RunArrays(Journal* journal)
	:
	fJournal(journal),
	fBlockCount(0),
	fArrays(),
	fLastArray(NULL)
{
}


RunArrays::~RunArrays()
{
	run_array* array;
	while (fArrays.Pop(&array))
		free(array);
}


bool
RunArrays::_ContainsRun(block_run& run)
{
	for (int32 i = 0; i < CountArrays(); i++) {
		run_array* array = ArrayAt(i);

		for (int32 j = 0; j < array->CountRuns(); j++) {
			block_run& arrayRun = array->runs[j];
			if (run.AllocationGroup() != arrayRun.AllocationGroup())
				continue;

			if (run.Start() >= arrayRun.Start()
				&& run.Start() + run.Length()
					<= arrayRun.Start() + arrayRun.Length())
				return true;
		}
	}

	return false;
}


/*!	Adds the specified block_run into the array.
	Note: it doesn't support overlapping - it must only be used
	with block_runs of length 1!
*/
bool
RunArrays::_AddRun(block_run& run)
{
	ASSERT(run.length == 1);

	// Be's BFS log replay routine can only deal with block_runs of size 1
	// A pity, isn't it? Too sad we have to be compatible.

	if (fLastArray == NULL || fLastArray->CountRuns() == fLastArray->MaxRuns())
		return false;

	fLastArray->Insert(run);
	fBlockCount++;
	return true;
}


status_t
RunArrays::_AddArray()
{
	int32 blockSize = fJournal->GetVolume()->BlockSize();

	run_array* array = (run_array*)malloc(blockSize);
	if (array == NULL)
		return B_NO_MEMORY;

	if (fArrays.Push(array) != B_OK) {
		free(array);
		return B_NO_MEMORY;
	}

	array->Init(blockSize);
	fLastArray = array;
	return B_OK;
}


status_t
RunArrays::Insert(off_t blockNumber)
{
	Volume* volume = fJournal->GetVolume();
	block_run run = volume->ToBlockRun(blockNumber);

	if (fLastArray != NULL) {
		// check if the block is already in the array
		if (_ContainsRun(run))
			return B_OK;
	}

	// insert block into array

	if (!_AddRun(run)) {
		// array is full
		if (_AddArray() != B_OK || !_AddRun(run))
			return B_NO_MEMORY;
	}

	return B_OK;
}


int32
RunArrays::MaxArrayLength()
{
	int32 max = 0;
	for (int32 i = 0; i < CountArrays(); i++) {
		if (ArrayAt(i)->CountRuns() > max)
			max = ArrayAt(i)->CountRuns();
	}

	return max;
}


//	#pragma mark - Journal


Journal::Journal(Volume* volume)
	:
	fVolume(volume),
	fOwner(NULL),
	fLogSize(volume->Log().Length()),
	fMaxTransactionSize(fLogSize / 2 - 5),
	fUsed(0),
	fUnwrittenTransactions(0),
	fHasSubtransaction(false),
	fSeparateSubTransactions(false),
	fChecksumEnabled((volume->SuperBlock().JournalFormatFlags()
		& BFS_JOURNAL_FORMAT_CHECKSUM) != 0),
	fNextSequence(volume->SuperBlock().LogCommitSequence())
{
	recursive_lock_init(&fLock, "bfs journal");
	mutex_init(&fEntriesLock, "bfs journal entries");

	fLogFlusherSem = create_sem(0, "bfs log flusher");
	fLogFlusher = spawn_kernel_thread(&Journal::_LogFlusher, "bfs log flusher",
		B_NORMAL_PRIORITY, this);
	if (fLogFlusher > 0)
		resume_thread(fLogFlusher);
}


Journal::~Journal()
{
	FlushLogAndBlocks();

	recursive_lock_destroy(&fLock);
	mutex_destroy(&fEntriesLock);

	sem_id logFlusher = fLogFlusherSem;
	fLogFlusherSem = -1;
	delete_sem(logFlusher);
	wait_for_thread(fLogFlusher, NULL);
}


status_t
Journal::InitCheck()
{
	return B_OK;
}


/*!	\brief Does a very basic consistency check of the run array.
	It will check the maximum run count as well as if all of the runs fall
	within a the volume.
*/
status_t
Journal::_CheckRunArray(const run_array* array)
{
	int32 maxRuns = run_array::MaxRuns(fVolume->BlockSize()) - 1;
		// the -1 works around an off-by-one bug in Be's BFS implementation,
		// same as in run_array::MaxRuns()
	if (array->MaxRuns() != maxRuns
		|| array->CountRuns() > maxRuns
		|| array->CountRuns() <= 0) {
		dprintf("run count: %d, array max: %d, max runs: %d\n",
			(int)array->CountRuns(), (int)array->MaxRuns(), (int)maxRuns);
		FATAL(("Log entry has broken header!\n"));
		return B_ERROR;
	}

	for (int32 i = 0; i < array->CountRuns(); i++) {
		if (fVolume->ValidateBlockRun(array->RunAt(i)) != B_OK)
			return B_ERROR;
	}

	PRINT(("Log entry has %" B_PRId32 " entries\n", array->CountRuns()));
	return B_OK;
}


/*!	DeBeOS (#91 Gap 2): a quiet geometry check on a run_array read from the log,
	used by the checksum-validation walk. Unlike _CheckRunArray() it never calls
	Volume::Panic() or FATAL(), because the integrity walk deliberately probes
	entries it expects to be torn or stale (and, in _FindCommittedEntryAfter(),
	arbitrary log blocks that are not entry starts at all); flipping the volume
	read-only or logging on those would be wrong. Returns the total data-block
	count of the array in \a _blocks when valid. */
bool
Journal::_ValidRunArrayGeometry(const run_array* array, int32* _blocks) const
{
	int32 maxRuns = run_array::MaxRuns(fVolume->BlockSize()) - 1;
	if (array->MaxRuns() != maxRuns
		|| array->CountRuns() > maxRuns
		|| array->CountRuns() <= 0)
		return false;

	int32 blocks = 0;
	for (int32 i = 0; i < array->CountRuns(); i++) {
		const block_run& run = array->RunAt(i);
		// quiet mirror of Volume::ValidateBlockRun()
		if (run.AllocationGroup() < 0
			|| run.AllocationGroup() > (int32)fVolume->AllocationGroups()
			|| run.Start() > (1UL << fVolume->AllocationGroupShift())
			|| run.length == 0
			|| uint32(run.Length() + run.Start())
					> (1UL << fVolume->AllocationGroupShift()))
			return false;

		blocks += run.Length();
	}

	if (_blocks != NULL)
		*_blocks = blocks;
	return true;
}


/*!	DeBeOS (#91 Gap 2): computes the CRC32 over a whole log entry -- the index
	block (with its checksum field taken as zero) followed by every data block of
	the entry read back from the log ring, in log order. \a firstDataBlock is the
	ring position (already reduced modulo fLogSize) of the entry's first data
	block. Mirrors the block walk in _ReplayRunArray() so the value matches what
	the write side stored. */
status_t
Journal::_RunArrayChecksum(const run_array* array, off_t firstDataBlock,
	uint32* _checksum)
{
	int32 blockSize = fVolume->BlockSize();
	off_t logOffset = fVolume->ToBlock(fVolume->Log());

	uint32 crc = checksum_index_block(0xffffffff, array, blockSize);

	CachedBlock cached(fVolume);
	off_t blockNumber = firstDataBlock;

	for (int32 index = 0; index < array->CountRuns(); index++) {
		const block_run& run = array->RunAt(index);
		for (int32 i = 0; i < run.Length(); i++) {
			status_t status = cached.SetTo(logOffset + blockNumber);
			if (status != B_OK)
				return status;

			crc = crc32_update(crc, cached.Block(), blockSize);
			blockNumber = (blockNumber + 1) % fLogSize;
		}
	}

	*_checksum = crc ^ 0xffffffff;
	return B_OK;
}


/*!	DeBeOS (#91 Gap 2): inspects the log entry that starts at ring offset \a
	start without modifying anything. Reports whether the run_array header is
	geometrically valid, whether its recorded checksum matches a recomputation
	over the entry's body, the entry's stored sequence number, and its length in
	log blocks (index block + data blocks) so the caller can advance to the next
	entry. A geometrically invalid header yields \a _geometryValid == false and a
	\a _length of 0 (the caller must not advance past it). */
status_t
Journal::_ScanLogEntry(int32 start, int32* _length, uint32* _sequence,
	bool* _geometryValid, bool* _checksumValid)
{
	off_t logOffset = fVolume->ToBlock(fVolume->Log());
	off_t firstBlockNumber = start % fLogSize;

	*_length = 0;
	*_sequence = 0;
	*_geometryValid = false;
	*_checksumValid = false;

	CachedBlock cachedArray(fVolume);
	status_t status = cachedArray.SetTo(logOffset + firstBlockNumber);
	if (status != B_OK)
		return status;

	const run_array* array = (const run_array*)cachedArray.Block();
	int32 blocks = 0;
	if (!_ValidRunArrayGeometry(array, &blocks))
		return B_OK;

	*_geometryValid = true;
	*_length = 1 + blocks;

	int32 blockSize = fVolume->BlockSize();
	const run_array_trailer* trailer = array_trailer(array, blockSize);
	*_sequence = BFS_ENDIAN_TO_HOST_INT32(trailer->sequence);
	uint32 stored = BFS_ENDIAN_TO_HOST_INT32(trailer->checksum);

	uint32 crc;
	status = _RunArrayChecksum(array, (firstBlockNumber + 1) % fLogSize, &crc);
	if (status != B_OK)
		return status;

	*_checksumValid = (crc == stored);
	return B_OK;
}


/*!	DeBeOS (#91 Gap 2): decides whether a bad entry found at \a afterStart is the
	torn tail (nothing valid follows it) or a hole in the middle of the log (a
	durably committed entry follows it). Scans every log block from just after
	\a afterStart up to log_end and returns true as soon as it finds a block that
	is a geometrically valid run_array whose checksum verifies and -- when a good
	prefix established an expected sequence -- whose sequence is at least the one
	the bad entry should have carried. A stale run array left from an earlier
	wrap fails that sequence test; a false positive on an arbitrary block fails
	the CRC32 with ~2^-32 probability. Scanning block-by-block (rather than by
	entry length) is deliberate: the bad entry's own length cannot be trusted. */
bool
Journal::_FindCommittedEntryAfter(int32 afterStart, uint32 minSequence,
	bool haveMinSequence)
{
	off_t logOffset = fVolume->ToBlock(fVolume->Log());
	int32 blockSize = fVolume->BlockSize();
	int32 logEnd = fVolume->LogEnd();

	CachedBlock cachedArray(fVolume);
	int32 pos = (afterStart + 1) % fLogSize;

	while (pos != logEnd) {
		if (cachedArray.SetTo(logOffset + pos) == B_OK) {
			const run_array* array = (const run_array*)cachedArray.Block();
			if (_ValidRunArrayGeometry(array, NULL)) {
				const run_array_trailer* trailer
					= array_trailer(array, blockSize);
				uint32 seq = BFS_ENDIAN_TO_HOST_INT32(trailer->sequence);
				uint32 stored = BFS_ENDIAN_TO_HOST_INT32(trailer->checksum);

				uint32 crc;
				if (_RunArrayChecksum(array, (pos + 1) % fLogSize, &crc) == B_OK
					&& crc == stored
					&& (!haveMinSequence || seq >= minSequence)) {
					return true;
				}
			}
		}

		pos = (pos + 1) % fLogSize;
	}

	return false;
}


/*!	DeBeOS (#91 Gap 2): walks the log from log_start to log_end verifying each
	run_array's geometry, checksum, and per-transaction sequence continuity, and
	reports the position up to which replay is safe in \a _effectiveEnd.

	Because a BFS transaction is committed atomically (one log_end advance) but
	may span several run_arrays that share one sequence number, the truncation
	point is always a transaction boundary -- replay never half-applies a
	transaction.

	- All entries valid: \a _effectiveEnd == log_end (replay everything, the
	  historical behaviour).
	- A torn/incomplete tail with no committed transaction after it: \a
	  _effectiveEnd is set to the start of the torn transaction, so the whole
	  transaction is discarded. This is the redo-log guarantee that an
	  un-fully-committed transaction is simply lost, never half-applied.
	- A bad entry with a strictly-later committed transaction still after it:
	  returns B_BAD_DATA. That is real corruption (a hole), not a torn tail, and
	  must fail the mount as before rather than silently masking it. */
status_t
Journal::_ValidateLogTail(int32* _effectiveEnd)
{
	int32 logEnd = fVolume->LogEnd();
	int32 start = fVolume->LogStart();

	// Per-transaction sequence bookkeeping. All run_arrays of one transaction
	// share a sequence; the sequence increments by one at each transaction
	// boundary. transactionStart tracks where the current transaction's first
	// run_array sits, so a torn tail can be dropped to a transaction boundary
	// rather than mid-transaction.
	uint32 lastSequence = 0;
	bool haveSequence = false;
	int32 transactionStart = start;
	int32 lastStart = -1;

	while (start != logEnd) {
		if (start == lastStart) {
			// no forward progress -- a zero-length or self-referential entry;
			// treat as corruption rather than spin.
			return B_BAD_DATA;
		}
		lastStart = start;

		int32 length;
		uint32 sequence;
		bool geometryValid;
		bool checksumValid;
		status_t status = _ScanLogEntry(start, &length, &sequence,
			&geometryValid, &checksumValid);
		if (status != B_OK)
			return status;

		// A run_array is a valid continuation only if it is intact and its
		// sequence either repeats the current transaction's or opens the next.
		bool sequenceOK = !haveSequence
			|| sequence == lastSequence || sequence == lastSequence + 1;
		if (!geometryValid || !checksumValid || !sequenceOK) {
			// First bad run_array. Work out which transaction it belongs to and
			// that transaction's sequence: an intact header whose sequence opens
			// a new transaction starts its own; anything else (a continuation,
			// or a header too corrupt to read a sequence from) belongs to the
			// current transaction.
			bool startsNewTransaction = geometryValid && haveSequence
				&& sequence == lastSequence + 1;
			int32 truncateAt;
			uint32 tornSequence;
			bool tornSequenceKnown;
			if (!haveSequence) {
				// The very first run_array is bad: nothing before it is
				// committed, so the whole log is a torn/garbage tail.
				truncateAt = start;
				tornSequenceKnown = false;
				tornSequence = 0;
			} else if (startsNewTransaction) {
				// Keep the completed transactions before it; drop this one on.
				truncateAt = start;
				tornSequenceKnown = true;
				tornSequence = sequence;
			} else {
				// Continuation of, or an unreadable head of, the current
				// transaction: discard the whole transaction. This is the
				// conservative branch -- if a fresh transaction's very first
				// run_array was torn into stale content whose header happens to
				// parse but whose sequence is old, we cannot prove the prior
				// transaction was complete, so we drop it too. That may discard
				// one extra fully-committed transaction, but it never
				// half-applies one (consistency over squeezing out the last
				// committed byte on a crash).
				truncateAt = transactionStart;
				tornSequenceKnown = true;
				tornSequence = lastSequence;
			}

			// If a strictly-later transaction was committed after the torn one,
			// this is a hole in the middle of the log -- real corruption, fail
			// as before. Siblings of the torn transaction share its sequence and
			// are correctly ignored (they are not a later commit).
			if (_FindCommittedEntryAfter(start,
					tornSequenceKnown ? tornSequence + 1 : 0, tornSequenceKnown))
				return B_BAD_DATA;

			*_effectiveEnd = truncateAt;
			return B_OK;
		}

		if (!haveSequence || sequence == lastSequence + 1) {
			// entering a new transaction
			transactionStart = start;
			lastSequence = sequence;
			haveSequence = true;
		}
		start = (start + length) % fLogSize;
	}

	*_effectiveEnd = logEnd;
	return B_OK;
}


/*!	Replays an entry in the log.
	\a _start points to the entry in the log, and will be bumped to the next
	one if replaying succeeded.
*/
status_t
Journal::_ReplayRunArray(int32* _start)
{
	PRINT(("ReplayRunArray(start = %" B_PRId32 ")\n", *_start));

	off_t logOffset = fVolume->ToBlock(fVolume->Log());
	off_t firstBlockNumber = *_start % fLogSize;

	CachedBlock cachedArray(fVolume);

	status_t status = cachedArray.SetTo(logOffset + firstBlockNumber);
	if (status != B_OK)
		return status;

	const run_array* array = (const run_array*)cachedArray.Block();
	if (_CheckRunArray(array) < B_OK)
		return B_BAD_DATA;

	// First pass: check integrity of the blocks in the run array

	CachedBlock cached(fVolume);

	firstBlockNumber = (firstBlockNumber + 1) % fLogSize;
	off_t blockNumber = firstBlockNumber;
	int32 blockSize = fVolume->BlockSize();

	for (int32 index = 0; index < array->CountRuns(); index++) {
		const block_run& run = array->RunAt(index);

		off_t offset = fVolume->ToOffset(run);
		for (int32 i = 0; i < run.Length(); i++) {
			status = cached.SetTo(logOffset + blockNumber);
			if (status != B_OK)
				RETURN_ERROR(status);

			// TODO: eventually check other well known offsets, like the
			// root and index dirs
			if (offset == 0) {
				// This log entry writes over the superblock - check if
				// it's valid!
				if (Volume::CheckSuperBlock(cached.Block()) != B_OK) {
					FATAL(("Log contains invalid superblock!\n"));
					RETURN_ERROR(B_BAD_DATA);
				}
			}

			blockNumber = (blockNumber + 1) % fLogSize;
			offset += blockSize;
		}
	}

	// Second pass: write back its blocks

	blockNumber = firstBlockNumber;
	int32 count = 1;

	for (int32 index = 0; index < array->CountRuns(); index++) {
		const block_run& run = array->RunAt(index);
		INFORM(("replay block run %u:%u:%u in log at %" B_PRIdOFF "!\n",
			(int)run.AllocationGroup(), run.Start(), run.Length(), blockNumber));

		off_t offset = fVolume->ToOffset(run);
		for (int32 i = 0; i < run.Length(); i++) {
			status = cached.SetTo(logOffset + blockNumber);
			if (status != B_OK)
				RETURN_ERROR(status);

			ssize_t written = write_pos(fVolume->Device(), offset,
				cached.Block(), blockSize);
			if (written != blockSize)
				RETURN_ERROR(B_IO_ERROR);

			blockNumber = (blockNumber + 1) % fLogSize;
			offset += blockSize;
			count++;
		}
	}

	*_start += count;
	return B_OK;
}


/*!	Replays all log entries - this will put the disk into a
	consistent and clean state, if it was not correctly unmounted
	before.
	This method is called by Journal::InitCheck() if the log start
	and end pointer don't match.
*/
status_t
Journal::ReplayLog()
{
	// TODO: this logic won't work whenever the size of the pending transaction
	//	equals the size of the log (happens with the original BFS only)
	if (fVolume->LogStart() == fVolume->LogEnd())
		return B_OK;

	INFORM(("Replay log, disk was not correctly unmounted...\n"));

	if (fVolume->SuperBlock().flags != SUPER_BLOCK_DISK_DIRTY) {
		INFORM(("log_start and log_end differ, but disk is marked clean - "
			"trying to replay log...\n"));
	}

	if (fVolume->IsReadOnly())
		return B_READ_ONLY_DEVICE;

	// DeBeOS (#91 Gap 2): on a checksummed volume, verify the log's per-entry
	// integrity before touching a single home block. This distinguishes a torn
	// or stale tail (the redo-log guarantee: an un-fully-committed transaction
	// is discarded, not half-applied) from real mid-log corruption (which must
	// still fail the mount, exactly as it does today). replayEnd is where replay
	// must stop; on a legacy volume it stays at log_end and the loop below is
	// byte-for-byte the historical behaviour.
	int32 replayEnd = fVolume->LogEnd();
	if (fChecksumEnabled) {
		status_t status = _ValidateLogTail(&replayEnd);
		if (status != B_OK) {
			FATAL(("log integrity check failed, data may be corrupted: %s\n",
				strerror(status)));
			return B_ERROR;
		}
		if (replayEnd != fVolume->LogEnd()) {
			INFORM(("bfs: discarding torn journal tail (log_end %d -> %d), "
				"mounting clean\n", (int)fVolume->LogEnd(), (int)replayEnd));
		}
	}

	int32 start = fVolume->LogStart();
	int32 lastStart = -1;
	while (true) {
		// stop if the log is completely flushed
		if (start == replayEnd)
			break;

		if (start == lastStart) {
			// strange, flushing the log hasn't changed the log_start pointer
			return B_ERROR;
		}
		lastStart = start;

		status_t status = _ReplayRunArray(&start);
		if (status != B_OK) {
			FATAL(("replaying log entry from %d failed: %s\n", (int)start,
				strerror(status)));
			return B_ERROR;
		}
		start = start % fLogSize;
	}

	PRINT(("replaying worked fine!\n"));
	// A discarded torn tail moves log_end back to the last good entry as well,
	// so the on-disk log is empty and consistent after this write.
	fVolume->SuperBlock().log_end = HOST_ENDIAN_TO_BFS_INT64(replayEnd);
	fVolume->LogEnd() = replayEnd;
	fVolume->SuperBlock().log_start = HOST_ENDIAN_TO_BFS_INT64(replayEnd);
	fVolume->LogStart() = replayEnd;
	fVolume->SuperBlock().flags = HOST_ENDIAN_TO_BFS_INT32(
		SUPER_BLOCK_DISK_CLEAN);

	return fVolume->WriteSuperBlock();
}


size_t
Journal::CurrentTransactionSize() const
{
	if (_HasSubTransaction()) {
		return cache_blocks_in_sub_transaction(fVolume->BlockCache(),
			fTransactionID);
	}

	return cache_blocks_in_main_transaction(fVolume->BlockCache(),
		fTransactionID);
}


bool
Journal::CurrentTransactionTooLarge() const
{
	return CurrentTransactionSize() > fLogSize;
}


/*!	This is a callback function that is called by the cache, whenever
	all blocks of a transaction have been flushed to disk.
	This lets us keep track of completed transactions, and update
	the log start pointer as needed. Note, the transactions may not be
	completed in the order they were written.
*/
/*static*/ void
Journal::_TransactionWritten(int32 transactionID, int32 event, void* _logEntry)
{
	LogEntry* logEntry = (LogEntry*)_logEntry;

	PRINT(("Log entry %p has been finished, transaction ID = %" B_PRId32 "\n",
		logEntry, transactionID));

	Journal* journal = logEntry->GetJournal();
	disk_super_block& superBlock = journal->fVolume->SuperBlock();
	bool update = false;

	// Set log_start pointer if possible...

	mutex_lock(&journal->fEntriesLock);

	if (logEntry == journal->fEntries.First()) {
		LogEntry* next = journal->fEntries.GetNext(logEntry);
		if (next != NULL) {
			superBlock.log_start = HOST_ENDIAN_TO_BFS_INT64(next->Start()
				% journal->fLogSize);
		} else {
			superBlock.log_start = HOST_ENDIAN_TO_BFS_INT64(
				journal->fVolume->LogEnd());
		}

		update = true;
	}

	T(LogEntry(logEntry, superBlock.LogStart(), false));

	journal->fUsed -= logEntry->Length();
	journal->fEntries.Remove(logEntry);
	mutex_unlock(&journal->fEntriesLock);

	delete logEntry;

	// update the superblock, and change the disk's state, if necessary

	if (update) {
		if (superBlock.log_start == superBlock.log_end)
			superBlock.flags = HOST_ENDIAN_TO_BFS_INT32(SUPER_BLOCK_DISK_CLEAN);

		status_t status = journal->fVolume->WriteSuperBlock();
		if (status != B_OK) {
			FATAL(("_TransactionWritten: could not write back superblock: %s\n",
				strerror(status)));
		}

		journal->fVolume->LogStart() = superBlock.LogStart();
	}
}


/*!	Listens to TRANSACTION_IDLE events, and flushes the log when that happens */
/*static*/ void
Journal::_TransactionIdle(int32 transactionID, int32 event, void* _journal)
{
	// The current transaction seems to be idle - flush it. (We can't do this
	// in this thread, as flushing the log can produce new transaction events.)
	Journal* journal = (Journal*)_journal;
	release_sem(journal->fLogFlusherSem);
}


/*static*/ status_t
Journal::_LogFlusher(void* _journal)
{
	Journal* journal = (Journal*)_journal;
	while (journal->fLogFlusherSem >= 0) {
		if (acquire_sem(journal->fLogFlusherSem) != B_OK)
			continue;

		journal->_FlushLog(false, false);
	}
	return B_OK;
}


/*!	Writes the blocks that are part of current transaction into the log,
	and ends the current transaction.
	If the current transaction is too large to fit into the log, it will
	try to detach an existing sub-transaction.
*/
status_t
Journal::_WriteTransactionToLog()
{
	// TODO: in case of a failure, we need a backup plan like writing all
	//	changed blocks back to disk immediately (hello disk corruption!)

	bool detached = false;

	if (_TransactionSize() > fLogSize) {
		// The current transaction won't fit into the log anymore, try to
		// detach the current sub-transaction
		if (_HasSubTransaction() && cache_blocks_in_main_transaction(
				fVolume->BlockCache(), fTransactionID) < (int32)fLogSize) {
			detached = true;
		} else {
			// We created a transaction larger than one we can write back to
			// disk - the only option we have (besides risking disk corruption
			// by writing it back anyway), is to let it fail.
			dprintf("transaction too large (%d blocks, log size %d)!\n",
				(int)_TransactionSize(), (int)fLogSize);
			return B_BUFFER_OVERFLOW;
		}
	}

	fHasSubtransaction = false;

	int32 blockShift = fVolume->BlockShift();
	off_t logOffset = fVolume->ToBlock(fVolume->Log()) << blockShift;
	off_t logStart = fVolume->LogEnd() % fLogSize;
	off_t logPosition = logStart;
	status_t status;

	// create run_array structures for all changed blocks

	RunArrays runArrays(this);

	off_t blockNumber;
	long cookie = 0;
	while (cache_next_block_in_transaction(fVolume->BlockCache(),
			fTransactionID, detached, &cookie, &blockNumber, NULL,
			NULL) == B_OK) {
		status = runArrays.Insert(blockNumber);
		if (status < B_OK) {
			FATAL(("filling log entry failed!"));
			return status;
		}
	}

	if (runArrays.CountBlocks() == 0) {
		// nothing has changed during this transaction
		if (detached) {
			fTransactionID = cache_detach_sub_transaction(fVolume->BlockCache(),
				fTransactionID, NULL, NULL);
			fUnwrittenTransactions = 1;
		} else {
			cache_end_transaction(fVolume->BlockCache(), fTransactionID, NULL,
				NULL);
			fUnwrittenTransactions = 0;
		}
		return B_OK;
	}

	// If necessary, flush the log, so that we have enough space for this
	// transaction
	if (runArrays.LogEntryLength() > FreeLogBlocks()) {
		cache_sync_transaction(fVolume->BlockCache(), fTransactionID);
		if (runArrays.LogEntryLength() > FreeLogBlocks()) {
			panic("no space in log after sync (%ld for %ld blocks)!",
				(long)FreeLogBlocks(), (long)runArrays.LogEntryLength());
		}
	}

	// Write log entries to disk

	int32 maxVecs = runArrays.MaxArrayLength() + 1;
		// one extra for the index block

	BStackOrHeapArray<iovec, 8> vecs(maxVecs);
	if (!vecs.IsValid()) {
		// TODO: write back log entries directly?
		return B_NO_MEMORY;
	}

	// DeBeOS (#91 Gap 2): every run_array of this transaction is stamped with the
	// SAME sequence number, and the counter is advanced once, per transaction.
	// BFS commits a whole transaction with a single log_end advance, so the unit
	// replay must be able to discard atomically is the transaction, not the
	// individual run_array. A shared per-transaction sequence lets replay find
	// the transaction boundary (where the sequence changes) and drop a torn tail
	// back to it, never half-applying a transaction.
	uint32 transactionSequence = (uint32)fNextSequence;

	for (int32 k = 0; k < runArrays.CountArrays(); k++) {
		run_array* array = runArrays.ArrayAt(k);
		int32 index = 0, count = 1;
		int32 wrap = fLogSize - logStart;

		// Stamp this run_array's integrity trailer before it is written. The
		// checksum covers the index block (its checksum field taken as zero)
		// plus every data block of the entry, so replay can tell a torn or stale
		// copy from an intact one. This is a read-only pass over the same blocks
		// the write loop below fetches again -- cheap against the block cache,
		// and it leaves the write path's own logic untouched.
		if (fChecksumEnabled) {
			run_array_trailer* trailer
				= array_trailer(array, fVolume->BlockSize());
			trailer->sequence = HOST_ENDIAN_TO_BFS_INT32(transactionSequence);
			trailer->checksum = 0;

			uint32 crc = checksum_index_block(0xffffffff, array,
				fVolume->BlockSize());
			for (int32 i = 0; i < array->CountRuns(); i++) {
				const block_run& run = array->RunAt(i);
				off_t dataBlock = fVolume->ToBlock(run);
				for (int32 j = 0; j < run.Length(); j++) {
					const void* data = block_cache_get(fVolume->BlockCache(),
						dataBlock + j);
					if (data == NULL)
						return B_IO_ERROR;

					crc = crc32_update(crc, data, fVolume->BlockSize());
					block_cache_put(fVolume->BlockCache(), dataBlock + j);
				}
			}

			trailer->checksum = HOST_ENDIAN_TO_BFS_INT32(crc ^ 0xffffffff);
		}

		add_to_iovec(vecs, index, maxVecs, (void*)array, fVolume->BlockSize());

		// add block runs

		for (int32 i = 0; i < array->CountRuns(); i++) {
			const block_run& run = array->RunAt(i);
			off_t blockNumber = fVolume->ToBlock(run);

			for (int32 j = 0; j < run.Length(); j++) {
				if (count >= wrap) {
					// We need to write back the first half of the entry
					// directly as the log wraps around
					if (writev_pos(fVolume->Device(), logOffset
							+ (logStart << blockShift), vecs, index) < 0) {
						// The log body did not reach the device (e.g. the block
						// device vanished on a forced stop). We must not fall
						// through to advance log_end below: committing a
						// transaction whose body was never written makes replay
						// walk a torn entry, and the log format carries no
						// per-entry checksum to catch it, so a plausible stale
						// run array replays into live blocks -- silent
						// corruption. Bailing out leaves the on-disk log_end at
						// the last good transaction, which is consistent. (Same
						// contract as the block_cache_get failure just below.)
						FATAL(("could not write log area: %s\n",
							strerror(errno)));
						return B_IO_ERROR;
					}

					logPosition = logStart + count;
					logStart = 0;
					wrap = fLogSize;
					count = 0;
					index = 0;
				}

				// make blocks available in the cache
				const void* data = block_cache_get(fVolume->BlockCache(),
					blockNumber + j);
				if (data == NULL)
					return B_IO_ERROR;

				add_to_iovec(vecs, index, maxVecs, data, fVolume->BlockSize());
				count++;
			}
		}

		// write back the rest of the log entry
		if (count > 0) {
			logPosition = logStart + count;
			if (writev_pos(fVolume->Device(), logOffset
					+ (logStart << blockShift), vecs, index) < 0) {
				// See the wrap case above: a failed body write must not be
				// committed. Unlike that mid-array case, every run of this
				// array has already been fetched, so release them here before
				// bailing out rather than leaking the cache references.
				for (int32 i = 0; i < array->CountRuns(); i++) {
					const block_run& run = array->RunAt(i);
					off_t releaseBlock = fVolume->ToBlock(run);
					for (int32 j = 0; j < run.Length(); j++)
						block_cache_put(fVolume->BlockCache(), releaseBlock + j);
				}
				FATAL(("could not write log area: %s\n", strerror(errno)));
				return B_IO_ERROR;
			}
		}

		// release blocks again
		for (int32 i = 0; i < array->CountRuns(); i++) {
			const block_run& run = array->RunAt(i);
			off_t blockNumber = fVolume->ToBlock(run);

			for (int32 j = 0; j < run.Length(); j++) {
				block_cache_put(fVolume->BlockCache(), blockNumber + j);
			}
		}

		logStart = logPosition % fLogSize;
	}

	LogEntry* logEntry = new(std::nothrow) LogEntry(this, fVolume->LogEnd(),
		runArrays.LogEntryLength());
	if (logEntry == NULL) {
		FATAL(("no memory to allocate log entries!"));
		return B_NO_MEMORY;
	}

#ifdef BFS_DEBUGGER_COMMANDS
	logEntry->SetTransactionID(fTransactionID);
#endif

	// Barrier between the log body and the commit record. The run arrays and
	// their block data were just written to the log area; the superblock write
	// below advances log_end, which is the commit that makes replay treat this
	// entry as valid. Both writes go straight to the device and, on a volatile
	// write-back cache, may reach the platter in any order. Without a barrier
	// here a power loss can persist the advanced log_end while the entry it
	// points at is still stale in the cache -- replay then walks a log entry
	// whose body never landed. _CheckRunArray() only sanity-checks the run-array
	// header and block geometry; there is no CRC or sequence number over the
	// body, so a plausible-looking stale run array replays into live blocks
	// (silent corruption). Flushing before the commit guarantees the body is
	// durable first; the flush after the commit (below) then orders the log
	// ahead of the in-place writeback. This is standard write-ahead-log commit
	// ordering. See graviton/docs/device-watchdog-and-bfs-crashsafety.md.
	//
	// DeBeOS (#91 Gap 2): on a checksummed volume the per-entry checksum+sequence
	// stamped above now lets replay *detect* a body that did not land and discard
	// it, closing the silent-corruption path this comment warned about. The
	// barrier is still required: it keeps a committed entry actually replayable
	// (durable-before-commit) rather than something replay would always have to
	// throw away.
	ioctl(fVolume->Device(), B_FLUSH_DRIVE_CACHE);

	// Update the log end pointer in the superblock

	fVolume->SuperBlock().flags = SUPER_BLOCK_DISK_DIRTY;
	fVolume->SuperBlock().log_end = HOST_ENDIAN_TO_BFS_INT64(logPosition);
	if (fChecksumEnabled) {
		// Advance the per-volume commit counter once for this transaction and
		// persist it in the same superblock write that commits log_end, so the
		// sequence is durable and monotonic for the volume's lifetime (never
		// reused, even across a discarded torn tail).
		fNextSequence++;
		fVolume->SuperBlock().log_commit_sequence
			= HOST_ENDIAN_TO_BFS_INT64(fNextSequence);
	}

	status = fVolume->WriteSuperBlock();
	if (status != B_OK) {
		// The commit record itself (the superblock's log_end) did not reach
		// the device. Leave both the on-disk and in-memory log_end at the last
		// good transaction and do not end the cache transaction: its blocks
		// stay pinned in the block cache instead of being written back to their
		// home locations with no durable log entry behind them (a write-ahead
		// log violation that would corrupt on the next replay). The next flush
		// retries; a device that is truly gone keeps failing here without ever
		// leaving the on-disk state inconsistent.
		FATAL(("writing the log commit failed: %s\n", strerror(status)));
		delete logEntry;
		return status;
	}

	fVolume->LogEnd() = logPosition;
	T(LogEntry(logEntry, fVolume->LogEnd(), true));

	// We need to flush the drives own cache here to ensure
	// disk consistency.
	// If that call fails, we can't do anything about it anyway
	ioctl(fVolume->Device(), B_FLUSH_DRIVE_CACHE);

#ifdef BFS_GROW_FAULT_INJECTION
	// Host-only crash injection for the #91 Gap 2 torn-tail A/B. The log body
	// and the commit record (superblock log_end + sequence) are now durable, but
	// the transaction's home blocks are still dirty in the block cache -- exactly
	// the on-disk state a power loss right after commit leaves: a dirty log the
	// next mount must replay. _exit()s on the BFS_JOURNAL_ABORT_NTH'th commit
	// when BFS_JOURNAL_ABORT is set (see bfs_journal_fault_commit()).
	bfs_journal_fault_commit();
#endif

	// at this point, we can finally end the transaction - we're in
	// a guaranteed valid state

	mutex_lock(&fEntriesLock);
	fEntries.Add(logEntry);
	fUsed += logEntry->Length();
	mutex_unlock(&fEntriesLock);

	if (detached) {
		fTransactionID = cache_detach_sub_transaction(fVolume->BlockCache(),
			fTransactionID, _TransactionWritten, logEntry);
		fUnwrittenTransactions = 1;

		if (status == B_OK && _TransactionSize() > fLogSize) {
			// If the transaction is too large after writing, there is no way to
			// recover, so let this transaction fail.
			dprintf("transaction too large (%d blocks, log size %d)!\n",
				(int)_TransactionSize(), (int)fLogSize);
			return B_BUFFER_OVERFLOW;
		}
	} else {
		cache_end_transaction(fVolume->BlockCache(), fTransactionID,
			_TransactionWritten, logEntry);
		fUnwrittenTransactions = 0;
	}

	return status;
}


/*!	Flushes the current log entry to disk. If \a flushBlocks is \c true it will
	also write back all dirty blocks for this volume. If \a alreadyLocked is \c
	true, we allow the lock to be held when the function is called.
*/
status_t
Journal::_FlushLog(bool canWait, bool flushBlocks, bool alreadyLocked)
{
	status_t status = canWait ? recursive_lock_lock(&fLock)
		: recursive_lock_trylock(&fLock);
	if (status != B_OK)
		return status;

	int32 allowedLocks = alreadyLocked ? 2 : 1;
	if (recursive_lock_get_recursion(&fLock) > allowedLocks) {
		// whoa, FlushLogAndBlocks() was called from inside a transaction
		recursive_lock_unlock(&fLock);
		return B_OK;
	}

	// write the current log entry to disk

	if (fUnwrittenTransactions != 0) {
		status = _WriteTransactionToLog();
		if (status < B_OK)
			FATAL(("writing current log entry failed: %s\n", strerror(status)));
	}

	if (flushBlocks)
		status = fVolume->FlushDevice();

	recursive_lock_unlock(&fLock);
	return status;
}


/*!	Flushes the current log entry to disk, and also writes back all dirty
	blocks for this volume (completing all open transactions).
*/
status_t
Journal::FlushLogAndBlocks()
{
	return _FlushLog(true, true);
}


/*!	Locks the journal, in addition to flushing the log and blocks. A return
	value of \c B_OK indicates that the operation was successful, and that
	the journal is locked.
*/
status_t
Journal::FlushLogAndLockJournal()
{
	status_t status = Lock(NULL, true);
	if (status != B_OK)
		return status;

	status = _FlushLog(true, true, true);

	if (status != B_OK)
		recursive_lock_unlock(&fLock);

	return status;
}


status_t
Journal::Lock(Transaction* owner, bool separateSubTransactions)
{
	status_t status = recursive_lock_lock(&fLock);
	if (status != B_OK)
		return status;

	if (!fSeparateSubTransactions && recursive_lock_get_recursion(&fLock) > 1) {
		// we'll just use the current transaction again
		return B_OK;
	}

	if (separateSubTransactions)
		fSeparateSubTransactions = true;

	if (owner != NULL)
		owner->SetParent(fOwner);

	fOwner = owner;

	// TODO: we need a way to find out how big the current transaction is;
	//	we need to be able to either detach the latest sub transaction on
	//	demand, as well as having some kind of fall back plan in case the
	//	sub transaction itself grows bigger than the log.
	//	For that, it would be nice to have some call-back interface in the
	//	cache transaction API...

	if (fOwner != NULL) {
		if (fUnwrittenTransactions > 0) {
			// start a sub transaction
			cache_start_sub_transaction(fVolume->BlockCache(), fTransactionID);
			fHasSubtransaction = true;
		} else
			fTransactionID = cache_start_transaction(fVolume->BlockCache());

		if (fTransactionID < B_OK) {
			recursive_lock_unlock(&fLock);
			return fTransactionID;
		}

		cache_add_transaction_listener(fVolume->BlockCache(), fTransactionID,
			TRANSACTION_IDLE, _TransactionIdle, this);
	}
	return B_OK;
}


status_t
Journal::Unlock(Transaction* owner, bool success)
{
	if (fSeparateSubTransactions || recursive_lock_get_recursion(&fLock) == 1) {
		// we only end the transaction if we would really unlock it
		// TODO: what about failing transactions that do not unlock?
		// (they must make the parent fail, too)
		if (owner != NULL) {
			status_t status = _TransactionDone(success);
			if (status != B_OK)
				return status;

			// Unlocking the inodes might trigger new transactions, but we
			// cannot reuse the current one anymore, as this one is already
			// closed.
			bool separateSubTransactions = fSeparateSubTransactions;
			fSeparateSubTransactions = true;
			owner->NotifyListeners(success);
			fSeparateSubTransactions = separateSubTransactions;

			fOwner = owner->Parent();
		} else
			fOwner = NULL;

		fTimestamp = system_time();

		if (fSeparateSubTransactions
			&& recursive_lock_get_recursion(&fLock) == 1)
			fSeparateSubTransactions = false;
	} else
		owner->MoveListenersTo(fOwner);

	recursive_lock_unlock(&fLock);
	return B_OK;
}


uint32
Journal::_TransactionSize() const
{
	int32 count = cache_blocks_in_transaction(fVolume->BlockCache(),
		fTransactionID);
	if (count <= 0)
		return 0;

	// take the number of array blocks in this transaction into account
	uint32 maxRuns = run_array::MaxRuns(fVolume->BlockSize());
	uint32 arrayBlocks = (count + maxRuns - 1) / maxRuns;
	return count + arrayBlocks;
}


status_t
Journal::_TransactionDone(bool success)
{
	if (!success) {
		if (_HasSubTransaction()) {
			cache_abort_sub_transaction(fVolume->BlockCache(), fTransactionID);
			// We can continue to use the parent transaction afterwards
		} else {
			cache_abort_transaction(fVolume->BlockCache(), fTransactionID);
			fUnwrittenTransactions = 0;
		}

		return B_OK;
	}

	// Up to a maximum size, we will just batch several
	// transactions together to improve speed
	uint32 size = _TransactionSize();
	if (size < fMaxTransactionSize) {
		// Flush the log from time to time, so that we have enough space
		// for this transaction
		if (size > FreeLogBlocks())
			cache_sync_transaction(fVolume->BlockCache(), fTransactionID);

		fUnwrittenTransactions++;
		return B_OK;
	}

	return _WriteTransactionToLog();
}


status_t
Journal::MoveLog(block_run newLog)
{
	block_run oldLog = fVolume->Log();
	if (newLog == oldLog)
		return B_OK;

	off_t newEnd = newLog.Start() + newLog.Length();
	off_t oldEnd = oldLog.Start() + oldLog.Length();

	// make sure the new log position is ok
	if (newLog.AllocationGroup() != 0)
		return B_BAD_VALUE;

	if (fVolume->ValidateBlockRun(newLog) != B_OK)
		return B_BAD_VALUE;

	if (newLog.Start() < 1 + fVolume->NumBitmapBlocks())
		return B_BAD_VALUE;

	if (newEnd > fVolume->NumBlocks())
		return B_BAD_VALUE;

	status_t status;
	block_run allocatedRun = {};

	BlockAllocator& allocator = fVolume->Allocator();

	// allocate blocks if necessary
	if (newEnd > oldEnd) {
		if (oldEnd > newLog.Start())
			allocatedRun.SetTo(newLog.AllocationGroup(), oldEnd, newEnd - oldEnd);
		else
			allocatedRun = newLog;

		Transaction transaction(fVolume, 0);

		status = allocator.AllocateBlockRun(transaction, allocatedRun);
		if (status != B_OK) {
			FATAL(("MoveLog: Could not allocate space to move log area!\n"));
			return status;
		}

		status = transaction.Done();
		if (status != B_OK)
			return status;
	}

	MutexLocker volumeLock(fVolume->Lock());

	status = FlushLogAndLockJournal();
	if (status != B_OK)
		return status;

	// update references to the log location and size
	fVolume->SuperBlock().log_blocks = newLog;
	status = fVolume->WriteSuperBlock();
	if (status != B_OK) {
		fVolume->SuperBlock().log_blocks = oldLog;

		Unlock(NULL, true);

		// if we had to allocate some blocks, try to free them
		if (!allocatedRun.IsZero()) {
			Transaction transaction(fVolume, 0);
			status_t freeStatus = allocator.Free(transaction, allocatedRun);
			if (freeStatus == B_OK)
				freeStatus = transaction.Done();

			// don't really care if we fail
			if (freeStatus != B_OK)
				REPORT_ERROR(freeStatus);
		}

		return status;
	}

	fLogSize = newLog.Length();
	fMaxTransactionSize = fLogSize / 2 - 5;

	Unlock(NULL, true);
	volumeLock.Unlock();

	// at this point, the log is moved and functional in its new location

	// free blocks if necessary
	if (newEnd < oldEnd) {
		block_run runToFree = block_run::Run(0, newEnd, oldEnd - newEnd);

		Transaction transaction(fVolume, 0);

		status = allocator.Free(transaction, runToFree);
		if (status == B_OK)
			status = transaction.Done();

		// we've already moved the log, no sense in failing just because we
		// couldn't free a couple of blocks
		if (status != B_OK)
			REPORT_ERROR(status);
	}

	return B_OK;
}


//	#pragma mark - debugger commands


#ifdef BFS_DEBUGGER_COMMANDS


void
Journal::Dump()
{
	kprintf("Journal %p\n", this);
	kprintf("  log start:            %" B_PRId32 "\n", fVolume->LogStart());
	kprintf("  log end:              %" B_PRId32 "\n", fVolume->LogEnd());
	kprintf("  owner:                %p\n", fOwner);
	kprintf("  log size:             %" B_PRIu32 "\n", fLogSize);
	kprintf("  max transaction size: %" B_PRIu32 "\n", fMaxTransactionSize);
	kprintf("  used:                 %" B_PRIu32 "\n", fUsed);
	kprintf("  unwritten:            %" B_PRId32 "\n", fUnwrittenTransactions);
	kprintf("  timestamp:            %" B_PRId64 "\n", fTimestamp);
	kprintf("  transaction ID:       %" B_PRId32 "\n", fTransactionID);
	kprintf("  has subtransaction:   %d\n", fHasSubtransaction);
	kprintf("  separate sub-trans.:  %d\n", fSeparateSubTransactions);
	kprintf("entries:\n");
	kprintf("  address        id  start length\n");

	LogEntryList::Iterator iterator = fEntries.GetIterator();

	while (iterator.HasNext()) {
		LogEntry* entry = iterator.Next();

		kprintf("  %p %6" B_PRId32 " %6" B_PRIu32 " %6" B_PRIu32 "\n", entry,
			entry->TransactionID(), entry->Start(), entry->Length());
	}
}


int
dump_journal(int argc, char** argv)
{
	if (argc != 2 || !strcmp(argv[1], "--help")) {
		kprintf("usage: %s <ptr-to-volume>\n", argv[0]);
		return 0;
	}

	Volume* volume = (Volume*)parse_expression(argv[1]);
	Journal* journal = volume->GetJournal(0);

	journal->Dump();
	return 0;
}


#endif	// BFS_DEBUGGER_COMMANDS


//	#pragma mark - TransactionListener


TransactionListener::TransactionListener()
{
}


TransactionListener::~TransactionListener()
{
}


//	#pragma mark - Transaction


status_t
Transaction::Start(Volume* volume, off_t refBlock)
{
	// has it already been started?
	if (fJournal != NULL)
		return B_OK;

	fJournal = volume->GetJournal(refBlock);
	if (fJournal != NULL && fJournal->Lock(this, false) == B_OK)
		return B_OK;

	fJournal = NULL;
	return B_ERROR;
}


void
Transaction::AddListener(TransactionListener* listener)
{
	if (fJournal == NULL)
		panic("Transaction is not running!");

	fListeners.Add(listener);
}


void
Transaction::RemoveListener(TransactionListener* listener)
{
	if (fJournal == NULL)
		panic("Transaction is not running!");

	fListeners.Remove(listener);
	listener->RemovedFromTransaction();
}


void
Transaction::NotifyListeners(bool success)
{
	while (TransactionListener* listener = fListeners.RemoveHead()) {
		listener->TransactionDone(success);
		listener->RemovedFromTransaction();
	}
}


/*!	Move the inodes into the parent transaction. This is needed only to make
	sure they will still be reverted in case the transaction is aborted.
*/
void
Transaction::MoveListenersTo(Transaction* transaction)
{
	while (TransactionListener* listener = fListeners.RemoveHead()) {
		transaction->fListeners.Add(listener);
	}
}
