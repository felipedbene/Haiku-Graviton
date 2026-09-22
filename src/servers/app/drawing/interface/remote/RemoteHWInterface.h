/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef REMOTE_HW_INTERFACE_H
#define REMOTE_HW_INTERFACE_H

#include "HWInterface.h"
#include "NetReceiver.h"
#include "RemoteWireWriter.h"

#include <AutoDeleter.h>
#include <Locker.h>
#include <ObjectList.h>
#include <Path.h>

class BNetEndpoint;
class StreamingRingBuffer;
class NetSender;
class RemoteDrawingEngine;
class RemoteEventStream;
class RemoteMessage;

struct callback_info;


class RemoteHWInterface : public HWInterface {
public:
									RemoteHWInterface(const char* target);
virtual								~RemoteHWInterface();

virtual	status_t					Initialize();
virtual	status_t					Shutdown();

virtual	DrawingEngine*				CreateDrawingEngine();
virtual	EventStream*				CreateEventStream();

virtual	status_t					SetMode(const display_mode& mode);
virtual	void						GetMode(display_mode* mode);
virtual	status_t					GetPreferredMode(display_mode* mode);

virtual status_t					GetDeviceInfo(accelerant_device_info* info);

virtual status_t					GetModeList(display_mode** _modeList,
										uint32* _count);
virtual status_t					GetPixelClockLimits(display_mode* mode,
										uint32* _low, uint32* _high);
virtual status_t					GetTimingConstraints(
										display_timing_constraints* constraints);
virtual status_t					ProposeMode(display_mode* candidate,
										const display_mode* low,
										const display_mode* high);

virtual sem_id						RetraceSemaphore();
virtual status_t					WaitForRetrace(
										bigtime_t timeout = B_INFINITE_TIMEOUT);

virtual status_t					SetDPMSMode(uint32 state);
virtual uint32						DPMSMode();
virtual uint32						DPMSCapabilities();

virtual status_t			SetBrightness(float);
virtual status_t			GetBrightness(float*);

		// cursor handling
virtual	void						SetCursor(ServerCursor* cursor);
virtual	void						SetCursorVisible(bool visible);
virtual	void						MoveCursorTo(float x, float y);
virtual	void						SetDragBitmap(const ServerBitmap* bitmap,
										const BPoint& offsetFormCursor);

		// frame buffer access
virtual	RenderingBuffer*			FrontBuffer() const;
virtual	RenderingBuffer*			BackBuffer() const;
virtual	bool						IsDoubleBuffered() const;

virtual	status_t					InvalidateRegion(const BRegion& region);
virtual	status_t					Invalidate(const BRect& frame);
virtual	status_t					CopyBackToFront(const BRect& frame);

		// drawing engine interface
		StreamingRingBuffer*		ReceiveBuffer()
										{ return fReceiveBuffer.Get(); }
			// Outbound messages go through the wire writer, not straight into
			// the ring buffer: it owns the negotiated compression state and the
			// guarantee that messages reach the socket in the order they were
			// compressed.
		RemoteWireWriter*			SendBuffer() { return fWireWriter.Get(); }

			// True once a client has attached and announced its display mode.
			// A synchronous drawing call (DrawString, StringWidth, ReadBitmap)
			// must not wait on a client reply when this is false: with no client
			// draining the send buffer there is nothing to answer, and the wait
			// would just burn its whole timeout on every call -- the headless
			// stall (1 s per string, 10 s per screenshot).
			bool						IsConnected() const
											{ return fIsConnected; }

			// The URP/1 capability bits the connected client advertised in
			// RP_HELLO, masked to what this server understands. Zero for a
			// pre-handshake client. The server may only use a feature set here.
			uint32						ClientCapabilities() const
											{ return fClientCapabilities; }

typedef bool (*CallbackFunction)(void* cookie, RemoteMessage& message);

		status_t					AddCallback(uint32 token,
										CallbackFunction callback,
										void* cookie);
		bool						RemoveCallback(uint32 token);

		// A remote drawing engine outlives individual client connections,
		// so it must register here to be told when a fresh client attaches
		// and its cached, client-side drawing state has to be re-established.
		void						RegisterDrawingEngine(
										RemoteDrawingEngine* engine);
		void						UnregisterDrawingEngine(
										RemoteDrawingEngine* engine);

private:
		callback_info*				_FindCallback(uint32 token);
static	int							_CallbackCompare(const uint32* key,
										const callback_info* info);

static	int32						_EventThreadEntry(void* data);
		status_t					_EventThread();

static	status_t					_NewConnectionCallback(void *cookie,
										BNetEndpoint &endpoint);
		status_t					_NewConnection(BNetEndpoint &endpoint);

static	void						_ConnectionClosedCallback(void *cookie);
		void						_ConnectionClosed();

		void						_Disconnect();

		status_t					_MintSessionCookie();
		void						_RemoveSessionCookie();

		void						_FillDisplayModeTiming(display_mode &mode);

		const char*					fTarget;
		status_t					fInitStatus;
		bool						fIsConnected;
		uint32						fProtocolVersion;
		uint32						fClientProtocolVersion;
		uint32						fClientCapabilities;
		uint32						fConnectionSpeed;
		display_mode				fFallbackMode;
		display_mode				fCurrentMode;
		display_mode				fClientMode;
		uint16						fListenPort;

		// The per-listener secret a connection has to present before it may
		// become the session, and the owner-only file it is published in for
		// the broker and the test instruments to read. Minted before the
		// listening socket exists, so there is never a listener without one,
		// and removed when the listener goes away.
		char						fSessionCookie[kMaxSessionCookieLength];
		size_t						fSessionCookieLength;
		BPath						fSessionCookiePath;

		ObjectDeleter<BNetEndpoint>	fListenEndpoint;
		ObjectDeleter<StreamingRingBuffer>
									fSendBuffer;
		ObjectDeleter<RemoteWireWriter>
									fWireWriter;
		ObjectDeleter<StreamingRingBuffer>
									fReceiveBuffer;

		ObjectDeleter<NetSender>	fSender;
		ObjectDeleter<NetReceiver>	fReceiver;

		thread_id					fEventThread;
		ObjectDeleter<RemoteEventStream>
									fEventStream;

		BLocker						fCallbackLocker;
		BObjectList<callback_info>	fCallbacks;

		BLocker						fEngineListLocker;
		BObjectList<RemoteDrawingEngine>
									fDrawingEngines;
};

#endif // REMOTE_HW_INTERFACE_H
