/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "RemoteHWInterface.h"
#include "RemoteDrawingEngine.h"
#include "RemoteEventStream.h"
#include "RemoteMessage.h"

#include "NetReceiver.h"
#include "NetSender.h"
#include "StreamingRingBuffer.h"

#include "SystemPalette.h"

#include <Autolock.h>
#include <NetEndpoint.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <new>
#include <string.h>


#define TRACE(x...)				/*debug_printf("RemoteHWInterface: " x)*/
#define TRACE_ALWAYS(x...)		debug_printf("RemoteHWInterface: " x)
#define TRACE_ERROR(x...)		debug_printf("RemoteHWInterface: " x)


// The URP/1 capabilities this server implements. A client's advertised feature
// bitmap is masked to this in RP_HELLO, so an unknown or not-yet-implemented bit
// the client offers is simply not negotiated. Not a constant, because wire
// compression is only on offer when this build actually has a compressor:
// RemoteWireWriter::SupportedCapabilities() is empty without the zstd build
// feature, and offering a capability the server cannot honour would leave the
// client waiting for segments that never come.
static uint32
supported_capabilities()
{
	return RP_CAP_STRING_WIDTH_REPLY | RemoteWireWriter::SupportedCapabilities();
}


struct callback_info {
	uint32				token;
	RemoteHWInterface::CallbackFunction	callback;
	void*				cookie;
};


/*!	Whether the IPv4 address \a networkOrderAddress (network byte order) is a
	loopback, RFC 1918 private, or link-local address -- the only addresses
	the unauthenticated remote protocol may be bound to. Notably false for
	INADDR_ANY and for every publicly routable address.
*/
static bool
is_loopback_or_private_address(uint32 networkOrderAddress)
{
	uint32 address = ntohl(networkOrderAddress);
	uint8 first = address >> 24;
	uint8 second = (address >> 16) & 0xff;

	if (first == 127)
		return true;
	if (first == 10)
		return true;
	if (first == 172 && second >= 16 && second <= 31)
		return true;
	if (first == 192 && second == 168)
		return true;
	if (first == 169 && second == 254)
		return true;

	return false;
}


RemoteHWInterface::RemoteHWInterface(const char* target)
	:
	HWInterface(),
	fTarget(target),
	fIsConnected(false),
	fProtocolVersion(100),
	fClientProtocolVersion(0),
	fClientCapabilities(0),
	fConnectionSpeed(0),
	fListenPort(10901),
	fListenEndpoint(NULL),
	fSendBuffer(NULL),
	fWireWriter(NULL),
	fReceiveBuffer(NULL),
	fSender(NULL),
	fReceiver(NULL),
	fEventThread(-1),
	fEventStream(NULL),
	fCallbackLocker("callback locker"),
	fEngineListLocker("engine list locker")
{
	memset(&fFallbackMode, 0, sizeof(fFallbackMode));
	fFallbackMode.virtual_width = 640;
	fFallbackMode.virtual_height = 480;
	fFallbackMode.space = B_RGB32;
	_FillDisplayModeTiming(fFallbackMode);

	fCurrentMode = fClientMode = fFallbackMode;

	// The target is "<port>", which binds loopback -- the only form anything
	// in the tree uses -- or the deliberately awkward opt-in
	// "unsafe-bind:<IPv4-address>:<port>" for serving a trusted private
	// segment directly.
	//
	// The opt-in is spelled that way on purpose. The remote protocol has no
	// authentication and no encryption of any kind: anything that can reach
	// the port gets full control of the session, including every keystroke.
	// And "private address" is NOT a routability property on a cloud
	// instance -- an EC2 instance's only NIC address is RFC 1918 with a
	// 1:1-NAT public address in front of it, so binding "the private
	// address" there binds the internet-facing interface. There is no
	// address this protocol is safe to bind but loopback; non-loopback
	// access belongs to the TLS-terminating, authenticating remote_broker
	// daemon. The escape hatch exists only for a genuinely isolated segment
	// and must be typed out.
	uint32 bindAddress = htonl(INADDR_LOOPBACK);
	static const char* kUnsafePrefix = "unsafe-bind:";
	const size_t kUnsafePrefixLength = strlen(kUnsafePrefix);

	if (strncmp(fTarget, kUnsafePrefix, kUnsafePrefixLength) == 0) {
		unsigned int a, b, c, d;
		unsigned int scannedPort;
		char extra;
		if (sscanf(fTarget + kUnsafePrefixLength, "%u.%u.%u.%u:%u%c", &a, &b,
				&c, &d, &scannedPort, &extra) != 5
			|| a > 255 || b > 255 || c > 255 || d > 255 || scannedPort == 0
			|| scannedPort > 65535) {
			TRACE_ERROR("malformed target '%s'; expected "
				"unsafe-bind:<a.b.c.d>:<port>\n", fTarget);
			fInitStatus = B_BAD_VALUE;
			return;
		}

		bindAddress = htonl((a << 24) | (b << 16) | (c << 8) | d);
		fListenPort = (uint16)scannedPort;

		// Still never INADDR_ANY or a publicly routable address: those are
		// not "a trusted segment" under any reading.
		if (!is_loopback_or_private_address(bindAddress)) {
			TRACE_ERROR("refusing to bind the unauthenticated remote protocol "
				"to a public or wildcard address\n");
			fInitStatus = B_BAD_VALUE;
			return;
		}

		if (ntohl(bindAddress) >> 24 != 127) {
			TRACE_ALWAYS("WARNING: binding the UNAUTHENTICATED, UNENCRYPTED "
				"remote protocol to %s -- every peer that can reach it owns "
				"this session, including its keystrokes. On a cloud instance "
				"a private NIC address may be reachable from the internet "
				"through 1:1 NAT.\n", fTarget + kUnsafePrefixLength);
		}
	} else {
		char extra;
		if (sscanf(fTarget, "%" B_SCNu16 "%c", &fListenPort, &extra) != 1
			|| fListenPort == 0) {
			TRACE_ERROR("malformed target '%s'; expected a port number or "
				"unsafe-bind:<a.b.c.d>:<port>\n", fTarget);
			fInitStatus = B_BAD_VALUE;
			return;
		}
	}

	fListenEndpoint.SetTo(new(std::nothrow) BNetEndpoint());
	if (!fListenEndpoint.IsSet()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	// BNetAddress stores the address as it will appear in sockaddr_in, i.e.
	// in network byte order.
	BNetAddress listenAddress(bindAddress, fListenPort);
	fInitStatus = fListenEndpoint->Bind(listenAddress);
	if (fInitStatus != B_OK)
		return;

	// discardWithoutReader: the only drain of this buffer is the NetSender of
	// the currently connected client, and there may be no client for an
	// unbounded time -- before the first one arrives, and after one dies. A
	// full buffer with no drain used to block whichever thread was drawing,
	// forever; see the comment in _NewConnection().
	//
	// 1 MiB, not 16 KiB: bitmaps cross the wire as raw pixels (RP_DRAW_BITMAP),
	// so a single 256x256 RGBA32 icon is ~256 KiB. With a 16 KiB buffer any
	// draw larger than that stalled the producing app_server thread in
	// StreamingRingBuffer::Write() while the sender drained it 4 KiB at a time
	// -- a per-bitmap hitch proportional to size and link speed. A megabyte
	// covers a burst of icons plus a wallpaper tile against the drain. It is
	// allocated up front (the ring is a fixed malloc, not growable), but 1 MiB
	// per session is negligible next to the stalls it removes.
	fSendBuffer.SetTo(new(std::nothrow) StreamingRingBuffer(1 * 1024 * 1024, true));
	if (!fSendBuffer.IsSet()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	fInitStatus = fSendBuffer->InitCheck();
	if (fInitStatus != B_OK)
		return;

	// Every outbound message goes through the wire writer on its way into the
	// send buffer. Until a client negotiates compression it is a passthrough.
	fWireWriter.SetTo(new(std::nothrow) RemoteWireWriter(fSendBuffer.Get()));
	if (!fWireWriter.IsSet()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	fReceiveBuffer.SetTo(new(std::nothrow) StreamingRingBuffer(16 * 1024));
	if (!fReceiveBuffer.IsSet()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	fInitStatus = fReceiveBuffer->InitCheck();
	if (fInitStatus != B_OK)
		return;

	fReceiver.SetTo(new(std::nothrow) NetReceiver(fListenEndpoint.Get(), fReceiveBuffer.Get(),
		_NewConnectionCallback, this, _ConnectionClosedCallback));
	if (!fReceiver.IsSet()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	fEventStream.SetTo(new(std::nothrow) RemoteEventStream());
	if (!fEventStream.IsSet()) {
		fInitStatus = B_NO_MEMORY;
		return;
	}

	fEventThread = spawn_thread(_EventThreadEntry, "remote event thread",
		B_NORMAL_PRIORITY, this);
	if (fEventThread < 0) {
		fInitStatus = fEventThread;
		return;
	}

	resume_thread(fEventThread);
}


RemoteHWInterface::~RemoteHWInterface()
{
	//TODO: check order
	fReceiver.Unset();
	fReceiveBuffer.Unset();

	// Before the buffer it writes into.
	fWireWriter.Unset();

	fSendBuffer.Unset();
	fSender.Unset();

	fListenEndpoint.Unset();

	fEventStream.Unset();
}


status_t
RemoteHWInterface::Initialize()
{
	return fInitStatus;
}


status_t
RemoteHWInterface::Shutdown()
{
	_Disconnect();
	return B_OK;
}


DrawingEngine*
RemoteHWInterface::CreateDrawingEngine()
{
	return new(std::nothrow) RemoteDrawingEngine(this);
}


EventStream*
RemoteHWInterface::CreateEventStream()
{
	return fEventStream.Get();
}


status_t
RemoteHWInterface::AddCallback(uint32 token, CallbackFunction callback,
	void* cookie)
{
	BAutolock lock(fCallbackLocker);
	int32 index = fCallbacks.BinarySearchIndexByKey(token, &_CallbackCompare);
	if (index >= 0)
		return B_NAME_IN_USE;

	callback_info* info = new(std::nothrow) callback_info;
	if (info == NULL)
		return B_NO_MEMORY;

	info->token = token;
	info->callback = callback;
	info->cookie = cookie;

	fCallbacks.AddItem(info, -index - 1);
	return B_OK;
}


bool
RemoteHWInterface::RemoveCallback(uint32 token)
{
	BAutolock lock(fCallbackLocker);
	int32 index = fCallbacks.BinarySearchIndexByKey(token, &_CallbackCompare);
	if (index < 0)
		return false;

	delete fCallbacks.RemoveItemAt(index);
	return true;
}


void
RemoteHWInterface::RegisterDrawingEngine(RemoteDrawingEngine* engine)
{
	BAutolock lock(fEngineListLocker);
	fDrawingEngines.AddItem(engine);
}


void
RemoteHWInterface::UnregisterDrawingEngine(RemoteDrawingEngine* engine)
{
	BAutolock lock(fEngineListLocker);
	fDrawingEngines.RemoveItem(engine);
}


callback_info*
RemoteHWInterface::_FindCallback(uint32 token)
{
	BAutolock lock(fCallbackLocker);
	return fCallbacks.BinarySearchByKey(token, &_CallbackCompare);
}


int
RemoteHWInterface::_CallbackCompare(const uint32* key,
	const callback_info* info)
{
	if (info->token == *key)
		return 0;

	if (info->token < *key)
		return -1;

	return 1;
}


int32
RemoteHWInterface::_EventThreadEntry(void* data)
{
	return ((RemoteHWInterface*)data)->_EventThread();
}


status_t
RemoteHWInterface::_EventThread()
{
	// Read-only: no target. Cast to disambiguate against the wire-writer
	// constructor overload.
	RemoteMessage message(fReceiveBuffer.Get(), (RemoteWireWriter*)NULL);
	while (true) {
		uint16 code;
		status_t result = message.NextMessage(code);
		if (result != B_OK) {
			// This thread is the only consumer of fReceiveBuffer. If it exits,
			// nothing drains the buffer; the NetReceiver then blocks forever in
			// StreamingRingBuffer::Write() once the 16 KiB fills, which wedges
			// the accept loop -- accepted sockets pile up in CLOSE_WAIT and new
			// clients never get their RP_INIT_CONNECTION acknowledged (see
			// issue #294). So never return here: resynchronise and keep going.
			//
			// B_CANCELED is not a framing error. It is StreamingRingBuffer::
			// MakeEmpty() waking this parked reader when the receive buffer is
			// flushed at a connection boundary (_ConnectionClosed()). The buffer
			// is already empty and our framing must restart -- but we must NOT
			// flush it again here: the receiver may already be writing the next
			// client's one and only RP_INIT_CONNECTION into the ring, and a
			// second MakeEmpty() races that write and discards it, leaving the
			// remote desktop black for every client. Just drop our partial
			// framing and re-block on the next read, which then returns that
			// RP_INIT_CONNECTION.
			//
			// Any other error is a genuine framing desync (a truncated or
			// malformed message); the stream is already unusable, so empty the
			// buffer to realign to the next message boundary before continuing.
			if (result != B_CANCELED) {
				TRACE_ERROR("failed to read message from receiver, resyncing: "
					"%s\n", strerror(result));
				fReceiveBuffer->MakeEmpty();
			}
			message.Reset();
			continue;
		}

		TRACE("got message code %" B_PRIu16 " with %" B_PRIu32 " bytes\n", code,
			message.DataLeft());

		if (code >= RP_MOUSE_MOVED && code <= RP_MODIFIERS_CHANGED) {
			// an input event, dispatch to the event stream
			if (fEventStream->EventReceived(message))
				continue;
		}

		switch (code) {
			case RP_INIT_CONNECTION:
			{
				RemoteMessage reply(NULL, fWireWriter.Get());
				reply.Start(RP_INIT_CONNECTION);
				status_t result = reply.Flush();
				(void)result;
				TRACE("init connection result: %s\n", strerror(result));
				reply.Start(RP_SET_CURSOR);
				reply.AddCursor(CursorAndDragBitmap().Get());
				result = reply.Flush();
				TRACE("init connection set cursor result: %s\n", strerror(result));
				reply.Start(RP_SET_CURSOR_VISIBLE);
				reply.Add(fCursorVisible);
				result = reply.Flush();
				TRACE("init connection set cursor visible result: %s\n", strerror(result));
				BPoint position = CursorPosition();
				reply.Start(RP_MOVE_CURSOR_TO);
				reply.Add(position.x);
				reply.Add(position.y);
				result = reply.Flush();
				TRACE("init connection set cursor position result: %s\n", strerror(result));
				break;
			}

			case RP_HELLO:
			{
				// URP/1 session handshake. A capability- and version-aware
				// client sends this immediately after connecting, before it
				// draws anything; a legacy client never does and keeps working
				// on the pre-handshake path. The reply advertises only the
				// features this server understands and the client also offered,
				// so neither side ever uses a capability the other lacks.
				uint32 clientVersion = 0;
				uint32 clientCapabilities = 0;
				uint32 maxDecodeWidth = 0, maxDecodeHeight = 0;
				uint32 requestedWidth = 0, requestedHeight = 0;
				message.Read(clientVersion);
				message.Read(clientCapabilities);
				message.Read(maxDecodeWidth);
				message.Read(maxDecodeHeight);
				message.Read(requestedWidth);
				result = message.Read(requestedHeight);
				if (result != B_OK) {
					TRACE_ERROR("failed to read hello\n");
					break;
				}

				// M0 negotiates version and capabilities only; the decode
				// dimensions and requested size are read for forward
				// compatibility but not acted on yet (Tier P / resize land
				// later).
				(void)maxDecodeWidth;
				(void)maxDecodeHeight;
				(void)requestedWidth;
				(void)requestedHeight;

				fClientProtocolVersion = min_c(clientVersion,
					(uint32)RP_PROTOCOL_VERSION);
				fClientCapabilities
					= clientCapabilities & supported_capabilities();

				RemoteMessage reply(NULL, fWireWriter.Get());
				reply.Start(RP_HELLO_ACK);
				reply.Add(fClientProtocolVersion);
				reply.Add(fClientCapabilities);

				// If wire compression was negotiated it starts at the byte after
				// this acknowledgement, which is why the flush and the switch are
				// one operation: the acknowledgement must reach a client that is
				// still reading plain bytes, and a concurrent drawing op must not be
				// able to land between the two and be read as a segment header.
				reply.FlushAndEnableCompression(
					fClientCapabilities & RP_CAP_COMPRESS_ZSTD);
				break;
			}

			case RP_UPDATE_DISPLAY_MODE:
			{
				int32 width, height;
				message.Read(width);
				result = message.Read(height);
				if (result != B_OK) {
					TRACE_ERROR("failed to read display mode\n");
					break;
				}

				fIsConnected = true;
				fClientMode.virtual_width = width;
				fClientMode.virtual_height = height;
				_FillDisplayModeTiming(fClientMode);
				_NotifyScreenChanged();
				break;
			}

			case RP_GET_SYSTEM_PALETTE:
			{
				RemoteMessage reply(NULL, fWireWriter.Get());
				reply.Start(RP_GET_SYSTEM_PALETTE_RESULT);

				const color_map *map = SystemColorMap();
				uint32 count = (uint32)B_COUNT_OF(map->color_list);

				reply.Add(count);
				for (size_t i = 0; i < count; i++) {
					const rgb_color &color = map->color_list[i];
					reply.Add(color.red);
					reply.Add(color.green);
					reply.Add(color.blue);
					reply.Add(color.alpha);
				}

				break;
			}

			default:
			{
				uint32 token;
				if (message.Read(token) == B_OK) {
					callback_info* info = _FindCallback(token);
					if (info != NULL && info->callback(info->cookie, message))
						break;
				}

				TRACE_ERROR("unhandled remote event code %u\n", code);
				break;
			}
		}
	}
}


status_t
RemoteHWInterface::_NewConnectionCallback(void *cookie, BNetEndpoint &endpoint)
{
	return ((RemoteHWInterface *)cookie)->_NewConnection(endpoint);
}


status_t
RemoteHWInterface::_NewConnection(BNetEndpoint &endpoint)
{
	// Order matters. Destroying the old sender un-registers it as the reader of
	// fSendBuffer, so between here and the new NetSender below the buffer
	// discards rather than blocks. That window is what used to be permanent
	// whenever no client had connected yet: the send buffer is 16 KiB, nothing
	// drained it, and the first application to draw more than that -- Deskbar,
	// whose tray icons cross the wire as raw bitmaps -- blocked in
	// StreamingRingBuffer::Write() inside its app_server ServerWindow thread.
	// Its client-side window thread then blocked on the reply while holding the
	// window lock, its own looper port filled to capacity, and the application
	// stayed alive in `ps` having drawn nothing at all for the rest of the boot.
	fSender.Unset();

	// Empties the send buffer and puts the stream back to plain for this client
	// to renegotiate. Both happen under the wire writer's lock, so a drawing
	// thread cannot be left half way through a message when the stream restarts.
	// Safe to take that lock here because the sender is already gone: with no
	// reader registered the send buffer discards instead of blocking, so nobody
	// can be holding the lock across an unbounded wait.
	fWireWriter->Reset();

	// A fresh client has not yet sent its RP_HELLO, so it has no negotiated
	// capabilities until it does. Clear any left from a previous connection so
	// the server does not act on a feature this client never advertised.
	fClientProtocolVersion = 0;
	fClientCapabilities = 0;

	// Deliberately do NOT flush fReceiveBuffer here. A departed client's partial
	// message is already dropped at disconnect by _ConnectionClosed(), which runs
	// before the next client is accepted, so the receive stream is clean by the
	// time we get here. Flushing again at this point would race the receiver
	// thread writing this new client's RP_INIT_CONNECTION into the ring: the
	// flush cancels the parked event-thread read (B_CANCELED) and empties the
	// buffer out from under that write, discarding the one message every client
	// sends to bring up its display -- a black screen for every connection.
	// Keep the receive-side flush confined to the disconnect path.

	BNetEndpoint *sendEndpoint = new(std::nothrow) BNetEndpoint(endpoint);
	if (sendEndpoint == NULL)
		return B_NO_MEMORY;

	// Disable Nagle on the connection. The protocol is a stream of small
	// commands, several of which (DrawString, StringWidth, ReadBitmap) block the
	// drawing thread waiting for a client reply. With Nagle enabled each small
	// write is held back to coalesce and then collides with the peer's delayed
	// ACK, adding tens to ~200 ms to every one of those round-trips and to the
	// flush of every bitmap. TCP_NODELAY is a per-socket option and this send
	// endpoint shares the accepted connection with the receiver, so setting it
	// here also covers the inbound input and result replies. Best effort: a
	// failure here only leaves the previous (laggier) behaviour.
	int noDelay = 1;
	sendEndpoint->SetOption(TCP_NODELAY, IPPROTO_TCP, &noDelay, sizeof(noDelay));

	fSender.SetTo(new(std::nothrow) NetSender(sendEndpoint, fSendBuffer.Get()));
	if (!fSender.IsSet()) {
		delete sendEndpoint;
		return B_NO_MEMORY;
	}

	// The drawing engines have persisted across the disconnect, but the new
	// client has none of the per-token drawing state they had already sent to
	// the previous one, and each engine still believes that state is current.
	// Re-establish it: with the new sender now draining the buffer (so these
	// messages reach the client rather than being discarded), tell every engine
	// to recreate its client-side state and forget its cached view of it, so
	// the repaint that follows the client's display-mode update re-sends the
	// full state instead of short-circuiting on stale comparisons -- which is
	// what left the reconnected screen black.
	BAutolock lock(fEngineListLocker);
	for (int32 i = 0; i < fDrawingEngines.CountItems(); i++)
		fDrawingEngines.ItemAt(i)->ConnectionReset();

	return B_OK;
}


void
RemoteHWInterface::_ConnectionClosedCallback(void *cookie)
{
	((RemoteHWInterface *)cookie)->_ConnectionClosed();
}


void
RemoteHWInterface::_ConnectionClosed()
{
	// Called from the receiver thread the moment a client's connection ends.
	// The receiver has already closed its accepted socket; drop the sender that
	// holds a dup of the same connection so the socket can leave CLOSE_WAIT
	// instead of lingering until the next client happens to connect. Give up
	// both ring buffers too: with no client there is nothing to send, and any
	// half-received inbound message must not carry into the next connection.
	fIsConnected = false;

	// The next client re-negotiates from scratch; do not carry a departed
	// client's capabilities into a connection that never sent an RP_HELLO.
	fClientProtocolVersion = 0;
	fClientCapabilities = 0;

	fSender.Unset();

	fWireWriter->Reset();
	fReceiveBuffer->MakeEmpty();
}


void
RemoteHWInterface::_Disconnect()
{
	if (fIsConnected) {
		RemoteMessage message(NULL, fWireWriter.Get());
		message.Start(RP_CLOSE_CONNECTION);
		message.Flush();
		fIsConnected = false;
	}

	if (fListenEndpoint.IsSet())
		fListenEndpoint->Close();
}


status_t
RemoteHWInterface::SetMode(const display_mode& mode)
{
	TRACE("set mode: %" B_PRIu16 " %" B_PRIu16 "\n", mode.virtual_width,
		mode.virtual_height);
	fCurrentMode = mode;
	return B_OK;
}


void
RemoteHWInterface::GetMode(display_mode* mode)
{
	if (mode == NULL || !ReadLock())
		return;

	*mode = fCurrentMode;
	ReadUnlock();

	TRACE("get mode: %" B_PRIu16 " %" B_PRIu16 "\n", mode->virtual_width,
		mode->virtual_height);
}


status_t
RemoteHWInterface::GetPreferredMode(display_mode* mode)
{
	*mode = fClientMode;
	return B_OK;
}


status_t
RemoteHWInterface::GetDeviceInfo(accelerant_device_info* info)
{
	if (!ReadLock())
		return B_ERROR;

	info->version = fProtocolVersion;
	info->dac_speed = fConnectionSpeed;
	info->memory = 33554432; // 32MB
	strlcpy(info->name, "Haiku, Inc. RemoteHWInterface", sizeof(info->name));
	strlcpy(info->chipset, "Haiku, Inc. Chipset", sizeof(info->chipset));
	strlcpy(info->serial_no, fTarget, sizeof(info->serial_no));

	ReadUnlock();
	return B_OK;
}


status_t
RemoteHWInterface::GetModeList(display_mode** _modes, uint32* _count)
{
	AutoReadLocker _(this);

	display_mode* modes = new(std::nothrow) display_mode[2];
	if (modes == NULL)
		return B_NO_MEMORY;

	modes[0] = fFallbackMode;
	modes[1] = fClientMode;
	*_modes = modes;
	*_count = 2;

	return B_OK;
}


status_t
RemoteHWInterface::GetPixelClockLimits(display_mode* mode, uint32* low,
	uint32* high)
{
	TRACE("get pixel clock limits unsupported\n");
	return B_UNSUPPORTED;
}


status_t
RemoteHWInterface::GetTimingConstraints(display_timing_constraints* constraints)
{
	TRACE("get timing constraints unsupported\n");
	return B_UNSUPPORTED;
}


status_t
RemoteHWInterface::ProposeMode(display_mode* candidate, const display_mode* low,
	const display_mode* high)
{
	TRACE("propose mode: %" B_PRIu16 " %" B_PRIu16 "\n",
		candidate->virtual_width, candidate->virtual_height);
	return B_OK;
}


status_t
RemoteHWInterface::SetDPMSMode(uint32 state)
{
	return B_UNSUPPORTED;
}


uint32
RemoteHWInterface::DPMSMode()
{
	return B_UNSUPPORTED;
}


uint32
RemoteHWInterface::DPMSCapabilities()
{
	return 0;
}


status_t
RemoteHWInterface::SetBrightness(float)
{
	return B_UNSUPPORTED;
}


status_t
RemoteHWInterface::GetBrightness(float*)
{
	return B_UNSUPPORTED;
}


sem_id
RemoteHWInterface::RetraceSemaphore()
{
	return -1;
}


status_t
RemoteHWInterface::WaitForRetrace(bigtime_t timeout)
{
	return B_UNSUPPORTED;
}


void
RemoteHWInterface::SetCursor(ServerCursor* cursor)
{
	HWInterface::SetCursor(cursor);
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_SET_CURSOR);
	message.AddCursor(CursorAndDragBitmap().Get());
}


void
RemoteHWInterface::SetCursorVisible(bool visible)
{
	HWInterface::SetCursorVisible(visible);
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_SET_CURSOR_VISIBLE);
	message.Add(visible);
}


void
RemoteHWInterface::MoveCursorTo(float x, float y)
{
	HWInterface::MoveCursorTo(x, y);
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_MOVE_CURSOR_TO);
	message.Add(x);
	message.Add(y);
}


void
RemoteHWInterface::SetDragBitmap(const ServerBitmap* bitmap,
	const BPoint& offsetFromCursor)
{
	HWInterface::SetDragBitmap(bitmap, offsetFromCursor);
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_SET_CURSOR);
	message.AddCursor(CursorAndDragBitmap().Get());
}


RenderingBuffer*
RemoteHWInterface::FrontBuffer() const
{
	return NULL;
}


RenderingBuffer*
RemoteHWInterface::BackBuffer() const
{
	return NULL;
}


bool
RemoteHWInterface::IsDoubleBuffered() const
{
	return false;
}


status_t
RemoteHWInterface::InvalidateRegion(const BRegion& region)
{
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_INVALIDATE_REGION);
	message.AddRegion(region);
	return B_OK;
}


status_t
RemoteHWInterface::Invalidate(const BRect& frame)
{
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_INVALIDATE_RECT);
	message.Add(frame);
	return B_OK;
}


status_t
RemoteHWInterface::CopyBackToFront(const BRect& frame)
{
	return B_OK;
}


void
RemoteHWInterface::_FillDisplayModeTiming(display_mode &mode)
{
	mode.timing.pixel_clock
		= (uint64_t)mode.virtual_width * mode.virtual_height * 60 / 1000;
	mode.timing.h_display = mode.timing.h_sync_start = mode.timing.h_sync_end
		= mode.timing.h_total = mode.virtual_width;
	mode.timing.v_display = mode.timing.v_sync_start = mode.timing.v_sync_end
		= mode.timing.v_total = mode.virtual_height;
}
