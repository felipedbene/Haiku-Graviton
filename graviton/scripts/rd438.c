/*
 * rd438.c -- issue #436 / PR #438 arm-2 harness for the Haiku remote-display
 * (app_server) protocol. Native Haiku arm64; runs on the machine under test and
 * connects over loopback, because the RP port is bound to 127.0.0.1 only and
 * because a shared SSH forward has produced false "the server sent nothing"
 * readings before (see rd420 / #420).
 *
 * WHAT THIS COVERS THAT rd420 DOES NOT
 *   rd420's `noise` mode parks a connection that sends *only* junk (16 bytes of
 *   0xab). That junk fails the #422 candidate gate, so the gate is what decides
 *   its fate. This harness drives the case the gate cannot refuse: a connection
 *   whose first six bytes are a perfectly valid RP_INIT_CONNECTION header
 *   followed IMMEDIATELY, IN THE SAME TCP SEGMENT, by junk. The gate validates
 *   the header and promotes the connection; the question is what the promotion
 *   hands to the protocol parser.
 *
 *     stock  _ReceiveCandidateData() reads up to 4096 bytes, so one Receive()
 *            takes header+junk, and promotion forwards ALL of it -- the parser
 *            sees code 0xABAB (43947) with a declared length of 0xABABABAB.
 *     fixed  the candidate buffer is exactly the 6 header bytes, so promotion
 *            forwards only the header; the rest of the stream is read after
 *            promotion, in order, by the normal session read path.
 *
 * SAME-SEGMENT GUARANTEE
 *   The junk connection sets TCP_NODELAY *before* it writes anything, writes
 *   header+junk with a SINGLE send(), and never writes again. Nagle therefore
 *   has nothing to coalesce with and nothing to hold back, and no earlier byte
 *   exists on that socket to split the write across segments. The server side
 *   corroborates it: the instrumented gate prints how many bytes one Receive()
 *   took off the socket.
 *
 * MODES
 *   sameseg [junkbytes]  live session, then a header+junk connection in one
 *                        segment, then a fresh client -- the wedge test.
 *   hdronly              control: the same connection with NO junk after the
 *                        header (a normal client's opening), so any difference
 *                        measured in `sameseg` is attributable to the junk.
 *
 * ENV: RD_PORT (default 10900), RD_HOST (default 127.0.0.1).
 *
 * EXIT: 0 pass (the server kept serving), 3 fail (wedged), 2 setup failure.
 *
 * BUILD (guests have no compiler; cross-compile with the DeBeOS arm64 tools):
 *   CROSS=<cross-tools>/bin/aarch64-unknown-haiku-gcc
 *   $CROSS -O1 -o rd438 rd438.c -lnetwork
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

#define RP_INIT_CONNECTION		1
#define RP_UPDATE_DISPLAY_MODE	2

static const char *sHost = "127.0.0.1";
static int sPort = 10900;

// A valid, complete RP_INIT_CONNECTION frame: code 1, total length 6, no body.
// Little-endian by specification -- written out byte by byte so the harness
// does not depend on the host's order.
static const uint8_t kInitFrame[6] = {0x01, 0x00, 0x06, 0x00, 0x00, 0x00};


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

	// Set before the first write, so the first write is also the first segment.
	int one = 1;
	setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return handle;
}


static int
localPort(int handle)
{
	struct sockaddr_in local;
	socklen_t localSize = sizeof(local);
	if (getsockname(handle, (struct sockaddr *)&local, &localSize) < 0)
		return -1;
	return ntohs(local.sin_port);
}


// A plain client: connect, send RP_INIT_CONNECTION, be served.
static int
connectClient()
{
	int handle = openSocket();
	if (handle < 0)
		return -1;

	if (send(handle, kInitFrame, sizeof(kInitFrame), 0)
			!= (ssize_t)sizeof(kInitFrame)) {
		printf("sending the handshake failed: %s\n", strerror(errno));
		close(handle);
		return -1;
	}

	return handle;
}


/*! Reads \a label's socket for \a budget seconds, counting bytes and complete
	protocol messages, and answering the mode query so the server has something
	to draw. Reports whether the peer closed. */
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
	double first = -1.0;
	bool closed = false;

	while (now() - start < budget) {
		if (answerMode) {
			// Alternate the requested mode so every round is a real change the
			// server must draw: asking twice for the mode already in effect
			// legitimately produces nothing on a static desktop.
			uint8_t modeFrame[14];
			int32_t sizeA[2] = { 1152, 864 };
			int32_t sizeB[2] = { 1024, 768 };
			size_t modeSize = frame(modeFrame, RP_UPDATE_DISPLAY_MODE,
				(round++ % 2) == 0 ? sizeA : sizeB, sizeof(sizeA));
			send(handle, modeFrame, modeSize, 0);
		}

		uint8_t buffer[16384];
		ssize_t readSize = recv(handle, buffer, sizeof(buffer), 0);
		if (readSize < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ETIMEDOUT)
				continue;
			printf("%s: recv error %s\n", label, strerror(errno));
			break;
		}

		if (readSize == 0) {
			closed = true;
			break;
		}

		if (first < 0.0)
			first = now() - start;
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

	printf("%s: %d messages, %d bytes, first byte %s%.2f s, open %.1f s,"
		" closed=%s\n", label, messages, totalBytes,
		first < 0.0 ? "never " : "", first < 0.0 ? 0.0 : first,
		now() - start, closed ? "true" : "false");
	fflush(stdout);

	if (bytesOut != NULL)
		*bytesOut = totalBytes;
	if (closedOut != NULL)
		*closedOut = closed;
	return messages;
}


int
main(int argc, char *argv[])
{
	const char *portText = getenv("RD_PORT");
	if (portText != NULL)
		sPort = atoi(portText);
	const char *hostText = getenv("RD_HOST");
	if (hostText != NULL)
		sHost = hostText;

	const char *mode = argc > 1 ? argv[1] : "sameseg";
	bool withJunk = strcmp(mode, "hdronly") != 0;
	size_t junkSize = 16;
	if (argc > 2)
		junkSize = (size_t)atoi(argv[2]);
	if (junkSize > 1024)
		junkSize = 1024;

	if (!withJunk)
		junkSize = 0;

	// 1. A live session, served, as in the #436 report.
	int live = connectClient();
	if (live < 0) {
		printf("RESULT: NO LIVE SESSION\n");
		return 2;
	}
	setReceiveTimeout(live, 300);
	int liveBytes = 0;
	printf("live client on local port %d\n", localPort(live));
	drain("live-before", live, 3.0, &liveBytes, NULL, false);
	if (liveBytes <= 0) {
		printf("RESULT: NO LIVE SESSION (0 bytes)\n");
		close(live);
		return 2;
	}

	// 2. The connection under test: a valid RP_INIT_CONNECTION header and
	//    junkSize junk bytes, in ONE send() on a TCP_NODELAY socket that has
	//    never been written to -- therefore one segment.
	int probe = openSocket();
	if (probe < 0) {
		printf("RESULT: PROBE CONNECT FAILED\n");
		close(live);
		return 2;
	}
	setReceiveTimeout(probe, 300);

	uint8_t opening[6 + 1024];
	memcpy(opening, kInitFrame, sizeof(kInitFrame));
	memset(opening + sizeof(kInitFrame), 0xab, junkSize);
	size_t openingSize = sizeof(kInitFrame) + junkSize;

	ssize_t written = send(probe, opening, openingSize, 0);
	printf("probe on local port %d: one send() of %zu bytes (6 byte valid"
		" RP_INIT_CONNECTION header + %zu bytes of 0xab) returned %zd\n",
		localPort(probe), openingSize, junkSize, written);
	if (written != (ssize_t)openingSize) {
		// A short write would mean the bytes did NOT go out together, which
		// invalidates the whole point of this arm.
		printf("RESULT: SHORT WRITE, the same-segment premise does not hold\n");
		close(probe);
		close(live);
		return 2;
	}
	fflush(stdout);

	// A quiet window with no further writes on this socket, so that whatever
	// the server's first read of this connection returns can contain nothing
	// but the bytes of that one send(). Without it the first mode query of the
	// measurement phase below lands in the same read and the server-side
	// "candidate read N bytes in one Receive()" evidence is about a larger N
	// than the send under test.
	sleep(2);
	printf("quiet window: 2 s with no further writes on the probe socket\n");
	fflush(stdout);

	// 3. What happens next. The probe sent a valid first frame, so it is
	//    *expected* to be promoted and to preempt the live session on both arms
	//    -- that is the documented reconnect-preempt behaviour and is NOT the
	//    signal here. The signal is whether the server survives the junk:
	//    whether the probe keeps being served, and whether a FRESH client can
	//    still be accepted and served afterwards.
	int probeBytes = 0;
	bool probeClosed = false;
	drain("probe-after", probe, 12.0, &probeBytes, &probeClosed, true);

	int liveAfter = 0;
	bool liveClosed = false;
	drain("live-after", live, 3.0, &liveAfter, &liveClosed, true);

	// 4. The decisive measurement: can the server still serve anyone at all?
	int fresh = connectClient();
	int freshBytes = 0;
	int freshMessages = 0;
	bool freshClosed = false;
	if (fresh < 0)
		printf("fresh client: CONNECT FAILED\n");
	else {
		setReceiveTimeout(fresh, 300);
		printf("fresh client on local port %d\n", localPort(fresh));
		freshMessages = drain("fresh", fresh, 12.0, &freshBytes, &freshClosed,
			true);
	}

	printf("SUMMARY: live-before=%d probe-after=%d live-after=%d fresh=%d"
		" fresh-messages=%d junk=%zu\n", liveBytes, probeBytes, liveAfter,
		freshBytes, freshMessages, junkSize);

	close(probe);
	close(live);
	if (fresh >= 0)
		close(fresh);

	// Two separate facts, both reported rather than collapsed into one verdict.
	// The promoted connection IS the session, so `probe-after` is how much the
	// session was served across the junk -- compare it against the `hdronly`
	// control run, where the same connection sends no junk at all. `fresh` is
	// whether the server can still serve anyone once that connection is gone.
	bool served = freshBytes > 0 && freshMessages > 0;
	printf("RESULT: session-served-across-the-junk=%d bytes,"
		" fresh-client=%s\n", probeBytes, served ? "served" : "NOTHING");
	return served ? 0 : 3;
}
