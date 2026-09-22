/*
 * rd423.c -- issue #423 harness for the per-boot session cookie on the Haiku
 * remote-display (app_server) port. Native Haiku arm64; runs on the machine
 * under test and connects over loopback, because that is exactly the position
 * the threat model describes -- a local process reaching 127.0.0.1:10900 -- and
 * because the RP port is bound to loopback only.
 *
 * WHAT IT ASSERTS
 *   The candidate gate used to prove that a connection *speaks the protocol*
 *   (six constant bytes of RP_INIT_CONNECTION). It now requires proof that the
 *   connection is *authorized*: RP_SESSION_COOKIE carrying the secret
 *   app_server minted into an owner-only file before it began listening. So:
 *
 *     arm 1  a connection presenting the correct cookie is served;
 *     arm 2  connections presenting no cookie, a wrong cookie, the pre-#423
 *            client shape (a bare RP_INIT_CONNECTION) or a malformed cookie
 *            frame are refused, see not one byte of the session, and leave the
 *            LIVE session undisturbed -- which is the half that makes the fix
 *            safe rather than merely strict;
 *     arm 3  a legitimate reconnect still takes the session over afterwards,
 *            so the refusals did not cost the gate its normal behaviour.
 *
 * THE MUTATION THAT PROVES ARM 1 IS REAL
 *   Arm 1 passing only means something if it *can* fail. Corrupt the published
 *   cookie file (app_server holds the real one in memory, so the file no longer
 *   matches) and run again: arm 1 must fail. The two arms are also each other's
 *   mutation -- a fail-open gate fails arm 2, a broken cookie path fails arm 1.
 *
 *     cp /boot/system/settings/remote_desktop/session_cookie.10900 /tmp/c.good
 *     echo 0000000000000000000000000000000000000000000000000000000000000000 \
 *         > /boot/system/settings/remote_desktop/session_cookie.10900
 *     rd423                      # expect RD423=FAIL, arm 1 red
 *     cp /tmp/c.good /boot/system/settings/remote_desktop/session_cookie.10900
 *     rd423                      # expect RD423=PASS again
 *
 * ENV: RD_PORT (default 10900), RD_HOST (default 127.0.0.1),
 *      RD_COOKIE_FILE (default
 *      /boot/system/settings/remote_desktop/session_cookie.<port>), RD_COOKIE.
 *
 * EXIT: 0 all checks passed, 3 a check failed, 2 setup failure (no cookie, no
 * session port) -- which is deliberately NOT reported as a gate failure.
 *
 * BUILD (guests have no compiler; cross-compile with the DeBeOS arm64 tools):
 *   CROSS=<cross-tools>/bin/aarch64-unknown-haiku-gcc
 *   $CROSS -O1 -o rd423 rd423.c -lnetwork
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

#define RP_INIT_CONNECTION			1
#define RP_UPDATE_DISPLAY_MODE		2
#define RP_SESSION_COOKIE			12
#define RP_COOKIE_METHOD_PER_BOOT	1

static const char *sHost = "127.0.0.1";
static int sPort = 10900;

// Little-endian by specification -- written out byte by byte so the harness
// does not depend on the host's order.
static const uint8_t kInitFrame[6] = {0x01, 0x00, 0x06, 0x00, 0x00, 0x00};

static char sCookie[257];
static size_t sCookieLength = 0;

static int sChecks = 0;
static int sFailures = 0;


static void
check(const char *label, bool condition, const char *detail)
{
	sChecks++;
	if (condition) {
		printf("  ok    %s\n", label);
	} else {
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


/*! Builds an RP_SESSION_COOKIE frame carrying \a cookie of \a cookieLength.
	\a declaredLength is what the frame's length field will claim the cookie is;
	pass cookieLength for a well-formed frame, anything else to produce the
	malformed shape arm 2 tests. */
static size_t
cookieFrame(uint8_t *out, const char *cookie, size_t cookieLength,
	size_t declaredLength)
{
	uint8_t payload[8 + sizeof(sCookie)];
	uint32_t method = RP_COOKIE_METHOD_PER_BOOT;
	uint32_t declared = (uint32_t)declaredLength;
	for (int i = 0; i < 4; i++) {
		payload[i] = (uint8_t)(method >> (8 * i));
		payload[4 + i] = (uint8_t)(declared >> (8 * i));
	}
	memcpy(payload + 8, cookie, cookieLength);
	return frame(out, RP_SESSION_COOKIE, payload, 8 + cookieLength);
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
		printf("session cookie: %zu characters from RD_COOKIE\n",
			sCookieLength);
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

	printf("session cookie: %zu characters from %s\n", sCookieLength, path);
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
	setReceiveTimeout(handle, 300);
	return handle;
}


/*! Connects and presents \a cookie (of \a cookieLength, declaring
	\a declaredLength), then RP_INIT_CONNECTION -- the opening of a real client.
	Returns the socket or -1. */
static int
connectWithCookie(const char *cookie, size_t cookieLength,
	size_t declaredLength)
{
	int handle = openSocket();
	if (handle < 0)
		return -1;

	uint8_t opening[6 + 8 + sizeof(sCookie) + sizeof(kInitFrame)];
	size_t openingSize = cookieFrame(opening, cookie, cookieLength,
		declaredLength);
	memcpy(opening + openingSize, kInitFrame, sizeof(kInitFrame));
	openingSize += sizeof(kInitFrame);

	if (send(handle, opening, openingSize, 0) != (ssize_t)openingSize) {
		printf("sending the opening failed: %s\n", strerror(errno));
		close(handle);
		return -1;
	}

	return handle;
}


/*! Reads \a handle for \a budget seconds, counting bytes and complete protocol
	messages. With \a answerMode it asks for an alternating display mode each
	round, so the server has a reason to draw. Sets *closedOut when the peer
	went away. */
static int
drain(const char *label, int handle, double budget, int *bytesOut,
	bool *closedOut, bool answerMode)
{
	double start = now();
	uint8_t pending[65536];
	size_t pendingSize = 0;
	int messages = 0;
	int totalBytes = 0;
	int round = 0;
	bool closed = false;

	while (now() - start < budget) {
		if (answerMode) {
			uint8_t modeFrame[14];
			int32_t sizeA[2] = { 1152, 864 };
			int32_t sizeB[2] = { 1024, 768 };
			size_t modeSize = frame(modeFrame, RP_UPDATE_DISPLAY_MODE,
				(round++ % 2) == 0 ? sizeA : sizeB, sizeof(sizeA));
			send(handle, modeFrame, modeSize, 0);
			// Paced, or this loop asks for a new mode as fast as the server can
			// repaint the whole screen and the "is it still being served"
			// question is answered in tens of megabytes.
			usleep(50 * 1000);
		}

		uint8_t buffer[16384];
		ssize_t readSize = recv(handle, buffer, sizeof(buffer), 0);
		if (readSize < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ETIMEDOUT)
				continue;

			// A connection the gate drops still has the bytes it pipelined
			// behind its first frame sitting unread in the receive queue, and
			// closing a socket with unread data sends a RST rather than a FIN.
			// So a refused probe usually learns about it as ECONNRESET, not as
			// a clean EOF -- and both mean the same thing: the server dropped
			// us. Counting only the FIN would report a working gate as broken.
			if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)
				closed = true;
			else {
				printf("    %s: recv error %s\n", label, strerror(errno));
			}

			break;
		}

		if (readSize == 0) {
			closed = true;
			break;
		}

		totalBytes += readSize;

		if (pendingSize + readSize <= sizeof(pending)) {
			memcpy(pending + pendingSize, buffer, readSize);
			pendingSize += readSize;
		} else
			pendingSize = 0;

		size_t offset = 0;
		while (pendingSize - offset >= 6) {
			uint32_t length = (uint32_t)pending[offset + 2]
				| ((uint32_t)pending[offset + 3] << 8)
				| ((uint32_t)pending[offset + 4] << 16)
				| ((uint32_t)pending[offset + 5] << 24);
			if (length < 6 || length > sizeof(pending)) {
				printf("%s: DESYNC\n", label);
				pendingSize = 0;
				break;
			}
			if (pendingSize - offset < length)
				break;
			offset += length;
			messages++;
		}

		if (offset > 0 && offset <= pendingSize) {
			memmove(pending, pending + offset, pendingSize - offset);
			pendingSize -= offset;
		}
	}

	printf("    %s: %d messages, %d bytes, closed=%s\n", label, messages,
		totalBytes, closed ? "true" : "false");
	fflush(stdout);

	if (bytesOut != NULL)
		*bytesOut = totalBytes;
	if (closedOut != NULL)
		*closedOut = closed;
	return messages;
}


/*! Runs one arm-2 probe: an unauthorized connection that must be refused
	without the live session noticing. \a opening/\a openingSize is what the
	probe writes (nothing at all when openingSize is 0). \a live is the live
	session, which is checked afterwards. */
static void
probeMustBeRefused(const char *what, const uint8_t *opening,
	size_t openingSize, int live)
{
	printf("  -- %s --\n", what);

	char label[160];
	int probe = openSocket();
	snprintf(label, sizeof(label), "%s: connects", what);
	check(label, probe >= 0, NULL);
	if (probe < 0)
		return;

	if (openingSize > 0
		&& send(probe, opening, openingSize, 0) != (ssize_t)openingSize) {
		snprintf(label, sizeof(label), "%s: writes its opening", what);
		check(label, false, strerror(errno));
		close(probe);
		return;
	}

	// The refused connection must be dropped, and must never have been handed a
	// byte of the session. A silent probe has nothing to decide on, so it is
	// dropped on the gate's 10 s deadline rather than at once; the budget is
	// generous enough to cover that without making a served probe look refused.
	int probeBytes = 0;
	bool probeClosed = false;
	drain("probe", probe, openingSize == 0 ? 13.0 : 6.0, &probeBytes,
		&probeClosed, false);

	snprintf(label, sizeof(label), "%s: is refused, not served", what);
	check(label, probeBytes == 0, "the probe was sent session bytes");

	snprintf(label, sizeof(label), "%s: is dropped by the server", what);
	check(label, probeClosed,
		"the server neither served nor dropped it (FIN or RST)");

	close(probe);

	// The half that matters most: the live session is still there, still being
	// served. A gate that dropped the session along with the probe would be a
	// remote kill switch for anything that can open a TCP connection.
	int liveBytes = 0;
	bool liveClosed = false;
	int liveMessages = drain("live", live, 4.0, &liveBytes, &liveClosed, true);

	snprintf(label, sizeof(label), "%s: the live session survives", what);
	check(label, !liveClosed && liveBytes > 0 && liveMessages > 0,
		"the live session stopped being served");
}


int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *portText = getenv("RD_PORT");
	if (portText != NULL)
		sPort = atoi(portText);
	const char *hostText = getenv("RD_HOST");
	if (hostText != NULL)
		sHost = hostText;

	printf("rd423: session cookie gate on %s:%d\n", sHost, sPort);

	if (!loadCookie()) {
		printf("RD423=SETUP_FAILED (no session cookie to present)\n");
		return 2;
	}

	// ---- arm 1: the correct cookie is served -------------------------------
	printf("  -- arm 1: a connection with the correct cookie --\n");
	int live = connectWithCookie(sCookie, sCookieLength, sCookieLength);
	check("arm 1: the authorized connection is accepted", live >= 0, NULL);
	if (live < 0) {
		printf("RD423=FAIL  CHECKS=%d FAILURES=%d\n", sChecks, sFailures);
		return 3;
	}

	int liveBytes = 0;
	bool liveClosed = false;
	int liveMessages = drain("live", live, 6.0, &liveBytes, &liveClosed, true);
	check("arm 1: the authorized connection is served the session",
		liveBytes > 0 && liveMessages > 0 && !liveClosed,
		"nothing arrived -- is a Desktop using this port?");

	if (liveBytes == 0) {
		// Without a served session there is nothing for arm 2 to protect, and
		// "the live session survived" would be vacuously true. Stop instead of
		// reporting checks that cannot mean anything.
		printf("RD423=FAIL  CHECKS=%d FAILURES=%d (no live session; arm 2"
			" would be vacuous)\n", sChecks, sFailures);
		close(live);
		return 3;
	}

	// ---- arm 2: everything else is refused, session undisturbed ------------
	uint8_t opening[6 + 8 + sizeof(sCookie) + 64];

	// 2a. No cookie at all: a connection that opens and says nothing, which is
	//     what a port scan or a health probe looks like.
	probeMustBeRefused("arm 2a: no cookie at all", NULL, 0, live);

	// 2b. A wrong cookie of exactly the right length -- one byte flipped, so
	//     the only thing distinguishing it from the real one is the secret.
	char wrong[sizeof(sCookie)];
	memcpy(wrong, sCookie, sCookieLength);
	wrong[0] = sCookie[0] == 'a' ? 'b' : 'a';
	size_t wrongSize = cookieFrame(opening, wrong, sCookieLength,
		sCookieLength);
	memcpy(opening + wrongSize, kInitFrame, sizeof(kInitFrame));
	probeMustBeRefused("arm 2b: a wrong cookie", opening,
		wrongSize + sizeof(kInitFrame), live);

	// 2c. The pre-#423 client: a bare RP_INIT_CONNECTION, the six constant
	//     bytes that used to be all a takeover needed.
	probeMustBeRefused("arm 2c: the pre-cookie client shape (RP_INIT_CONNECTION"
		" first)", kInitFrame, sizeof(kInitFrame), live);

	// 2d. A cookie frame whose declared cookie length disagrees with the frame
	//     length: the shape that would tempt a decoder into reading past what
	//     arrived.
	size_t lyingSize = cookieFrame(opening, sCookie, sCookieLength,
		sCookieLength - 1);
	probeMustBeRefused("arm 2d: a malformed cookie frame", opening, lyingSize,
		live);

	// ---- arm 3: a legitimate reconnect still takes over --------------------
	printf("  -- arm 3: a legitimate reconnect after the refusals --\n");
	int fresh = connectWithCookie(sCookie, sCookieLength, sCookieLength);
	check("arm 3: the reconnect is accepted", fresh >= 0, NULL);
	if (fresh >= 0) {
		int freshBytes = 0;
		bool freshClosed = false;
		int freshMessages = drain("fresh", fresh, 8.0, &freshBytes,
			&freshClosed, true);
		check("arm 3: the reconnect is served the session",
			freshBytes > 0 && freshMessages > 0 && !freshClosed,
			"the gate stopped serving authorized clients");
		close(fresh);
	}

	close(live);

	printf("\n");
	// The check count is part of the result: a run that covered four checks
	// fewer than the last one is a failure this has to be able to report.
	if (sFailures > 0) {
		printf("RD423=FAIL  CHECKS=%d FAILURES=%d\n", sChecks, sFailures);
		return 3;
	}

	printf("RD423=PASS  CHECKS=%d\n", sChecks);
	return 0;
}
