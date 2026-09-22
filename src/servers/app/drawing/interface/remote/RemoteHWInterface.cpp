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
#include <FindDirectory.h>
#include <NetEndpoint.h>
#include <Path.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


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


// Defined with the other cursor handling, below; used by the event thread.
static bool send_cursor(RemoteMessage& message,
	const ServerCursorReference& cursor);


struct callback_info {
	uint32				token;
	RemoteHWInterface::CallbackFunction	callback;
	void*				cookie;
};


// Where the session cookie is published, shared by convention with
// remote_broker (which reads the same path for the port it proxies to) and with
// the test instruments, which are pointed at it explicitly. The name carries
// the port because a host can serve more than one remote Desktop -- a Desktop
// is keyed on (uid, TARGET_SCREEN) -- and each listener has its own secret; one
// shared file would have the second listener silently invalidate the first's.
static const char* kCookieDirectoryName = "remote_desktop";
static const char* kCookieFilePrefix = "session_cookie.";

// 32 bytes of randomness, published as 64 hex characters, mirroring the shape
// of the broker's token file.
static const size_t kCookieRandomBytes = 32;


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
	fSessionCookieLength(0),
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
	memset(fSessionCookie, 0, sizeof(fSessionCookie));

	// Identifies this listener to its clients for the life of the process. Not
	// a secret and not a cookie: it only has to change when the *session*
	// changes, so that a reconnecting client can tell a session that survived
	// from one that was replaced. Derived from the clock and the pid, and forced
	// nonzero so zero stays available on the wire as "unknown".
	fSessionId = (uint32)(system_time() ^ ((bigtime_t)getpid() << 20));
	if (fSessionId == 0)
		fSessionId = 1;

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

	// Mint the session cookie after Bind() and before the receiver, whose thread
	// is what calls listen(). Both halves of that are deliberate.
	//
	// Before listening, because that is the only arrangement with no window:
	// nothing can be accepted until listen(), so there is never a moment where
	// the port is reachable and the secret it requires does not exist. app_server
	// both creates the listener and checks the cookie, which is what makes
	// "before" a statement about two adjacent lines rather than about two
	// processes starting in the right order.
	//
	// After binding, because minting publishes the cookie under a name derived
	// from the port -- so a second interface that loses the race for that port
	// would otherwise overwrite, and on its way out delete, the cookie file of
	// the listener that holds it. Binding first means we only ever write the
	// file for a port we own.
	//
	// And it fails closed: a cookie that cannot be minted or published leaves
	// this interface uninitialized, so ScreenManager discards it and the port is
	// never opened. Listening while unable to require anything is the
	// local-takeover gap this closes, restored silently.
	fInitStatus = _MintSessionCookie();
	if (fInitStatus != B_OK) {
		TRACE_ERROR("failed to mint the session cookie (%s); the remote "
			"session port will not be opened\n", strerror(fInitStatus));
		return;
	}

	fReceiver.SetTo(new(std::nothrow) NetReceiver(fListenEndpoint.Get(), fReceiveBuffer.Get(),
		_NewConnectionCallback, this, _ConnectionClosedCallback, NULL,
		fSessionCookie, fSessionCookieLength));
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

	// The cookie is only a secret while there is a listener to present it to.
	// Removing the file with the listener keeps a dead secret from lying around
	// suggesting otherwise; it is replaced anyway the next time a listener is
	// created, which is what makes it per-boot rather than persistent.
	_RemoveSessionCookie();
	memset(fSessionCookie, 0, sizeof(fSessionCookie));
	fSessionCookieLength = 0;
}


/*!	Generates this listener's session cookie and publishes it in an owner-only
	file for the broker (and the test instruments) to read.

	Called before the listening socket is created, so the cookie always exists
	before anything can connect. Returns an error -- which leaves the interface
	uninitialized, and therefore the port unopened -- when the cookie cannot be
	generated or published: on a host where the settings directory cannot be
	written, the remote desktop is unavailable rather than unauthenticated.
*/
status_t
RemoteHWInterface::_MintSessionCookie()
{
	BPath directory;
	status_t result = find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &directory);
	if (result != B_OK)
		return result;

	result = directory.Append(kCookieDirectoryName);
	if (result != B_OK)
		return result;

	if (mkdir(directory.Path(), 0755) != 0 && errno != EEXIST)
		return errno;

	char name[64];
	snprintf(name, sizeof(name), "%s%" B_PRIu16, kCookieFilePrefix,
		fListenPort);

	BPath path;
	result = path.SetTo(directory.Path(), name);
	if (result != B_OK)
		return result;

	// Randomness from the kernel, not from a seeded PRNG: this is a secret, and
	// app_server deliberately carries no crypto library of its own.
	uint8 random[kCookieRandomBytes];
	int randomFD = open("/dev/urandom", O_RDONLY);
	if (randomFD < 0)
		return errno;

	size_t randomRead = 0;
	while (randomRead < sizeof(random)) {
		ssize_t bytes = read(randomFD, random + randomRead,
			sizeof(random) - randomRead);
		if (bytes <= 0) {
			if (bytes < 0 && errno == EINTR)
				continue;
			close(randomFD);
			return bytes < 0 ? errno : B_IO_ERROR;
		}

		randomRead += bytes;
	}

	close(randomFD);

	// Hex, so the cookie survives every transport a human or a script might
	// carry it over (a settings file, an SSM command's output, a JSON field).
	for (size_t i = 0; i < sizeof(random); i++) {
		static const char kHex[] = "0123456789abcdef";
		fSessionCookie[2 * i] = kHex[random[i] >> 4];
		fSessionCookie[2 * i + 1] = kHex[random[i] & 0xf];
	}

	fSessionCookieLength = 2 * sizeof(random);
	memset(random, 0, sizeof(random));

	// Written to a temporary file and renamed into place, with the restrictive
	// mode applied at creation. Two reasons, both about a reader that is not
	// this process: a rename is atomic, so the broker either reads the whole
	// previous cookie or the whole new one and never a torn half; and an
	// O_EXCL create at 0600 neither follows a symlink somebody left in the
	// settings directory nor inherits a loose mode from a file that was
	// already there (O_CREAT's mode is ignored when the file exists).
	BPath temporaryPath;
	char temporaryName[64];
	snprintf(temporaryName, sizeof(temporaryName), "%s%" B_PRIu16 ".new",
		kCookieFilePrefix, fListenPort);
	result = temporaryPath.SetTo(directory.Path(), temporaryName);
	if (result != B_OK)
		return result;

	unlink(temporaryPath.Path());
	int fd = open(temporaryPath.Path(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return errno;

	char line[kMaxSessionCookieLength + 2];
	int lineLength = snprintf(line, sizeof(line), "%.*s\n",
		(int)fSessionCookieLength, fSessionCookie);
	bool written = lineLength > 0
		&& write(fd, line, lineLength) == (ssize_t)lineLength;
	memset(line, 0, sizeof(line));
	if (written)
		fsync(fd);
	close(fd);

	if (!written) {
		unlink(temporaryPath.Path());
		return B_IO_ERROR;
	}

	if (rename(temporaryPath.Path(), path.Path()) != 0) {
		result = errno;
		unlink(temporaryPath.Path());
		return result;
	}

	fSessionCookiePath = path;
	TRACE_ALWAYS("session cookie for port %" B_PRIu16 " published in %s\n",
		fListenPort, path.Path());
	return B_OK;
}


void
RemoteHWInterface::_RemoveSessionCookie()
{
	if (fSessionCookiePath.InitCheck() != B_OK)
		return;

	unlink(fSessionCookiePath.Path());
	fSessionCookiePath.Unset();
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
		// D3. If the connection changed under us, forget how far into a message
		// we had read. A body read that failed because the client went away left
		// fDataLeft pointing at bytes that will never arrive, and NextMessage()
		// opens by discarding that many -- which, with the ring already emptied
		// at the boundary, means discarding the *next* client's first bytes.
		// Ten of them is the whole of its one and only RP_INIT_CONNECTION.
		//
		// Checked here, at the top of the loop, because this is the one point
		// where no message is half-parsed by the code below: the generation may
		// change at any instant and a reset in the middle of reading a body
		// would lose fields the handler is about to use.
		message.ResetIfGenerationChanged(ConnectionGeneration());

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

				// Build the compressor before the acknowledgement claims it.
				// SupportedCapabilities() answers "was this compiled in", which
				// is not the same question as "can it be set up right now" --
				// the context and its output buffer are allocations and can
				// fail. Since the client switches its decoder on the strength of
				// this acknowledgement, a bit we cannot honour would desynchronise
				// the stream for the rest of the session with no way back. So
				// decide first and announce second, and drop the bit if the codec
				// did not come up: the session then runs uncompressed, which is
				// exactly what a client that never asked would have got.
				if ((fClientCapabilities & RP_CAP_COMPRESS_ZSTD) != 0
					&& !fWireWriter->PrepareCompression(
						RP_CAP_COMPRESS_ZSTD)) {
					TRACE_ERROR("cannot honour zstd compression, serving the "
						"session uncompressed\n");
					fClientCapabilities &= ~(uint32)RP_CAP_COMPRESS_ZSTD;
				}

				RemoteMessage reply(NULL, fWireWriter.Get());
				reply.Start(RP_HELLO_ACK);
				reply.Add(fClientProtocolVersion);
				reply.Add(fClientCapabilities);

				// If wire compression was negotiated it starts at the byte after
				// this acknowledgement, which is why the flush and the switch are
				// one operation: the acknowledgement must reach a client that is
				// still reading plain bytes, and a concurrent drawing op must not be
				// able to land between the two and be read as a segment header.
				// Only reachable if the connection was reset out from under us
				// between the prepare above and here, in which case the
				// acknowledgement was deliberately not written and the session
				// being torn down is the right outcome -- but it must not pass
				// unnoticed.
				// Session identity, appended only for a client that negotiated
				// RP_CAP_RESYNC. Gated rather than always appended because
				// "appending is harmless" is an assumption about every client
				// that exists, and the negotiated bit is a promise from the one
				// in front of us. A client that did not ask gets an
				// acknowledgement byte for byte identical to before.
				if ((fClientCapabilities & RP_CAP_RESYNC) != 0) {
					reply.Add(fSessionId);
					reply.Add(ConnectionGeneration());
				}

				result = reply.FlushAndEnableCompression(
					fClientCapabilities & RP_CAP_COMPRESS_ZSTD);
				if (result != B_OK) {
					TRACE_ERROR("failed to acknowledge hello: %s\n",
						strerror(result));
				}
				break;
			}

			case RP_RESYNC:
			{
				// The client says it cannot draw correctly and wants the server
				// to say everything again. The server cannot see every way a
				// client loses its place, so this is the recovery that does not
				// depend on the server noticing.
				uint32 clientGeneration = 0;
				if (message.Read(clientGeneration) != B_OK) {
					TRACE_ERROR("failed to read resync request\n");
					break;
				}

				if ((fClientCapabilities & RP_CAP_RESYNC) == 0) {
					// It cannot have negotiated the reply, so answering would
					// put an opcode on the wire it has no handler for. Replay
					// anyway -- that is all existing opcodes and can only help
					// -- but say nothing new.
					TRACE_ERROR("resync from a client that did not negotiate "
						"RP_CAP_RESYNC; replaying without the barrier\n");
					_ReplayState();
					break;
				}

				TRACE_ALWAYS("resync requested (client at generation %" B_PRIu32
					", server at %" B_PRIu32 ")\n", clientGeneration,
					ConnectionGeneration());

				// Barrier first, replay second. The barrier is what makes the
				// replay interpretable: it tells the client which generation
				// the bytes behind it belong to, so a client that has been
				// caching content across connections knows what to throw away.
				_SendResyncBarrier();
				_ReplayState();

				// And make a full repaint follow. Replaying state re-establishes
				// how to draw, not what was drawn; without this the client holds
				// a correct state and an empty screen until something happens to
				// invalidate it.
				RemoteMessage reply(NULL, fWireWriter.Get());
				if (send_cursor(reply, CursorAndDragBitmap()))
					reply.Flush();
				reply.Start(RP_SET_CURSOR_VISIBLE);
				reply.Add(fCursorVisible);
				reply.Flush();

				_NotifyScreenChanged();
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

	// A new connection is a new generation. Published before the sender exists,
	// so nothing can be written for this client until the event thread can
	// already see that its predecessor is gone.
	//
	// Deliberately do NOT flush fReceiveBuffer here. A departed client's partial
	// message is already dropped at disconnect by _ConnectionClosed(), which runs
	// before the next client is accepted, so the receive stream is clean by the
	// time we get here. Flushing again at this point would race the receiver
	// thread writing this new client's RP_INIT_CONNECTION into the ring: the
	// flush cancels the parked event-thread read (B_CANCELED) and empties the
	// buffer out from under that write, discarding the one message every client
	// sends to bring up its display -- a black screen for every connection.
	// Keep the receive-side flush confined to the disconnect path.
	//
	// What D3 actually needed was never the second flush: the bytes were already
	// gone, it was the event thread's *count* of the bytes still owing that
	// carried across. The generation bumped here is what retires that count --
	// see RemoteMessage::ResetIfGenerationChanged().
	atomic_add(&fConnectionGeneration, 1);

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
	// Re-establish it now that the new sender is draining the buffer, so these
	// messages reach the client rather than being discarded.
	//
	// Note this uses no capability and no new opcode: the replay is RP_CREATE_STATE
	// and the ordinary RP_SET_* ops, so the reconnect repair applies to every
	// client, including one that has never heard of URP/1.
	_ReplayState();

	return B_OK;
}


/*!	Re-states every live drawing engine's shadow drawing state to the client,
	unconditionally.

	D4, and the reason a symptom fix was not enough. Each setter on
	RemoteDrawingEngine compares against the shadow and returns early when they
	match, which is right within a connection and exactly wrong across one: the
	shadow records what the *previous* client was told, so after a reconnect the
	server is certain the client already has state that client has never seen.
	Nothing retries, because nothing believes anything is missing.

	The previous repair reset the shadow to the client's defaults and waited for
	a repaint to re-send it. That is lazy invalidation, and it is correct only if
	three things hold: a full repaint really does follow, the client's defaults
	really do match the ones assumed here, and every guarded field really is in
	the list of fields reset. Each is an assumption a future edit can break
	silently -- add a fourth guarded setter and forget the reset list and the
	black screen comes back, with nothing to fail until somebody reconnects.

	An eager, unconditional replay removes all three assumptions: after it, what
	the client has is what the server says it has, whether or not a repaint
	follows, whatever the client's idea of a default is, for exactly the fields
	the engine actually tracks.
*/
void
RemoteHWInterface::_ReplayState()
{
	BAutolock lock(fEngineListLocker);
	for (int32 i = 0; i < fDrawingEngines.CountItems(); i++)
		fDrawingEngines.ItemAt(i)->ReplayState();
}


/*!	Tells a resync-capable client which generation the bytes after this message
	belong to. A barrier, not a request: the replay follows it immediately.
*/
void
RemoteHWInterface::_SendResyncBarrier()
{
	RemoteMessage message(NULL, fWireWriter.Get());
	message.Start(RP_RESYNC);
	message.Add(ConnectionGeneration());
	message.Flush();
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

	// Retire this connection's generation before emptying the ring, so that any
	// reader woken by the cancel MakeEmpty() arms already sees the new value and
	// drops its half-read message instead of finishing it out of whatever
	// arrives next. Bumped at both ends of a connection on purpose: it is an
	// opaque monotonic counter, and being early is free while being late is the
	// bug.
	atomic_add(&fConnectionGeneration, 1);

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


// AddCursor() takes a `const ServerCursor&`, so passing the reference's raw
// pointer dereferences it. CursorAndDragBitmap() is nullable by design and in
// three ways: fCursorAndDragBitmap starts NULL, the accessor returns
// ServerCursorReference(NULL) when it cannot take fFloatingOverlaysLock, and
// _UpdateCursorAndDragBitmap() leaves it NULL if its allocation fails. The rest
// of HWInterface treats it as nullable -- _CursorFrame() guards it explicitly --
// so these two call sites were the ones that did not.
//
// There is nothing useful to send without a cursor, so say nothing: emitting a
// half-built RP_SET_CURSOR would leave the client decoding a cursor that is not
// there, which is worse than the state being momentarily unreported. The next
// real cursor change sends a complete message.
static bool
send_cursor(RemoteMessage& message, const ServerCursorReference& cursor)
{
	if (cursor.Get() == NULL)
		return false;

	message.Start(RP_SET_CURSOR);
	message.AddCursor(*cursor.Get());
	return true;
}


void
RemoteHWInterface::SetCursor(ServerCursor* cursor)
{
	HWInterface::SetCursor(cursor);
	RemoteMessage message(NULL, fWireWriter.Get());
	send_cursor(message, CursorAndDragBitmap());
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
	// Clearing a drag bitmap passes NULL here, which is the likeliest way to
	// reach the nullable accessor -- see send_cursor().
	send_cursor(message, CursorAndDragBitmap());
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
