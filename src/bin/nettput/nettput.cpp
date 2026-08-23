/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * nettput -- measure bulk TCP throughput and the CPU cost of moving each byte.
 *
 * This exists because a stock Haiku image has no iperf, no netperf and no
 * compiler, so there is no way to answer the only question that matters after a
 * driver or stack change: did it actually get faster, and did it get cheaper?
 * Those are different questions. A larger MTU can leave the wire rate untouched
 * while cutting the per-byte cost in half, because the win is in interrupts,
 * descriptor turns and per-frame stack work rather than in bits per second --
 * and on a shared virtual NIC the wire rate is often set by the hypervisor, not
 * by us. Reporting only Mbit/s would hide exactly the effect we are looking for.
 *
 * So every run reports both: the observed rate, and CPU microseconds consumed
 * per mebibyte transferred, derived from cpu_info::active_time deltas summed
 * over all CPUs. The second number is the one to compare across MTUs.
 *
 *   nettput -c <host> [-p port] [-n bytes] [-b bufsize] [-w win] [-r] [-L label]
 *
 *     -r   reverse: this host receives instead of transmits
 *     -n   bytes to move, accepts K/M/G suffixes (default 512M)
 *     -b   read/write chunk size (default 64K)
 *     -w   SO_SNDBUF/SO_RCVBUF, set before connect
 *     -L   free-form label echoed into the output, e.g. the MTU under test
 *
 * -w exists because the first measurements taken with this tool were not
 * measuring the network at all. Transmit came out at 1600 Mbit/s, and
 * 65535 bytes / 0.326 ms of round trip is 1608 Mbit/s: the number was the
 * default socket buffer divided by the round-trip time, to three digits. Haiku
 * has no send-buffer autotuning, so a single stream cannot transmit faster than
 * one send buffer per round trip no matter what the driver or the wire can do.
 * Sweeping -w separates "the stack is capped" from "the driver is slow", which
 * otherwise look identical.
 *
 * Receiving is not symmetric, and -w is not a free knob there: TCP does grow its
 * own receive window towards the bandwidth-delay product, and a -w smaller than
 * the default replaces that growth with a fixed size, so it can measure slower
 * than passing no -w at all. See graviton/docs/tcp-rcvbuf-cliff.md.
 *
 * The peer is graviton/scripts/nettput-peer.py, which needs nothing but python3.
 * A byte count is negotiated up front rather than a duration, so both ends know
 * precisely when the transfer is over and neither has to trust a clock the other
 * cannot see.
 *
 * Transmit runs are timed until the peer acknowledges the last byte, not until
 * the last write returns. Without that the socket buffer alone would report an
 * impressive and entirely fictitious rate on short runs.
 *
 * -A shifts the data buffer off its natural alignment, which sounds like a knob
 * nobody needs and is in fact the only way to measure one specific thing from
 * userland. arm64 uses string/arch/generic/generic_memcpy.c, which copies one
 * byte at a time unless source and destination are misaligned by the same
 * amount; a received TCP payload starts 54 bytes into the frame, so the copy out
 * to this buffer is mismatched and takes the byte loop. Sweeping -A moves the
 * destination through all eight phases, so if the penalty is real one offset
 * must be measurably cheaper than the other seven.
 *
 * It only works at an MTU whose MSS is a multiple of 8. At MTU 9001 the MSS is
 * 8961, so the destination's phase advances by one byte per segment and drifts
 * through every value regardless of where the buffer starts -- which is why the
 * penalty cannot be dodged by aligning anything and has to be fixed in memcpy.
 * At MTU 9000 the MSS is 8960 and the phase is constant, so the sweep has
 * something to find. Running both is the experiment: a dip at 9000 next to a
 * flat line at 9001 is the alignment penalty and cannot be anything else.
 */


#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <OS.h>
#include <SupportDefs.h>


// Wire format: 16 bytes, so the peer can read it in one go without framing.
// "NTPT", mode byte, three pad bytes, then the length as big-endian uint64.
#define NETTPUT_MAGIC		"NTPT"
#define NETTPUT_HEADER_SIZE	16

#define MODE_TRANSMIT		'T'		// we send, peer receives
#define MODE_RECEIVE		'R'		// peer sends, we receive

#define DEFAULT_PORT		5301
#define DEFAULT_BYTES		(512 * 1024 * 1024LL)
#define DEFAULT_BUFFER		(64 * 1024)

#define MAX_CPUS			64


struct cpu_snapshot {
	uint32		count;
	bigtime_t	active[MAX_CPUS];
	bigtime_t	wall;
};


static void
take_cpu_snapshot(cpu_snapshot& snapshot)
{
	system_info systemInfo;
	cpu_info cpuInfo[MAX_CPUS];

	snapshot.count = 0;
	snapshot.wall = system_time();

	if (get_system_info(&systemInfo) != B_OK)
		return;

	uint32 count = systemInfo.cpu_count;
	if (count > MAX_CPUS)
		count = MAX_CPUS;

	if (get_cpu_info(0, count, cpuInfo) != B_OK)
		return;

	snapshot.count = count;
	for (uint32 i = 0; i < count; i++)
		snapshot.active[i] = cpuInfo[i].active_time;
}


// Busy time across all CPUs between two snapshots. Idle CPUs contribute nothing,
// so this is machine-wide cost and not merely our own thread's -- which is the
// point, since the driver and the network stack burn most of their time in
// interrupt and kernel threads that a per-thread measurement would miss.
static bigtime_t
cpu_busy_between(const cpu_snapshot& before, const cpu_snapshot& after)
{
	if (before.count == 0 || before.count != after.count)
		return -1;

	bigtime_t busy = 0;
	for (uint32 i = 0; i < before.count; i++) {
		bigtime_t delta = after.active[i] - before.active[i];
		if (delta > 0)
			busy += delta;
	}

	return busy;
}


static off_t
parse_size(const char* text)
{
	char* end = NULL;
	double value = strtod(text, &end);

	if (end == text || value <= 0)
		return -1;

	switch (*end) {
		case 'g': case 'G':
			value *= 1024.0 * 1024.0 * 1024.0;
			end++;
			break;
		case 'm': case 'M':
			value *= 1024.0 * 1024.0;
			end++;
			break;
		case 'k': case 'K':
			value *= 1024.0;
			end++;
			break;
		case '\0':
			break;
		default:
			return -1;
	}

	if (*end != '\0')
		return -1;

	return (off_t)value;
}


// send()/recv() are free to move less than asked on a stream socket. Every
// transfer below goes through these two so a short move is never mistaken for
// the end of the data.
static ssize_t
write_fully(int socket, const void* buffer, size_t size)
{
	const uint8* at = (const uint8*)buffer;
	size_t remaining = size;

	while (remaining > 0) {
		ssize_t written = send(socket, at, remaining, 0);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0)
			return -1;

		at += written;
		remaining -= written;
	}

	return (ssize_t)size;
}


static ssize_t
read_fully(int socket, void* buffer, size_t size)
{
	uint8* at = (uint8*)buffer;
	size_t remaining = size;

	while (remaining > 0) {
		ssize_t bytesRead = recv(socket, at, remaining, 0);
		if (bytesRead < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (bytesRead == 0)
			break;

		at += bytesRead;
		remaining -= bytesRead;
	}

	return (ssize_t)(size - remaining);
}


// Must happen before connect(): the window scale factor is negotiated in the
// SYN, so a buffer enlarged afterwards cannot raise the scale that was already
// agreed, and the socket stays capped at the size it had when it shook hands.
static void
set_socket_buffers(int socketFD, int wanted)
{
	if (wanted <= 0)
		return;

	if (setsockopt(socketFD, SOL_SOCKET, SO_SNDBUF, &wanted, sizeof(wanted)) != 0)
		fprintf(stderr, "nettput: SO_SNDBUF %d rejected: %s\n", wanted,
			strerror(errno));
	if (setsockopt(socketFD, SOL_SOCKET, SO_RCVBUF, &wanted, sizeof(wanted)) != 0)
		fprintf(stderr, "nettput: SO_RCVBUF %d rejected: %s\n", wanted,
			strerror(errno));
}


static int
connect_to_peer(const char* host, int port, int windowSize)
{
	char service[16];
	snprintf(service, sizeof(service), "%d", port);

	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* results = NULL;
	int status = getaddrinfo(host, service, &hints, &results);
	if (status != 0) {
		fprintf(stderr, "nettput: cannot resolve %s: %s\n", host,
			gai_strerror(status));
		return -1;
	}

	int socketFD = -1;
	for (addrinfo* info = results; info != NULL; info = info->ai_next) {
		socketFD = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
		if (socketFD < 0)
			continue;

		set_socket_buffers(socketFD, windowSize);

		if (connect(socketFD, info->ai_addr, info->ai_addrlen) == 0)
			break;

		close(socketFD);
		socketFD = -1;
	}

	freeaddrinfo(results);

	if (socketFD < 0) {
		fprintf(stderr, "nettput: cannot connect to %s:%d: %s\n", host, port,
			strerror(errno));
		return -1;
	}

	return socketFD;
}


// Printed alongside every result: a socket buffer smaller than the
// bandwidth-delay product caps throughput on its own, and would silently look
// like a driver that failed to get faster.
static void
report_socket_buffers(int socket)
{
	int sendSize = 0;
	int receiveSize = 0;
	socklen_t length = sizeof(sendSize);

	if (getsockopt(socket, SOL_SOCKET, SO_SNDBUF, &sendSize, &length) != 0)
		sendSize = -1;

	length = sizeof(receiveSize);
	if (getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receiveSize, &length) != 0)
		receiveSize = -1;

	printf("  socket buffers  : send %d, receive %d bytes\n", sendSize,
		receiveSize);
}


static void
report(const char* label, char mode, off_t bytes, bigtime_t elapsed,
	bigtime_t cpuBusy, uint32 cpuCount)
{
	if (elapsed <= 0) {
		fprintf(stderr, "nettput: implausible elapsed time %" B_PRIdBIGTIME
			" us\n", elapsed);
		return;
	}

	double seconds = (double)elapsed / 1000000.0;
	double mebibytes = (double)bytes / (1024.0 * 1024.0);
	double megabits = ((double)bytes * 8.0) / 1000000.0;

	printf("\n=== nettput result%s%s ===\n", label != NULL ? ": " : "",
		label != NULL ? label : "");
	printf("  direction       : %s\n",
		mode == MODE_TRANSMIT ? "transmit (this host -> peer)"
			: "receive (peer -> this host)");
	printf("  transferred     : %" B_PRIdOFF " bytes (%.1f MiB)\n", bytes,
		mebibytes);
	printf("  elapsed         : %.3f s\n", seconds);
	printf("  throughput      : %.1f Mbit/s (%.1f MiB/s)\n", megabits / seconds,
		mebibytes / seconds);

	if (cpuBusy < 0) {
		printf("  cpu cost        : unavailable (cpu_info unreadable)\n");
		return;
	}

	double cpuSeconds = (double)cpuBusy / 1000000.0;
	printf("  cpu busy        : %.3f s across %" B_PRIu32 " cpu(s)"
		" (%.1f%% of the machine)\n", cpuSeconds, cpuCount,
		100.0 * cpuSeconds / (seconds * (double)cpuCount));
	printf("  cost per MiB    : %.0f us of cpu\n",
		mebibytes > 0 ? (double)cpuBusy / mebibytes : 0.0);
	printf("  cost per GiB    : %.3f s of cpu\n",
		mebibytes > 0 ? (cpuSeconds * 1024.0) / mebibytes : 0.0);
}


static int
usage(int status)
{
	FILE* out = status == 0 ? stdout : stderr;
	fprintf(out,
		"Usage: nettput -c <host> [options]\n"
		"\n"
		"Measure bulk TCP throughput and CPU cost per byte against\n"
		"graviton/scripts/nettput-peer.py running on <host>.\n"
		"\n"
		"  -c <host>     peer to connect to (required)\n"
		"  -p <port>     peer port (default %d)\n"
		"  -n <bytes>    bytes to transfer, K/M/G suffixes ok (default 512M)\n"
		"  -b <bytes>    read/write chunk size (default 64K)\n"
		"  -A <offset>   shift the data buffer this many bytes past its\n"
		"                natural alignment (0..63, default 0). Only useful for\n"
		"                probing whether a copy in the kernel is paying an\n"
		"                alignment penalty -- see the note in the source.\n"
		"  -w <bytes>    SO_SNDBUF/SO_RCVBUF, set before connect (default: leave\n"
		"                the system default alone)\n"
		"  -r            receive instead of transmit\n"
		"  -L <label>    label echoed into the result, e.g. \"mtu 9001\"\n"
		"  -h            this help\n",
		DEFAULT_PORT);
	return status;
}


int
main(int argc, char** argv)
{
	const char* host = NULL;
	const char* label = NULL;
	int port = DEFAULT_PORT;
	off_t bytes = DEFAULT_BYTES;
	off_t bufferSize = DEFAULT_BUFFER;
	int windowSize = 0;
	int bufferAlignment = 0;
	char mode = MODE_TRANSMIT;

	int option;
	while ((option = getopt(argc, argv, "c:p:n:b:w:A:rL:h")) != -1) {
		switch (option) {
			case 'c':
				host = optarg;
				break;
			case 'p':
				port = atoi(optarg);
				if (port <= 0 || port > 65535) {
					fprintf(stderr, "nettput: bad port \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'A':
				bufferAlignment = atoi(optarg);
				if (bufferAlignment < 0 || bufferAlignment > 63) {
					fprintf(stderr, "nettput: buffer offset must be 0..63,"
						" not \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'n':
				bytes = parse_size(optarg);
				if (bytes < 0) {
					fprintf(stderr, "nettput: bad size \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'b':
				bufferSize = parse_size(optarg);
				if (bufferSize <= 0 || bufferSize > 16 * 1024 * 1024) {
					fprintf(stderr, "nettput: bad chunk size \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'w': {
				off_t wanted = parse_size(optarg);
				if (wanted <= 0 || wanted > 512 * 1024 * 1024) {
					fprintf(stderr, "nettput: bad window size \"%s\"\n", optarg);
					return 1;
				}
				windowSize = (int)wanted;
				break;
			}
			case 'r':
				mode = MODE_RECEIVE;
				break;
			case 'L':
				label = optarg;
				break;
			case 'h':
				return usage(0);
			default:
				return usage(1);
		}
	}

	if (host == NULL)
		return usage(1);

	// A peer that vanishes mid-transfer must surface as a failed write, not as
	// a signal that kills the run before it can report anything.
	signal(SIGPIPE, SIG_IGN);

	// The extra 64 bytes give -A somewhere to shift the buffer into without
	// running off the end of the allocation.
	uint8* allocation = (uint8*)malloc(bufferSize + 64);
	if (allocation == NULL) {
		fprintf(stderr, "nettput: cannot allocate a %" B_PRIdOFF " byte buffer\n",
			bufferSize);
		return 1;
	}
	uint8* buffer = allocation + bufferAlignment;

	// Non-constant payload, so nothing along the path can shortcut a run of
	// zeroes, and a mis-delivered buffer is at least in principle detectable.
	for (off_t i = 0; i < bufferSize; i++)
		buffer[i] = (uint8)(i & 0xff);

	int socketFD = connect_to_peer(host, port, windowSize);
	if (socketFD < 0) {
		free(allocation);
		return 1;
	}

	uint8 header[NETTPUT_HEADER_SIZE];
	memset(header, 0, sizeof(header));
	memcpy(header, NETTPUT_MAGIC, 4);
	header[4] = (uint8)mode;
	for (int i = 0; i < 8; i++)
		header[8 + i] = (uint8)((uint64)bytes >> (56 - 8 * i));

	if (write_fully(socketFD, header, sizeof(header)) < 0) {
		fprintf(stderr, "nettput: cannot send the request header: %s\n",
			strerror(errno));
		close(socketFD);
		free(allocation);
		return 1;
	}

	printf("nettput: %s %" B_PRIdOFF " bytes %s %s:%d in %" B_PRIdOFF
		" byte chunks\n", mode == MODE_TRANSMIT ? "sending" : "receiving",
		bytes, mode == MODE_TRANSMIT ? "to" : "from", host, port, bufferSize);
	report_socket_buffers(socketFD);
	fflush(stdout);

	cpu_snapshot before;
	cpu_snapshot after;
	off_t moved = 0;
	bool failed = false;

	take_cpu_snapshot(before);

	if (mode == MODE_TRANSMIT) {
		while (moved < bytes) {
			size_t chunk = (size_t)((bytes - moved) < bufferSize
				? (bytes - moved) : bufferSize);
			if (write_fully(socketFD, buffer, chunk) < 0) {
				fprintf(stderr, "nettput: send failed after %" B_PRIdOFF
					" bytes: %s\n", moved, strerror(errno));
				failed = true;
				break;
			}
			moved += chunk;
		}

		// Time the run until the peer confirms the last byte arrived. The
		// socket buffer would otherwise absorb the tail of the transfer and
		// flatter the result.
		if (!failed) {
			shutdown(socketFD, SHUT_WR);

			uint8 ack = 0;
			if (read_fully(socketFD, &ack, 1) != 1 || ack != 'A') {
				fprintf(stderr, "nettput: peer did not acknowledge the"
					" transfer\n");
				failed = true;
			}
		}
	} else {
		while (moved < bytes) {
			size_t chunk = (size_t)((bytes - moved) < bufferSize
				? (bytes - moved) : bufferSize);
			ssize_t bytesRead = read_fully(socketFD, buffer, chunk);
			if (bytesRead < 0) {
				fprintf(stderr, "nettput: receive failed after %" B_PRIdOFF
					" bytes: %s\n", moved, strerror(errno));
				failed = true;
				break;
			}
			if (bytesRead == 0) {
				fprintf(stderr, "nettput: peer closed after %" B_PRIdOFF
					" of %" B_PRIdOFF " bytes\n", moved, bytes);
				failed = true;
				break;
			}
			moved += bytesRead;
		}
	}

	take_cpu_snapshot(after);
	close(socketFD);
	free(allocation);

	if (failed)
		return 1;

	report(label, mode, moved, after.wall - before.wall,
		cpu_busy_between(before, after), before.count);

	return 0;
}
