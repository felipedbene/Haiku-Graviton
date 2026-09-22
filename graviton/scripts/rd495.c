/*
 * rd495.c -- issue #495 harness for URP/1 doc-M1 reconnect correctness on the
 * DeBeOS remote-display (app_server) protocol. Native Haiku arm64; runs on the
 * machine under test and connects over loopback, because the RP port is bound
 * to 127.0.0.1 only and because a shared SSH forward has produced false
 * readings before (see rd420 / #420).
 *
 * WHAT IT ASSERTS, AND WHY EACH ARM IS THE ONE THAT DISCRIMINATES
 *
 *   arm 1  midmsg -- a MID-MESSAGE disconnect, then reconnect. A clean
 *          disconnect can pass while this fails, which is exactly why it is
 *          the arm that matters. The truncated frame is an RP_UPDATE_DISPLAY_MODE
 *          declaring ten body bytes of which four are sent, so the server's
 *          event thread reads the header and one field and is left with SIX
 *          bytes owing -- the exact size of the next client's one and only
 *          RP_INIT_CONNECTION. It then asserts:
 *            (a) D3: the reconnecting client's RP_INIT_CONNECTION is
 *                acknowledged, i.e. it was not consumed as the tail of a
 *                message from a client that is gone;
 *            (b) D4: the full per-token drawing state is replayed
 *                unconditionally -- not just RP_CREATE_STATE, which is all the
 *                previous lazy-invalidation repair sent;
 *            (c) input is alive: a mouse move is answered by the server moving
 *                its cursor (RP_MOVE_CURSOR_TO), which is only possible if the
 *                event thread is still framing this connection correctly;
 *            (d) the screen repaints to a comparable size, not to a token few
 *                ops;
 *            (e) the replay costs no round trip -- nothing in it is a query the
 *                client has to answer.
 *
 *   arm 2  twice -- the same thing again on the SAME app_server. One-shot
 *          recovery has hidden second-attempt bugs here before: #420 was
 *          literally "session A serves only ONE client per app_server
 *          lifetime".
 *
 *   arm 3  oldclient -- the capability gate, tested rather than asserted.
 *          A client that advertises no RP_CAP_RESYNC must get an RP_HELLO_ACK
 *          of exactly the old shape (two fields, 14 bytes on the wire), and a
 *          client that sends no RP_HELLO at all must still be served. A client
 *          that does advertise it gets two more fields (session id, generation).
 *
 *   arm 4  resync -- RP_RESYNC, in both directions, including the gate: a
 *          client that did NOT negotiate RP_CAP_RESYNC and asks anyway must NOT
 *          be sent an opcode it has no handler for, while still getting the
 *          replay (which is all existing opcodes and can only help).
 *
 * THE MUTATION THAT MAKES ANY OF THIS MEAN ANYTHING
 *   Run this harness against the AMI's own app_server FIRST. Arms 1 and 2 must
 *   FAIL there -- (a) and (b) both -- and arm 4 must fail at the barrier. Then
 *   swap in the built binary, restart app_server, and run it again. A harness
 *   that passes before the change is measuring nothing, and this one prints
 *   enough per-check detail to tell "passed" from "never reproduced".
 *
 * THE COOKIE
 *   app_server requires its per-boot session cookie as a connection's first
 *   frame (#423/#482). Read from
 *   /boot/system/settings/remote_desktop/session_cookie.<port> by default --
 *   this harness runs on the machine under test -- or from RD_COOKIE_FILE /
 *   RD_COOKIE.
 *
 * ENV: RD_PORT (default 10900), RD_HOST (default 127.0.0.1),
 *      RD_COOKIE_FILE, RD_COOKIE.
 *
 * EXIT: 0 all arms passed, 3 one or more checks failed, 2 setup failure.
 *
 * BUILD (cross-compile with the DeBeOS arm64 tools):
 *   CROSS=<cross-tools>/bin/aarch64-unknown-haiku-gcc
 *   $CROSS -O1 -o rd495 rd495.c -lnetwork
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RP_INIT_CONNECTION				1
#define RP_UPDATE_DISPLAY_MODE			2
#define RP_CLOSE_CONNECTION				3
#define RP_HELLO						6
#define RP_HELLO_ACK					7
#define RP_RESYNC						8
#define RP_SESSION_COOKIE				12
#define RP_COOKIE_METHOD_PER_BOOT		1

#define RP_CREATE_STATE					20
#define RP_DELETE_STATE					21
#define RP_ENABLE_SYNC_DRAWING			22
#define RP_DISABLE_SYNC_DRAWING			23
#define RP_INVALIDATE_RECT				24
#define RP_INVALIDATE_REGION			25

#define RP_SET_OFFSETS					40
#define RP_SET_HIGH_COLOR				41
#define RP_SET_LOW_COLOR				42
#define RP_SET_PEN_SIZE					43
#define RP_SET_STROKE_MODE				44
#define RP_SET_BLENDING_MODE			45
#define RP_SET_PATTERN					46
#define RP_SET_DRAWING_MODE				47
#define RP_SET_FONT						48
#define RP_SET_TRANSFORM				49

#define RP_CONSTRAIN_CLIPPING_REGION	60
#define RP_COPY_RECT_NO_CLIPPING		61
#define RP_DRAW_BITMAP					63

#define RP_DRAW_STRING					180
#define RP_STRING_WIDTH					183
#define RP_READ_BITMAP					185

#define RP_SET_CURSOR					200
#define RP_SET_CURSOR_VISIBLE			201
#define RP_MOVE_CURSOR_TO				202

#define RP_MOUSE_MOVED					220

#define RP_PROTOCOL_VERSION				1
#define RP_CAP_STRING_WIDTH_REPLY		(1 << 0)
#define RP_CAP_COMPRESS_ZSTD			(1 << 1)
#define RP_CAP_RESYNC					(1 << 2)

#define MAX_BODY						(4 * 1024 * 1024)

static const char *sHost = "127.0.0.1";
static int sPort = 10900;

static char sCookie[257];
static size_t sCookieLength = 0;

static int sChecks = 0;
static int sFailures = 0;

static uint8_t sBody[MAX_BODY];


static void
check(const char *label, bool condition, const char *detail)
{
	sChecks++;
	if (condition)
		printf("  ok    %s\n", label);
	else {
		sFailures++;
		printf("  FAIL  %s%s%s\n", label, detail != NULL ? "  " : "",
			detail != NULL ? detail : "");
	}

	fflush(stdout);
}


static double
now()
{
	struct timeval time;
	gettimeofday(&time, NULL);
	return time.tv_sec + time.tv_usec / 1000000.0;
}


static size_t
frame(uint8_t *out, uint16_t code, const void *payload, size_t payloadSize)
{
	uint32_t length = 6 + payloadSize;
	out[0] = (uint8_t)(code & 0xff);
	out[1] = (uint8_t)(code >> 8);
	out[2] = (uint8_t)(length & 0xff);
	out[3] = (uint8_t)((length >> 8) & 0xff);
	out[4] = (uint8_t)((length >> 16) & 0xff);
	out[5] = (uint8_t)((length >> 24) & 0xff);
	if (payloadSize > 0)
		memcpy(out + 6, payload, payloadSize);
	return length;
}


static void
put32(uint8_t *out, uint32_t value)
{
	for (int i = 0; i < 4; i++)
		out[i] = (uint8_t)(value >> (8 * i));
}


static uint32_t
get32(const uint8_t *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
		| ((uint32_t)in[3] << 24);
}


static float
getFloat(const uint8_t *in)
{
	uint32_t bits = get32(in);
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}


static size_t
cookieFrame(uint8_t *out)
{
	uint8_t payload[8 + sizeof(sCookie)];
	put32(payload, RP_COOKIE_METHOD_PER_BOOT);
	put32(payload + 4, (uint32_t)sCookieLength);
	memcpy(payload + 8, sCookie, sCookieLength);
	return frame(out, RP_SESSION_COOKIE, payload, 8 + sCookieLength);
}


static bool
loadCookie()
{
	const char *direct = getenv("RD_COOKIE");
	if (direct != NULL && direct[0] != '\0') {
		size_t length = strlen(direct);
		if (length >= sizeof(sCookie)) {
			printf("RD_COOKIE is too long (%zu bytes)\n", length);
			return false;
		}

		memcpy(sCookie, direct, length);
		sCookieLength = length;
		return true;
	}

	char path[512];
	const char *fromEnvironment = getenv("RD_COOKIE_FILE");
	if (fromEnvironment != NULL && fromEnvironment[0] != '\0')
		snprintf(path, sizeof(path), "%s", fromEnvironment);
	else {
		snprintf(path, sizeof(path),
			"/boot/system/settings/remote_desktop/session_cookie.%d", sPort);
	}

	FILE *file = fopen(path, "r");
	if (file == NULL) {
		printf("cannot read the session cookie from %s: %s\n", path,
			strerror(errno));
		return false;
	}

	if (fgets(sCookie, sizeof(sCookie), file) == NULL) {
		printf("session cookie file %s is empty\n", path);
		fclose(file);
		return false;
	}

	fclose(file);

	sCookieLength = strlen(sCookie);
	while (sCookieLength > 0 && (sCookie[sCookieLength - 1] == '\n'
			|| sCookie[sCookieLength - 1] == '\r'
			|| sCookie[sCookieLength - 1] == ' ')) {
		sCookie[--sCookieLength] = '\0';
	}

	printf("session cookie: %zu characters\n", sCookieLength);
	return sCookieLength > 0;
}


static void
setReceiveTimeout(int handle, int milliseconds)
{
	struct timeval timeout;
	timeout.tv_sec = milliseconds / 1000;
	timeout.tv_usec = (milliseconds % 1000) * 1000;
	setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}


static int
openSocket()
{
	int handle = socket(AF_INET, SOCK_STREAM, 0);
	if (handle < 0)
		return -1;

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(sPort);
	address.sin_addr.s_addr = inet_addr(sHost);
	if (connect(handle, (struct sockaddr *)&address, sizeof(address)) < 0) {
		printf("connect failed: %s\n", strerror(errno));
		close(handle);
		return -1;
	}

	int one = 1;
	setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setReceiveTimeout(handle, 400);
	return handle;
}


static bool
sendAll(int handle, const uint8_t *data, size_t length)
{
	size_t sent = 0;
	while (sent < length) {
		ssize_t written = send(handle, data + sent, length - sent, 0);
		if (written <= 0)
			return false;

		sent += written;
	}

	return true;
}


/*!	Reads one whole message. Returns the opcode, or -1 on timeout / close, and
	fills \a _bodyLength with the payload size (the body itself lands in the
	shared sBody). \a deadline bounds the whole read, so a message that arrives
	in pieces cannot outlive the caller's budget. */
static int
readMessage(int handle, size_t *_bodyLength, double deadline)
{
	uint8_t header[6];
	size_t got = 0;
	while (got < sizeof(header)) {
		if (now() > deadline)
			return -1;

		ssize_t read = recv(handle, header + got, sizeof(header) - got, 0);
		if (read == 0)
			return -1;
		if (read < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}

		got += read;
	}

	uint32_t total = (uint32_t)header[2] | ((uint32_t)header[3] << 8)
		| ((uint32_t)header[4] << 16) | ((uint32_t)header[5] << 24);
	if (total < sizeof(header) || total - sizeof(header) > MAX_BODY) {
		printf("      (desynchronised: code %u declares %u bytes)\n",
			(unsigned)(header[0] | (header[1] << 8)), (unsigned)total);
		return -1;
	}

	size_t bodyLength = total - sizeof(header);
	got = 0;
	while (got < bodyLength) {
		if (now() > deadline)
			return -1;

		ssize_t read = recv(handle, sBody + got, bodyLength - got, 0);
		if (read == 0)
			return -1;
		if (read < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}

		got += read;
	}

	*_bodyLength = bodyLength;
	return (int)(header[0] | (header[1] << 8));
}


// What one drained stretch of the server -> client stream contained.
struct census {
	int			messages;
	long		bytes;
	int			drawingOps;
	long		drawingBytes;
	int			queries;			// messages that want a client reply
	int			initAcks;
	int			createStates;
	int			resyncs;
	uint32_t	resyncGeneration;
	int			cursorMoves;
	float		lastCursorX;
	float		lastCursorY;
	int			stateOps;			// the RP_SET_* replay block
	bool		sawHighColor;
	bool		sawLowColor;
	bool		sawPenSize;
	bool		sawStrokeMode;
	bool		sawBlendingMode;
	bool		sawPattern;
	bool		sawDrawingMode;
	bool		sawFont;
	bool		sawTransform;
	bool		sawClipping;
	bool		sawSyncDrawing;
	int			helloAckBodyLength;
	uint32_t	helloVersion;
	uint32_t	helloCapabilities;
	uint32_t	helloSessionId;
	uint32_t	helloGeneration;
	// Queries seen between the first RP_CREATE_STATE and the last state op --
	// the replay window. A replay that needs an answer is not a replay.
	int			queriesDuringReplay;
};


/*!	Server -> client messages that put ink on the screen. Deliberately starts
	above RP_CONSTRAIN_CLIPPING_REGION (60), which is state, not ink -- counting
	it would let a state replay masquerade as a repaint, which is the one thing
	the repaint comparison must not do. */
static bool
isDrawingOp(int code)
{
	if (code >= RP_COPY_RECT_NO_CLIPPING && code <= RP_DRAW_STRING + 1)
		return true;
	if (code >= 260 && code <= 268)
		return true;
	return false;
}


/*!	Whether \a code belongs to a state replay: the state token, the RP_SET_*
	block, the clipping region and the sync-drawing flag. Anything else ends the
	replay window. */
static bool
isReplayOp(int code)
{
	if (code == RP_CREATE_STATE)
		return true;
	if (code == RP_ENABLE_SYNC_DRAWING || code == RP_DISABLE_SYNC_DRAWING)
		return true;
	if (code >= RP_SET_OFFSETS && code <= RP_SET_TRANSFORM)
		return true;
	if (code == RP_CONSTRAIN_CLIPPING_REGION)
		return true;
	return false;
}


static bool
isQuery(int code)
{
	return code == RP_STRING_WIDTH || code == RP_READ_BITMAP
		|| code == RP_DRAW_STRING || code == RP_DRAW_STRING + 1;
}


/*!	Drains the stream until it has been quiet for \a idleMilliseconds, or until
	\a budgetSeconds have passed, accumulating into \a census. */
static void
drain(int handle, struct census *census, double budgetSeconds,
	int idleMilliseconds)
{
	double deadline = now() + budgetSeconds;
	double idle = idleMilliseconds / 1000.0;
	double lastMessage = now();
	bool inReplay = false;

	while (now() < deadline) {
		size_t bodyLength = 0;
		double messageDeadline = now() + idle;
		if (messageDeadline > deadline)
			messageDeadline = deadline;

		int code = readMessage(handle, &bodyLength, messageDeadline);
		if (code < 0) {
			if (now() - lastMessage >= idle)
				return;
			continue;
		}

		lastMessage = now();
		census->messages++;
		census->bytes += 6 + bodyLength;

		if (isDrawingOp(code)) {
			census->drawingOps++;
			census->drawingBytes += 6 + bodyLength;
		}

		if (isQuery(code)) {
			census->queries++;
			if (inReplay)
				census->queriesDuringReplay++;
		}

		switch (code) {
			case RP_INIT_CONNECTION:
				census->initAcks++;
				break;

			case RP_CREATE_STATE:
				census->createStates++;
				inReplay = true;
				break;

			case RP_RESYNC:
				census->resyncs++;
				if (bodyLength >= 4)
					census->resyncGeneration = get32(sBody);
				break;

			case RP_HELLO_ACK:
				census->helloAckBodyLength = (int)bodyLength;
				if (bodyLength >= 8) {
					census->helloVersion = get32(sBody);
					census->helloCapabilities = get32(sBody + 4);
				}
				if (bodyLength >= 16) {
					census->helloSessionId = get32(sBody + 8);
					census->helloGeneration = get32(sBody + 12);
				}
				break;

			case RP_MOVE_CURSOR_TO:
				census->cursorMoves++;
				if (bodyLength >= 8) {
					census->lastCursorX = getFloat(sBody);
					census->lastCursorY = getFloat(sBody + 4);
				}
				break;

			case RP_SET_HIGH_COLOR:
				census->stateOps++; census->sawHighColor = true; break;
			case RP_SET_LOW_COLOR:
				census->stateOps++; census->sawLowColor = true; break;
			case RP_SET_PEN_SIZE:
				census->stateOps++; census->sawPenSize = true; break;
			case RP_SET_STROKE_MODE:
				census->stateOps++; census->sawStrokeMode = true; break;
			case RP_SET_BLENDING_MODE:
				census->stateOps++; census->sawBlendingMode = true; break;
			case RP_SET_PATTERN:
				census->stateOps++; census->sawPattern = true; break;
			case RP_SET_DRAWING_MODE:
				census->stateOps++; census->sawDrawingMode = true; break;
			case RP_SET_FONT:
				census->stateOps++; census->sawFont = true; break;
			case RP_SET_TRANSFORM:
				census->stateOps++; census->sawTransform = true; break;
			case RP_CONSTRAIN_CLIPPING_REGION:
				census->sawClipping = true; break;
			case RP_ENABLE_SYNC_DRAWING:
			case RP_DISABLE_SYNC_DRAWING:
				census->sawSyncDrawing = true; break;

			default:
				break;
		}

		// The replay window closes at the first thing that is not part of it.
		if (inReplay && !isReplayOp(code))
			inReplay = false;
	}
}


static void
reportCensus(const char *label, const struct census *census)
{
	printf("      %s: %d messages / %ld bytes, %d drawing ops / %ld bytes, "
		"%d create-state, %d state ops, %d queries, %d cursor moves\n",
		label, census->messages, census->bytes, census->drawingOps,
		census->drawingBytes, census->createStates, census->stateOps,
		census->queries, census->cursorMoves);
	fflush(stdout);
}


/*!	Connects, presents the cookie, opens with RP_INIT_CONNECTION, optionally
	sends RP_HELLO advertising \a capabilities, and announces the display mode.
	Pass \a capabilities < 0 to send no RP_HELLO at all -- a pre-handshake
	legacy client. */
static int
openSession(int64_t capabilities)
{
	int handle = openSocket();
	if (handle < 0)
		return -1;

	uint8_t buffer[512];
	size_t length = cookieFrame(buffer);
	if (!sendAll(handle, buffer, length)) {
		close(handle);
		return -1;
	}

	length = frame(buffer, RP_INIT_CONNECTION, NULL, 0);
	if (!sendAll(handle, buffer, length)) {
		close(handle);
		return -1;
	}

	if (capabilities >= 0) {
		uint8_t hello[24];
		put32(hello, RP_PROTOCOL_VERSION);
		put32(hello + 4, (uint32_t)capabilities);
		put32(hello + 8, 4096);
		put32(hello + 12, 4096);
		put32(hello + 16, 1200);
		put32(hello + 20, 760);
		length = frame(buffer, RP_HELLO, hello, sizeof(hello));
		if (!sendAll(handle, buffer, length)) {
			close(handle);
			return -1;
		}
	}

	uint8_t mode[8];
	put32(mode, 1200);
	put32(mode + 4, 760);
	length = frame(buffer, RP_UPDATE_DISPLAY_MODE, mode, sizeof(mode));
	if (!sendAll(handle, buffer, length)) {
		close(handle);
		return -1;
	}

	return handle;
}


/*!	Leaves the server's event thread part-way through a message and then drops
	the connection hard.

	Ten declared body bytes, four delivered: the server reads the header and one
	int32 and is left with exactly six bytes owing -- the size of the next
	client's whole RP_INIT_CONNECTION. The pause is so the event thread has
	certainly consumed the header before the socket goes away; the linger-zero
	close makes it an RST rather than an orderly shutdown, which is what a real
	network drop looks like. */
static void
disconnectMidMessage(int handle)
{
	// A header declaring ten payload bytes, then only four of them: after the
	// server reads the width, six are still owing.
	uint8_t buffer[32];
	uint8_t payload[10];
	memset(payload, 0, sizeof(payload));
	put32(payload, 1200);
	frame(buffer, RP_UPDATE_DISPLAY_MODE, payload, sizeof(payload));
	sendAll(handle, buffer, 6 + 4);

	usleep(400000);

	struct linger linger;
	linger.l_onoff = 1;
	linger.l_linger = 0;
	setsockopt(handle, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
	close(handle);

	// Give the receiver thread time to notice and to get back to accept().
	usleep(600000);
}


/*!	The whole of arm 1 / arm 2 for one reconnect. \a label names the attempt so
	a second-attempt-only failure is visible as such. */
static void
checkReconnect(const char *label, const struct census *baseline)
{
	printf("  -- reconnect %s --\n", label);

	int handle = openSession(RP_CAP_RESYNC);
	if (handle < 0) {
		check("the reconnect is accepted", false, label);
		return;
	}

	struct census census;
	memset(&census, 0, sizeof(census));
	drain(handle, &census, 12.0, 900);
	reportCensus(label, &census);

	// (a) D3. The one message every client sends exactly once got through and
	// was answered -- it was not eaten as the tail of the dead connection's
	// half-sent message.
	char detail[160];
	snprintf(detail, sizeof(detail), "(init acks %d, messages %d)",
		census.initAcks, census.messages);
	check("D3: RP_INIT_CONNECTION is acknowledged after a mid-message "
		"disconnect", census.initAcks >= 1, detail);

	// (b) D4. Not just RP_CREATE_STATE -- the state itself. The previous repair
	// sent RP_CREATE_STATE and then relied on a repaint re-sending state that
	// the dedup guards would in fact skip.
	snprintf(detail, sizeof(detail),
		"(create-state %d, state ops %d, high %d low %d pen %d stroke %d "
		"blend %d pattern %d mode %d font %d transform %d clip %d sync %d)",
		census.createStates, census.stateOps, census.sawHighColor,
		census.sawLowColor, census.sawPenSize, census.sawStrokeMode,
		census.sawBlendingMode, census.sawPattern, census.sawDrawingMode,
		census.sawFont, census.sawTransform, census.sawClipping,
		census.sawSyncDrawing);
	check("D4: the whole shadow drawing state is replayed, not just the state "
		"token", census.createStates >= 1 && census.sawHighColor
			&& census.sawLowColor && census.sawPenSize && census.sawStrokeMode
			&& census.sawBlendingMode && census.sawPattern
			&& census.sawDrawingMode && census.sawFont && census.sawTransform
			&& census.sawClipping && census.sawSyncDrawing, detail);

	// (e) and no round trip inside it.
	snprintf(detail, sizeof(detail), "(queries during the replay %d)",
		census.queriesDuringReplay);
	check("the replay costs no round trip", census.queriesDuringReplay == 0,
		detail);

	// (d) The screen repainted to a comparable size, rather than to a token few
	// ops. Compared against the first session on this same desktop, so the
	// yardstick is what this desktop actually draws and not a guess.
	snprintf(detail, sizeof(detail),
		"(drawing ops %d vs baseline %d, bytes %ld vs %ld)",
		census.drawingOps, baseline->drawingOps, census.drawingBytes,
		baseline->drawingBytes);
	check("the screen repaints to a comparable size",
		baseline->drawingOps > 0
			&& census.drawingOps * 10 >= baseline->drawingOps * 8
			&& census.drawingBytes * 10 >= baseline->drawingBytes * 8, detail);

	// (c) Input. A mouse move the server acts on comes back as the server
	// moving its own cursor; a desynchronised event thread cannot produce that.
	uint8_t buffer[32];
	uint8_t point[8];
	float x = 137.0f, y = 211.0f;
	uint32_t bits;
	memcpy(&bits, &x, sizeof(bits));
	put32(point, bits);
	memcpy(&bits, &y, sizeof(bits));
	put32(point + 4, bits);
	size_t length = frame(buffer, RP_MOUSE_MOVED, point, sizeof(point));
	sendAll(handle, buffer, length);

	struct census afterInput;
	memset(&afterInput, 0, sizeof(afterInput));
	drain(handle, &afterInput, 5.0, 900);

	snprintf(detail, sizeof(detail), "(cursor moves %d, last at %.0f,%.0f)",
		afterInput.cursorMoves, afterInput.lastCursorX, afterInput.lastCursorY);
	check("D3: input still reaches the desktop after the reconnect",
		afterInput.cursorMoves >= 1 && afterInput.lastCursorX == x
			&& afterInput.lastCursorY == y, detail);

	// The session identity a resync-capable client is given.
	snprintf(detail, sizeof(detail),
		"(ack body %d bytes, session %#x, generation %u)",
		census.helloAckBodyLength, (unsigned)census.helloSessionId,
		(unsigned)census.helloGeneration);
	check("a resync-capable client is told the session and the generation",
		census.helloAckBodyLength == 16 && census.helloSessionId != 0
			&& census.helloGeneration > 0, detail);

	disconnectMidMessage(handle);
}


static void
armOldClient()
{
	printf("  -- the capability gate: an old client keeps working --\n");

	// A client that sends RP_HELLO but advertises nothing new must get an
	// acknowledgement of exactly the old shape. Two fields, 14 bytes framed --
	// the bytes it has always parsed, with nothing appended for it to trip on.
	int handle = openSession(0);
	if (handle < 0) {
		check("a capability-less client is served", false, NULL);
		return;
	}

	struct census plain;
	memset(&plain, 0, sizeof(plain));
	drain(handle, &plain, 12.0, 900);
	reportCensus("caps=0", &plain);

	char detail[160];
	snprintf(detail, sizeof(detail), "(ack body %d bytes, capabilities %#x)",
		plain.helloAckBodyLength, (unsigned)plain.helloCapabilities);
	check("a client advertising no RP_CAP_RESYNC gets the old two-field "
		"RP_HELLO_ACK", plain.helloAckBodyLength == 8
			&& (plain.helloCapabilities & RP_CAP_RESYNC) == 0, detail);
	check("and it is served: it is acknowledged and the screen paints",
		plain.initAcks >= 1 && plain.drawingOps > 0, NULL);
	close(handle);
	usleep(400000);

	// And a pre-handshake client: no RP_HELLO at all, which is the wire as it
	// was before any of this existed.
	handle = openSession(-1);
	if (handle < 0) {
		check("a pre-handshake client is served", false, NULL);
		return;
	}

	struct census legacy;
	memset(&legacy, 0, sizeof(legacy));
	drain(handle, &legacy, 12.0, 900);
	reportCensus("no hello", &legacy);

	check("a client that sends no RP_HELLO at all is still served",
		legacy.initAcks >= 1 && legacy.drawingOps > 0, NULL);
	check("and is sent no RP_HELLO_ACK and no RP_RESYNC",
		legacy.helloAckBodyLength == 0 && legacy.resyncs == 0, NULL);

	// It must also get the reconnect repair, which uses no capability: the
	// replay is RP_CREATE_STATE and ordinary state ops.
	snprintf(detail, sizeof(detail), "(create-state %d, state ops %d)",
		legacy.createStates, legacy.stateOps);
	check("and it still gets the state replay, which needs no capability",
		legacy.createStates >= 1 && legacy.stateOps >= 9, detail);

	close(handle);
	usleep(400000);
}


static void
armResync()
{
	printf("  -- RP_RESYNC --\n");

	int handle = openSession(RP_CAP_RESYNC);
	if (handle < 0) {
		check("a resync-capable client is served", false, NULL);
		return;
	}

	struct census opening;
	memset(&opening, 0, sizeof(opening));
	drain(handle, &opening, 12.0, 900);
	reportCensus("opening", &opening);

	uint32_t generation = opening.helloGeneration;

	// Ask. The client is the only one that can see some of the ways it loses
	// its place, so this is the recovery that does not wait to be noticed.
	uint8_t buffer[32];
	uint8_t payload[4];
	put32(payload, 0);
	size_t length = frame(buffer, RP_RESYNC, payload, sizeof(payload));
	sendAll(handle, buffer, length);

	struct census replay;
	memset(&replay, 0, sizeof(replay));
	drain(handle, &replay, 12.0, 900);
	reportCensus("after resync", &replay);

	char detail[192];
	snprintf(detail, sizeof(detail),
		"(resyncs %d, generation %u, hello said %u)", replay.resyncs,
		(unsigned)replay.resyncGeneration, (unsigned)generation);
	check("the server answers with the generation barrier",
		replay.resyncs == 1 && replay.resyncGeneration == generation, detail);

	snprintf(detail, sizeof(detail), "(create-state %d, state ops %d)",
		replay.createStates, replay.stateOps);
	check("and replays the whole state behind it",
		replay.createStates >= 1 && replay.sawHighColor && replay.sawLowColor
			&& replay.sawPenSize && replay.sawStrokeMode
			&& replay.sawBlendingMode && replay.sawPattern
			&& replay.sawDrawingMode && replay.sawFont && replay.sawTransform
			&& replay.sawClipping && replay.sawSyncDrawing, detail);

	snprintf(detail, sizeof(detail), "(drawing ops %d, cursor visible/set %d)",
		replay.drawingOps, replay.cursorMoves);
	check("and a repaint follows it, so the client has pixels and not just "
		"state", replay.drawingOps > 0, detail);

	check("the replay costs no round trip", replay.queriesDuringReplay == 0,
		NULL);

	close(handle);
	usleep(400000);

	// The gate, in the direction that matters: a client that did NOT negotiate
	// RP_CAP_RESYNC and asks anyway must not be sent an opcode it has no
	// handler for. The native in-tree client is exactly such a client, and an
	// unknown opcode there costs it a full timeout (D10).
	handle = openSession(0);
	if (handle < 0) {
		check("a capability-less client is served", false, NULL);
		return;
	}

	struct census ungated;
	memset(&ungated, 0, sizeof(ungated));
	drain(handle, &ungated, 12.0, 900);

	sendAll(handle, buffer, length);

	struct census ungatedReply;
	memset(&ungatedReply, 0, sizeof(ungatedReply));
	drain(handle, &ungatedReply, 8.0, 900);
	reportCensus("resync from caps=0", &ungatedReply);

	snprintf(detail, sizeof(detail), "(resyncs %d, create-state %d)",
		ungatedReply.resyncs, ungatedReply.createStates);
	check("a client that did not negotiate RP_CAP_RESYNC is sent no RP_RESYNC",
		ungatedReply.resyncs == 0, detail);
	check("but is still replayed, because the replay needs no capability",
		ungatedReply.createStates >= 1 && ungatedReply.stateOps >= 9, detail);

	close(handle);
	usleep(400000);
}


int
main(int argc, char *argv[])
{
	const char *port = getenv("RD_PORT");
	if (port != NULL && port[0] != '\0')
		sPort = atoi(port);

	const char *host = getenv("RD_HOST");
	if (host != NULL && host[0] != '\0')
		sHost = host;

	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("rd495: URP/1 reconnect correctness, %s:%d\n", sHost, sPort);

	if (!loadCookie()) {
		printf("RD495=SETUP_FAILURE\n");
		return 2;
	}

	// Session A, the baseline: what this desktop draws for a fresh client. Every
	// "did it repaint" comparison below is against this, measured here rather
	// than assumed.
	printf("  -- baseline session --\n");
	int handle = openSession(RP_CAP_RESYNC);
	if (handle < 0) {
		printf("RD495=SETUP_FAILURE  (no session)\n");
		return 2;
	}

	struct census baseline;
	memset(&baseline, 0, sizeof(baseline));
	drain(handle, &baseline, 20.0, 1200);
	reportCensus("baseline", &baseline);

	check("the baseline session paints something to compare against",
		baseline.drawingOps > 0 && baseline.initAcks >= 1, NULL);
	if (baseline.drawingOps == 0) {
		printf("RD495=SETUP_FAILURE  (the desktop drew nothing; there is "
			"nothing to compare a repaint against)\n");
		close(handle);
		return 2;
	}

	// Arm 1: the mid-message disconnect.
	disconnectMidMessage(handle);
	checkReconnect("attempt 1", &baseline);

	// Arm 2: and again, on the same app_server. checkReconnect() leaves the
	// connection dropped mid-message the same way, so this is a second
	// mid-message reconnect and not a clean one.
	checkReconnect("attempt 2", &baseline);

	// Arm 3 and 4.
	armOldClient();
	armResync();

	printf("\n");
	if (sFailures > 0) {
		printf("RD495=FAIL  CHECKS=%d FAILURES=%d\n", sChecks, sFailures);
		return 3;
	}

	printf("RD495=PASS  CHECKS=%d\n", sChecks);
	(void)argc;
	(void)argv;
	return 0;
}
