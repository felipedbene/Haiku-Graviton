/*
 * rpwedge.c -- issue #294/#298 A/B harness for the Haiku remote-display
 * (app_server) protocol. Native Haiku arm64; runs on the guest over loopback
 * because the RP port is bound to 127.0.0.1 only.
 *
 * RP framing (RemoteMessage.h): uint16 code, uint32 total_len (= 6 + payload),
 * then payload. RP_INIT_CONNECTION == 1 with no payload => 01 00 06 00 00 00.
 * The server answers a client RP_INIT_CONNECTION by sending RP_INIT_CONNECTION
 * (code 1) back, then RP_SET_CURSOR etc. Seeing code 1 come back == handshake ack.
 *
 * Procedure (single process so held sockets stay open across the measurement):
 *   Phase A  open N wedgers: connect, send RP_INIT, confirm ack, then hold idle
 *            forever (never close, never send more). A half-open/frozen peer the
 *            single-threaded accept loop blocks on -- the #294 mode-2 trigger.
 *   Phase B  M drop cycles: connect, send RP_INIT, brief pause, close() cleanly
 *            (TCP FIN, no RP_CLOSE_CONNECTION) -- the SSH-tunnel-drop case. On a
 *            wedged server these rot in CLOSE_WAIT unaccepted.
 *   Measure  run netstat, print :10900 lines, count CLOSE_WAIT.
 *   Phase C  fresh probe: connect, send RP_INIT, wait up to T seconds for the
 *            RP_INIT ack. Report ACK + latency, or STALL.
 *
 * WHAT IT CATCHES (why it lives in the tree)
 *   The garbage-frame tests missed the #298 regression because they never
 *   exercised a *clean* connect immediately followed by a single RP_INIT racing
 *   a connection-boundary buffer flush. Phase A/C here do exactly that, so a
 *   future change that eats the first RP_INIT (black screen) or re-wedges the
 *   accept loop (CLOSE_WAIT pile-up, PROBE_CONNECT_FAILED) fails loudly:
 *     - regression present -> PHASE_C: RESULT=STALL / PROBE_CONNECT_FAILED
 *     - healthy            -> PHASE_C: RESULT=ACK ... in ~0.00s, PHASE_B all acked
 *
 * USAGE (on the guest, over loopback):
 *   rpwedge [port=10900] [wedgers=1] [drops=12] [probe_timeout_s=8.0]
 *
 * BUILD (guests have no compiler; cross-compile with the DeBeOS arm64 tools and
 * push the binary). Against a configured generated.arm64 tree:
 *   CROSS=<cross-tools>/bin/aarch64-unknown-haiku-gcc
 *   GEN=<...>/generated.arm64/objects/haiku/arm64/release/system
 *   $CROSS -c -o rpwedge.o rpwedge.c -Iheaders -Iheaders/config \
 *       -Iheaders/posix -Iheaders/os -Iheaders/os/support
 *   $CROSS -nostdlib -o rpwedge \
 *       $GEN/glue/arch/arm64/crti.o <gcclib>/crtbegin.o \
 *       $GEN/glue/init_term_dyn.o $GEN/glue/start_dyn.o rpwedge.o \
 *       -L$GEN/libroot -L$GEN/libnetwork -lroot -lnetwork \
 *       <gcclib>/crtend.o $GEN/glue/arch/arm64/crtn.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const unsigned char RP_INIT_FRAME[6] = {0x01,0x00,0x06,0x00,0x00,0x00};

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* connect to 127.0.0.1:port with a timeout; returns fd or -1. */
static int connect_to(int port, double timeout_s) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
	if (r == 0) { fcntl(fd, F_SETFL, fl); return fd; }
	if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
		close(fd); return -1;
	}
	fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
	struct timeval tv;
	tv.tv_sec = (long)timeout_s;
	tv.tv_usec = (long)((timeout_s - tv.tv_sec) * 1e6);
	r = select(fd + 1, NULL, &w, NULL, &tv);
	if (r <= 0) { close(fd); return -1; }        /* timeout: backlog full / no accept */
	int err = 0; socklen_t el = sizeof(err);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
	if (err != 0) { close(fd); return -1; }
	fcntl(fd, F_SETFL, fl);
	return fd;
}

/* send RP_INIT and wait up to timeout_s for a frame with code == RP_INIT (1).
   Parses the framed reply stream (the server also sends ConnectionReset / cursor
   frames around the handshake) and scans it for the RP_INIT ack, tolerating
   whatever precedes it. Returns latency in seconds on ack, or -1.0 on
   timeout/EOF. */
static double init_and_wait_ack(int fd, double timeout_s) {
	double t0 = now_s();
	if (send(fd, RP_INIT_FRAME, sizeof(RP_INIT_FRAME), 0) != (ssize_t)sizeof(RP_INIT_FRAME))
		return -1.0;
	unsigned char buf[8192];
	size_t have = 0, off = 0;
	double deadline = t0 + timeout_s;
	while (now_s() < deadline) {
		/* try to parse whole frames already buffered */
		while (have - off >= 6) {
			unsigned code = buf[off] | (buf[off+1] << 8);
			unsigned len  = buf[off+2] | (buf[off+3] << 8)
			              | (buf[off+4] << 16) | ((unsigned)buf[off+5] << 24);
			if (code == 1) return now_s() - t0;   /* RP_INIT ack seen */
			if (len < 6) return -1.0;              /* framing desync */
			if (have - off < len) break;           /* need more bytes */
			off += len;
		}
		/* compact */
		if (off > 0) { memmove(buf, buf + off, have - off); have -= off; off = 0; }
		double remain = deadline - now_s();
		if (remain < 0) remain = 0;
		fd_set r; FD_ZERO(&r); FD_SET(fd, &r);
		struct timeval tv;
		tv.tv_sec = (long)remain;
		tv.tv_usec = (long)((remain - tv.tv_sec) * 1e6);
		int sr = select(fd + 1, &r, NULL, NULL, &tv);
		if (sr < 0) { if (errno == EINTR) continue; return -1.0; }
		if (sr == 0) break;                        /* timeout */
		if (have == sizeof(buf)) return -1.0;      /* full, no ack: give up */
		ssize_t n = recv(fd, buf + have, sizeof(buf) - have, 0);
		if (n <= 0) return -1.0;                   /* EOF/err before ack */
		have += (size_t)n;
	}
	return -1.0;
}

int main(int argc, char** argv) {
	int port    = argc > 1 ? atoi(argv[1]) : 10900;
	int wedgers = argc > 2 ? atoi(argv[2]) : 1;
	int drops   = argc > 3 ? atoi(argv[3]) : 12;
	double probeT = argc > 4 ? atof(argv[4]) : 8.0;

	printf("RPWEDGE port=%d wedgers=%d drops=%d probe_timeout=%.1fs\n",
		port, wedgers, drops, probeT);

	/* Phase A: wedgers held open for the whole run. */
	int held[64]; int nheld = 0; int wedge_acks = 0;
	for (int i = 0; i < wedgers && i < 64; i++) {
		int fd = connect_to(port, 3.0);
		if (fd < 0) { printf("PHASE_A wedger %d: connect failed\n", i); continue; }
		double lat = init_and_wait_ack(fd, 3.0);
		if (lat >= 0) { wedge_acks++;
			printf("PHASE_A wedger %d: connected, RP_INIT ack in %.3fs, HOLDING idle\n", i, lat);
		} else {
			printf("PHASE_A wedger %d: connected, NO ack (held anyway)\n", i);
		}
		held[nheld++] = fd;   /* never closed */
	}
	printf("PHASE_A: %d wedger(s) held, %d acked\n", nheld, wedge_acks);

	/* Let the accept loop settle on the held connection. */
	usleep(500 * 1000);

	/* Phase B: clean-close drop cycles (FIN, no RP_CLOSE_CONNECTION). */
	int drop_conn = 0, drop_ack = 0, drop_connfail = 0;
	for (int i = 0; i < drops; i++) {
		int fd = connect_to(port, 2.0);
		if (fd < 0) { drop_connfail++; continue; }
		drop_conn++;
		/* send RP_INIT; brief wait to see if serviced, then drop */
		double lat = init_and_wait_ack(fd, 0.4);
		if (lat >= 0) drop_ack++;
		usleep(50 * 1000);
		close(fd);            /* clean FIN, no protocol shutdown */
		usleep(50 * 1000);
	}
	printf("PHASE_B: attempted=%d connected=%d connect_timeout/fail=%d acked=%d\n",
		drops, drop_conn, drop_connfail, drop_ack);

	/* settle, then measure server-side socket states */
	usleep(500 * 1000);
	printf("---- netstat :%d ----\n", port);
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "netstat -n 2>/dev/null | grep %d", port);
	fflush(stdout);
	int sysrc = system(cmd);
	(void)sysrc;
	fflush(stdout);

	/* Phase C: fresh probe */
	printf("---- PHASE_C fresh probe ----\n");
	int pfd = connect_to(port, 4.0);
	if (pfd < 0) {
		printf("PHASE_C: RESULT=PROBE_CONNECT_FAILED (could not even reach TCP)\n");
	} else {
		double lat = init_and_wait_ack(pfd, probeT);
		if (lat >= 0)
			printf("PHASE_C: RESULT=ACK RP_INIT ack in %.3fs\n", lat);
		else
			printf("PHASE_C: RESULT=STALL no RP_INIT ack within %.1fs (black screen)\n", probeT);
		close(pfd);
	}
	printf("RPWEDGE_DONE\n");
	return 0;
}
