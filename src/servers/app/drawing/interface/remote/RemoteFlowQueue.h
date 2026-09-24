/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_FLOW_QUEUE_H
#define REMOTE_FLOW_QUEUE_H

#ifdef REMOTE_FLOW_QUEUE_TEST
#	include "FlowQueueTestDefs.h"
#else
#	include <SupportDefs.h>
#endif


/*!	URP/1 outbound flow control: a bounded, op-aware queue of whole messages.

	This is the M2 replacement for "discard when nobody listens". The send ring
	is constructed with discardWithoutReader, so before this queue existed a
	Flush() with no client attached *succeeded having written nothing* -- which
	is how server state came to be believed-sent-but-never-delivered, the server
	half of the reconnect black screen. Discarding is not itself wrong: nothing
	can hold an unbounded stream for a client that may never arrive. Discarding
	*silently* is. So this queue's contract is not "nothing is ever lost", it is:

		everything that is dropped is dropped by a rule, and any drop that
		cannot be proven invisible forces an RP_RESYNC.

	Three properties are load-bearing, in this order.

	**Messages, never bytes.** Every entry is one complete framed RP message
	(uint16 code, uint32 totalLength, payload). Nothing in this class can emit
	or retain a fragment of one. That is not a preference: the framer has no
	resync point inside a message, so half a message on the wire desynchronises
	the peer for the rest of the connection and no later message repairs it.
	PeekFront()/PopFront() hand out one whole message at a time and the caller
	may only pop what it wholly wrote.

	**Coalesce within a frame; drop only superseded whole frames.** See
	_Coalesce() and _Supersede() for the two rules and Classify() for the
	per-opcode classification they run on. An opcode this class has never heard
	of is PINNED, so a future opcode is safe by default and only becomes
	droppable when someone states that it is.

	**Bounded, and the bound degrades to RP_RESYNC.** At kMaxMessages or
	kMaxBytes the queue coalesces, then supersedes, and if it is still over the
	bound it drops everything and latches ResyncOwed(). The owner must then send
	an RP_RESYNC barrier and replay state before the stream is trustworthy
	again; a screen that is merely a frame behind is acceptable, a screen that is
	wrong is not.

	Not internally locked. The caller serialises (RemoteWireWriter::fLock already
	serialises every writer, and the compressed stream requires that anyway), so
	the policy stays pure logic that can be unit-tested off-target.
*/
class RemoteFlowQueue {
public:
			/*!	What may be done to a message. The classification is the whole
				safety argument, so it is stated per opcode in Classify() rather
				than inferred from ranges. */
			enum op_class {
				//! Pure pixel producer confined to its frame's declared damage.
				//! May be dropped, but only as part of a superseded frame.
				OP_DROPPABLE,

				//! Durable client-side state that a later message of the same
				//! opcode and token fully overwrites. May be coalesced away by
				//! that later message; may never be dropped outright, because
				//! nothing would re-send it (the engine dedups).
				OP_COALESCABLE,

				//! Reads the client's surface, so every pixel written before it
				//! is an input to it. Nothing older than a barrier may be
				//! dropped.
				OP_BARRIER,

				//! Session, state lifecycle, round-trip queries and replies,
				//! anything unrecognised. Never dropped, never coalesced.
				OP_PINNED
			};

	static	op_class			Classify(uint16 code);

			/*!	Bound, in messages and in bytes. Both are hard: whichever is
				reached first runs the overflow policy.

				4 MiB is ~128 cold first paints (measured: ~32 kB) and ~265 s of
				the heaviest steady interactive stream this tree has measured
				(15.8 kB/s, text typing). 32768 messages is ~30 s of the highest
				message rate at that byte rate. The pair is deliberately far
				above anything a healthy session produces: the queue exists for
				a reader that stopped, not for ordinary burstiness, and a bound
				that a real session can reach is a bound that turns latency into
				corruption-adjacent policy for no reason. */
	static	const size_t		kMaxMessages = 32768;
	static	const size_t		kMaxBytes = 4 * 1024 * 1024;

								RemoteFlowQueue(size_t maxMessages = kMaxMessages,
									size_t maxBytes = kMaxBytes);
								~RemoteFlowQueue();

			/*!	Whether RP_TIER_END_FRAME is the frame boundary (the client
				negotiated RP_CAP_FRAME_BOUNDARY) or RP_INVALIDATE_RECT /
				RP_INVALIDATE_REGION is (it did not). Both are the same instant
				in the same thread's stream; the explicit one exists so Tier P
				and any client that wants to double-buffer per frame can see it,
				and so the boundary is a stated part of the protocol rather than
				an inference. */
			void				SetExplicitBoundaries(bool explicitBoundaries);
			bool				ExplicitBoundaries() const
									{ return fExplicitBoundaries; }

			/*!	Updates the tracking state this class needs from a message that
				is NOT being queued -- i.e. one that went straight to the ring.
				Must be called for every outbound message, queued or not, or the
				per-token drawing mode below goes stale and the queue silently
				becomes conservative (or, worse, optimistic).

				Returns nothing and cannot fail: it only reads. */
			void				Observe(const void* message, size_t length);

			/*!	Copies one whole framed message in, applying the overflow policy
				first if it would not fit. \a owner identifies the emitting
				thread; frames are per-emitter (see _Supersede() for why).

				Returns B_OK, or B_NO_MEMORY when the copy itself failed -- in
				which case nothing was stored and the queue has latched
				ResyncOwed(), because a message the caller believed queued and
				that is not here is exactly the silent loss this class exists to
				prevent. */
			status_t			Enqueue(const void* message, size_t length,
									int32 owner);

			//! The oldest queued message, or NULL when empty. Not popped.
			const uint8*		PeekFront(size_t& _length) const;

			//! Discards the oldest queued message. Only legal once it has been
			//! wholly written.
			void				PopFront();

			bool				IsEmpty() const { return fCount == 0; }
			size_t				CountMessages() const { return fCount; }
			size_t				CountBytes() const { return fBytes; }

			/*!	Drops everything queued and latches ResyncOwed(). The only
				discard in this class that is allowed to lose content whose
				redelivery nobody can prove. */
			void				CollapseToResync();

			bool				ResyncOwed() const { return fResyncOwed; }
			void				ClearResyncOwed() { fResyncOwed = false; }

			/*!	Connection boundary: forget everything, including the per-token
				drawing modes (a new connection's client knows nothing) and the
				owed resync (the new connection replays state unconditionally).*/
			void				Reset();

			void				GetStatistics(uint64& _enqueued, uint64& _drained,
									uint64& _coalesced, uint64& _superseded,
									uint64& _collapses) const;

			// Exposed for the unit test; cheap enough to leave in.
			size_t				CountFrames() const;
			bool				FrameIsClose(uint16 code) const;

private:
			struct rect {
				float			left;
				float			top;
				float			right;
				float			bottom;
			};

			struct entry {
				uint8*			data;
				size_t			length;
				uint16			code;
				uint8			klass;
				int32			owner;
				uint32			frame;		// per-owner frame sequence
				uint32			token;		// 0 when the op carries none
				bool			hasToken;
			};

			bool				_Grow(size_t needed);
			void				_Remove(size_t index);
			bool				_EnforceBound(size_t incoming);
			size_t				_Coalesce();
			size_t				_Supersede();
			size_t				_SupersedeOwner(int32 owner, long lastBarrier);

			uint32				_FrameOf(int32 owner) const;
			void				_CloseFrame(int32 owner);
			int					_ModeOf(uint32 token) const;
			void				_NoteMode(uint32 token, int mode);

	static	bool				_ReadToken(const uint8* data, size_t length,
									uint32& _token);
	static	bool				_ReadDamage(const uint8* data, size_t length,
									uint16 code, rect* rects, size_t capacity,
									size_t& _count);
	static	bool				_IsIntegral(const rect& r);
	static	bool				_Covers(const rect* outer, size_t outerCount,
									const rect* inner, size_t innerCount);

			entry*				fEntries;
			size_t				fCapacity;
			size_t				fCount;
			size_t				fBytes;

			size_t				fMaxMessages;
			size_t				fMaxBytes;

			bool				fExplicitBoundaries;
			bool				fResyncOwed;

			struct owner_state {
				int32			owner;
				uint32			frame;
			};
			owner_state*		fOwners;
			size_t				fOwnerCapacity;
			size_t				fOwnerCount;

			struct token_state {
				uint32			token;
				int				mode;
			};
			token_state*		fTokens;
			size_t				fTokenCapacity;
			size_t				fTokenCount;

			uint64				fEnqueued;
			uint64				fDrained;
			uint64				fCoalesced;
			uint64				fSuperseded;
			uint64				fCollapses;
};

#endif	// REMOTE_FLOW_QUEUE_H
