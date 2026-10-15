/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_ACK_ESTIMATOR_H
#define REMOTE_ACK_ESTIMATOR_H

#ifdef REMOTE_ACK_ESTIMATOR_TEST
#	include "AckEstimatorTestDefs.h"
#else
#	include <SupportDefs.h>
#endif


/*!	Server-side estimator fed by RP_FRAME_ACK: what the link costs, and how many
	frames the client still owes an answer for.

	Two quantities come out of it, and both are deliberately *not* the obvious
	measurement.

	**Round-trip time, with the client's held time removed.** The naive estimate
	is "ack arrived minus frame sent", and it is wrong by however long the client
	sat on the frame before answering. That is not a small correction: a client
	in a backgrounded browser tab has its timers clamped to one second, so the
	naive number reports a one-second link on a LAN and any pacer built on it
	throttles a session whose network is idle. RP_FRAME_ACK therefore carries
	held_ms -- the client's own measure of the whole interval from "this frame's
	last byte arrived" to "this ack went to the socket" -- and this class
	subtracts it, leaving the two one-way link delays plus the server's own
	scheduling.

	held_ms *contains* decode_ms (decoding is part of holding the frame), so
	exactly one of them is subtracted. Subtracting both would double-count the
	decode and bias every estimate low -- toward a link that looks better than it
	is, which is the dangerous direction for a pacer. decode_ms and queue_depth
	are recorded here for the pacer that lands with Tier P, and are not inputs to
	the RTT.

	A client can also report a held_ms larger than the entire interval the server
	measured, through clock skew or simple invention. That sample is discarded
	rather than clamped to zero: a zero-latency sample folded into the estimate
	is a wrong number presented as a measurement, where a dropped one just leaves
	the previous estimate standing.

	**Backpressure, counted in frames actually sent.** The number that says "the
	client is not keeping up" is OutstandingFrames(): frames this server handed
	to the wire and has not had an ack for. It is NOT the distance between the
	current frame id and the last acked one, which is a different quantity in
	this tree and a misleading one:

	  - capture here is damage-gated, so an idle screen sends nothing at all. No
	    frames out means no acks in, and any signal derived from elapsed time or
	    from a frame-id gap flags a perfectly healthy client for the crime of
	    looking at a still screen. Counted in frames sent, an idle screen cannot
	    produce a signal, because the count is zero;
	  - frame ids are minted per composed frame and are monotonic for the life of
	    the interface, not per connection, and some of them are ids no client
	    will ever quote back: a frame whose bytes RemoteFlowQueue superseded
	    consumed an id and is never acked itself. (Frames composed while no
	    client is attached cost no ids in this tree, because
	    RemoteHWInterface::_EndFrame() returns on the RP_CAP_FRAME_BOUNDARY test
	    before minting one -- but that is one `if` away from changing, and
	    nothing here should depend on it.) A client that attaches once the
	    interface has reached id 50000 is 50000 ids away from an unacked
	    "frame 0" it never saw.

	An ack is cumulative, as it can be on an ordered reliable stream: an ack for
	frame N retires every frame sent before N as well. That is also what heals
	the residue of the second case above -- an id whose bytes the flow queue
	dropped is never acked itself, and is retired by the next frame that is.

	Not internally locked, and allocation-free: the frames in flight live in a
	fixed ring. The owner serialises (RemoteHWInterface holds fFrameAckLocker
	across both calls, since frames are sent from drawing threads and acks arrive
	on the event thread), so the estimator stays pure arithmetic that is
	unit-tested off-target against a synthetic ack stream.
*/
class RemoteAckEstimator {
public:
			/*!	Frames in flight the ring remembers. Beyond this the oldest is
				forgotten, which only loses the ability to time *that* frame's
				ack; the count of outstanding frames stays exact as a
				saturating one, and the cumulative retirement below keeps it
				from drifting.

				64 is far above a healthy session: an ack-per-frame client at
				60 fps has one or two frames in flight, and a client that has
				fallen 64 composed frames behind has been signalling
				backpressure for about a second already. */
	static	const size_t		kMaxTracked = 64;

			/*!	Outstanding frames at which the client is behind.

				An ack-per-frame client in a healthy session shows one
				outstanding frame (the one in the network) and two across a
				boundary, when the next frame is composed before the previous
				ack lands. Three means the server composed a third frame while
				two were still unanswered, i.e. the client is not consuming at
				the rate the server is producing.

				A constant is honest only for the short links this serves today.
				An ack-per-frame client legitimately pipelines about one frame
				per RoundTripTime() per frame interval, so at 60 fps a 50 ms
				WAN client sits at three in flight while perfectly healthy. The
				pacer that eventually acts on this should derive its threshold
				from the estimate rather than from here; until something acts on
				it, a signal is a statistic. */
	static	const uint32		kBackpressureFrames = 3;

			//! Weight of one new sample in the smoothed RTT, as 1/kRoundTripGain.
	static	const bigtime_t		kRoundTripGain = 8;

								RemoteAckEstimator();

			/*!	Connection boundary: forget the frames in flight and the
				estimate. Both belong to the connection -- a new client has
				acked nothing, and the previous link's RTT is not evidence
				about this one. The counters are cumulative for the life of the
				interface and are deliberately kept. */
			void				Reset();

			/*!	Records that the frame identified by \a frameId was emitted to
				the client at \a when (server monotonic µs).

				Called for every frame boundary handed to the wire, which is as
				close to "the client can answer this" as this end can get: a
				boundary whose bytes RemoteFlowQueue goes on to supersede is
				counted here and is never acked itself, and is retired by the
				cumulative ack of the next frame that is. Do not call it for
				anything weaker than a boundary that reached the wire writer --
				this count *is* the backpressure signal, so a frame counted
				here that the client was never in a position to acknowledge is
				a frame it appears to owe until some later ack clears it. */
			void				NoteFrameSent(uint32 frameId, bigtime_t when);

			enum ack_result {
				//! Timed, and the frame and its predecessors are retired.
				ACK_ACCEPTED,

				//! No such frame in flight: a duplicate, an ack from a
				//! previous connection, or an id this server never sent.
				//! Nothing is retired and nothing is timed.
				ACK_UNKNOWN_FRAME,

				//! The frame is retired, but the client's held_ms exceeded the
				//! interval the server measured, so there is no usable sample.
				ACK_HELD_IMPLAUSIBLE
			};

			/*!	Feeds one RP_FRAME_ACK, received at \a when (server monotonic
				µs), with the payload fields exactly as they came off the wire.
				See RemoteProtocol.h for their definitions. */
			ack_result			NoteAck(uint32 frameId, uint16 decodeMillis,
									uint8 queueDepth, uint16 heldMillis,
									bigtime_t when);

			//! Whether any ack has yielded a usable link sample yet.
			bool				HasEstimate() const { return fHasEstimate; }

			//! Smoothed link round-trip time in µs, held_ms removed. Zero
			//! until HasEstimate().
			bigtime_t			RoundTripTime() const
									{ return fRoundTrip; }

			//! The most recent single link sample, µs.
			bigtime_t			LastRoundTripTime() const
									{ return fLastRoundTrip; }

			//! The smallest link sample seen on this connection, µs. The least
			//! contaminated estimate of the link's own delay.
			bigtime_t			MinimumRoundTripTime() const
									{ return fMinimumRoundTrip; }

			//! Frames sent and not yet acknowledged. The backpressure input.
			uint32				OutstandingFrames() const
									{ return (uint32)fTrackedCount; }

			bool				BackpressureSignalled() const
									{ return OutstandingFrames()
										>= kBackpressureFrames; }

			// Last reported client-side costs. Recorded, not acted on: the
			// pacer that consumes them arrives with Tier P's encoder.
			uint16				LastDecodeMillis() const
									{ return fLastDecodeMillis; }
			uint8				LastQueueDepth() const
									{ return fLastQueueDepth; }
			uint16				LastHeldMillis() const
									{ return fLastHeldMillis; }

			void				GetStatistics(uint64& _sent, uint64& _accepted,
									uint64& _unknown, uint64& _implausible,
									uint64& _forgotten) const;

private:
			struct sent_frame {
				uint32			id;
				bigtime_t		when;
			};

			//! Index of \a frameId among the frames in flight, or -1.
			int					_IndexOf(uint32 frameId) const;

			//! Retires the frames in flight up to and including \a index.
			void				_Retire(int index);

			sent_frame			fTracked[kMaxTracked];
			size_t				fTrackedCount;

			bool				fHasEstimate;
			bigtime_t			fRoundTrip;
			bigtime_t			fLastRoundTrip;
			bigtime_t			fMinimumRoundTrip;

			uint16				fLastDecodeMillis;
			uint8				fLastQueueDepth;
			uint16				fLastHeldMillis;

			uint64				fSent;
			uint64				fAccepted;
			uint64				fUnknown;
			uint64				fImplausible;
			uint64				fForgotten;
};

#endif	// REMOTE_ACK_ESTIMATOR_H
