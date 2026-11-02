/*
 * udp899.c -- issue 899: a UDP socket with a datagram queued is readable,
 * even when the datagram is empty, whether the poll() or select() was
 * entered before the datagram arrived or after it. recv() of an empty
 * datagram returns 0, and once the queue is drained the socket is not
 * readable any more. Stream sockets are the control: there a 0-byte read
 * means EOF, and an idle connection is not readable.
 *
 * Covered, for each address setup (IPv4, IPv6, an IPv4 sender to a
 * dual-stack :: socket, and an IPv6 sender to ::ffff:127.0.0.1), each
 * connection mode (both ends unconnected; both connected; one socket
 * connected to itself, as Node's dgram tests do) and each wait (poll() or
 * select(), entered before or after the datagram arrives):
 *   - the sequence 0, 3, 0 bytes, each sent, waited for and read in turn
 *   - a fresh poll() on the drained socket reports nothing
 * And once per address setup:
 *   - 3 and 0 bytes queued together, then 0 and 0: a fresh poll() is
 *     readable before each recv(), and not after the last one
 *   - an empty datagram sent with sendmsg() and msg_iovlen 0
 *   - SO_RCVLOWAT larger than the queued datagram does not hide it
 * Stream control: an idle TCP connection, one with 1 byte queued, and one
 * whose peer has shut down writing (readable, recv() 0).
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux,
 * which is the reference.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


static int sFailed = 0;
static int sPassed = 0;


static void
report(const char* what, int ok, const char* detail)
{
	printf("%-58s %s  %s\n", what, ok ? "PASS" : "FAIL", detail);
	if (ok)
		sPassed++;
	else
		sFailed++;
}


static socklen_t
make_address(int family, const char* text, int port,
	struct sockaddr_storage* address)
{
	memset(address, 0, sizeof(*address));
	if (family == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)address;
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		inet_pton(AF_INET, text, &in->sin_addr);
		return sizeof(*in);
	}

	struct sockaddr_in6* in6 = (struct sockaddr_in6*)address;
	in6->sin6_family = AF_INET6;
	in6->sin6_port = htons(port);
	inet_pton(AF_INET6, text, &in6->sin6_addr);
	return sizeof(*in6);
}


static int
port_of(const struct sockaddr_storage* address)
{
	if (address->ss_family == AF_INET)
		return ntohs(((const struct sockaddr_in*)address)->sin_port);
	return ntohs(((const struct sockaddr_in6*)address)->sin6_port);
}


static int
local_port(int fd)
{
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	if (getsockname(fd, (struct sockaddr*)&address, &length) != 0)
		return -1;
	return port_of(&address);
}


/*!	A UDP socket of \a family bound to \a bindText, port 0. For AF_INET6,
	\a v6only sets IPV6_V6ONLY (-1 leaves the default).
*/
static int
udp_socket(int family, const char* bindText, int v6only)
{
	int fd = socket(family, SOCK_DGRAM, 0);
	if (fd < 0) {
		printf("socket(%d): %s\n", family, strerror(errno));
		return -1;
	}
	if (family == AF_INET6 && v6only >= 0
		&& setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
			sizeof(v6only)) != 0) {
		printf("IPV6_V6ONLY=%d: %s\n", v6only, strerror(errno));
		close(fd);
		return -1;
	}

	struct sockaddr_storage address;
	socklen_t length = make_address(family, bindText, 0, &address);
	if (bind(fd, (struct sockaddr*)&address, length) != 0) {
		printf("bind(%s): %s\n", bindText, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}


// #pragma mark - waiting


enum wait_mode {
	POLL_BEFORE,
	POLL_AFTER,
	SELECT_BEFORE,
	SELECT_AFTER
};

static const char* kWaitNames[] = {
	"poll-before", "poll-after", "select-before", "select-after"
};


/*!	Whether \a fd is readable, by poll() or select() per \a useSelect, with
	\a timeoutMs. Returns 1 readable, 0 not, -1 error; \a detail gets the raw
	result.
*/
static int
wait_readable(int fd, int useSelect, int timeoutMs, char* detail,
	size_t detailSize)
{
	if (!useSelect) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int r = poll(&pfd, 1, timeoutMs);
		if (r < 0) {
			snprintf(detail, detailSize, "poll: %s", strerror(errno));
			return -1;
		}
		snprintf(detail, detailSize, "poll=%d revents=0x%x", r,
			(unsigned)pfd.revents);
		return r == 1 && (pfd.revents & POLLIN) != 0;
	}

	fd_set set;
	FD_ZERO(&set);
	FD_SET(fd, &set);
	struct timeval timeout = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
	int r = select(fd + 1, &set, NULL, NULL, &timeout);
	if (r < 0) {
		snprintf(detail, detailSize, "select: %s", strerror(errno));
		return -1;
	}
	snprintf(detail, detailSize, "select=%d isset=%d", r,
		FD_ISSET(fd, &set) ? 1 : 0);
	return r == 1 && FD_ISSET(fd, &set);
}


struct waiter {
	int			fd;
	int			useSelect;
	int			result;
	char		detail[64];
};


static void*
waiter_thread(void* _waiter)
{
	struct waiter* waiter = (struct waiter*)_waiter;
	waiter->result = wait_readable(waiter->fd, waiter->useSelect, 3000,
		waiter->detail, sizeof(waiter->detail));
	return NULL;
}


// #pragma mark - UDP


struct setup {
	const char*	name;
	int			receiverFamily;
	const char*	receiverBind;
	int			receiverV6only;
	int			senderFamily;
	const char*	senderBind;
	int			senderV6only;
	const char*	destination;
		// receiver's address as the sender addresses it
	const char*	source;
		// sender's address as the receiver sees it
};

static const struct setup kSetups[] = {
	{ "ipv4", AF_INET, "127.0.0.1", -1, AF_INET, "127.0.0.1", -1,
		"127.0.0.1", "127.0.0.1" },
	{ "ipv6", AF_INET6, "::1", 1, AF_INET6, "::1", 1, "::1", "::1" },
	{ "v4-to-dual", AF_INET6, "::", 0, AF_INET, "127.0.0.1", -1,
		"127.0.0.1", "::ffff:127.0.0.1" },
	{ "mapped-to-dual", AF_INET6, "::", 0, AF_INET6, "::", 0,
		"::ffff:127.0.0.1", "::ffff:127.0.0.1" },
};


enum connect_mode {
	UNCONNECTED,
	CONNECTED,
	SELF_CONNECTED
};

static const char* kConnectNames[] = {
	"unconnected", "connected", "self-connected"
};


struct pair {
	int			sender;
	int			receiver;
	int			connected;
	struct sockaddr_storage destination;
	socklen_t	destinationLength;
};


static void
close_pair(struct pair* pair)
{
	if (pair->sender >= 0 && pair->sender != pair->receiver)
		close(pair->sender);
	if (pair->receiver >= 0)
		close(pair->receiver);
	pair->sender = pair->receiver = -1;
}


static int
open_pair(const struct setup* setup, int mode, struct pair* pair)
{
	pair->sender = pair->receiver = -1;
	pair->connected = mode != UNCONNECTED;

	if (mode == SELF_CONNECTED) {
		// One socket sends to itself. For the dual-stack setups it is the
		// :: socket, reaching itself through ::ffff:127.0.0.1.
		pair->receiver = udp_socket(setup->receiverFamily,
			setup->receiverBind, setup->receiverV6only);
		if (pair->receiver < 0)
			return -1;
		pair->sender = pair->receiver;
		const char* self = setup->receiverFamily == AF_INET
			? "127.0.0.1" : setup->receiverV6only == 0
				? "::ffff:127.0.0.1" : "::1";
		pair->destinationLength = make_address(setup->receiverFamily, self,
			local_port(pair->receiver), &pair->destination);
		if (connect(pair->receiver, (struct sockaddr*)&pair->destination,
				pair->destinationLength) != 0) {
			printf("connect(self %s): %s\n", self, strerror(errno));
			close_pair(pair);
			return -1;
		}
		return 0;
	}

	pair->receiver = udp_socket(setup->receiverFamily, setup->receiverBind,
		setup->receiverV6only);
	pair->sender = udp_socket(setup->senderFamily, setup->senderBind,
		setup->senderV6only);
	if (pair->receiver < 0 || pair->sender < 0) {
		close_pair(pair);
		return -1;
	}

	pair->destinationLength = make_address(setup->senderFamily,
		setup->destination, local_port(pair->receiver), &pair->destination);
	if (mode == CONNECTED) {
		struct sockaddr_storage source;
		socklen_t sourceLength = make_address(setup->receiverFamily,
			setup->source, local_port(pair->sender), &source);
		if (connect(pair->receiver, (struct sockaddr*)&source, sourceLength)
				!= 0
			|| connect(pair->sender, (struct sockaddr*)&pair->destination,
				pair->destinationLength) != 0) {
			printf("connect(): %s\n", strerror(errno));
			close_pair(pair);
			return -1;
		}
	}
	return 0;
}


static ssize_t
send_datagram(const struct pair* pair, size_t size)
{
	static const char kPayload[16] = "0123456789abcde";
	if (pair->connected)
		return send(pair->sender, kPayload, size, 0);
	return sendto(pair->sender, kPayload, size, 0,
		(const struct sockaddr*)&pair->destination, pair->destinationLength);
}


static ssize_t
receive_datagram(int fd)
{
	char buffer[64];
	return recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
}


/*!	Sends a \a size byte datagram and checks the receiver is reported
	readable by the \a mode wait, then that recv() returns \a size.
*/
static int
check_one(const struct pair* pair, int mode, size_t size, char* detail,
	size_t detailSize)
{
	int useSelect = mode == SELECT_BEFORE || mode == SELECT_AFTER;
	int readable;
	char waitDetail[64];

	if (mode == POLL_BEFORE || mode == SELECT_BEFORE) {
		struct waiter waiter = { pair->receiver, useSelect, -1, "" };
		pthread_t thread;
		if (pthread_create(&thread, NULL, waiter_thread, &waiter) != 0) {
			snprintf(detail, detailSize, "pthread_create failed");
			return 0;
		}
		usleep(200000);
			// give the waiter time to block
		ssize_t sent = send_datagram(pair, size);
		pthread_join(thread, NULL);
		if (sent != (ssize_t)size) {
			snprintf(detail, detailSize, "send(%zu)=%zd %s", size, sent,
				strerror(errno));
			return 0;
		}
		readable = waiter.result;
		strcpy(waitDetail, waiter.detail);
	} else {
		ssize_t sent = send_datagram(pair, size);
		if (sent != (ssize_t)size) {
			snprintf(detail, detailSize, "send(%zu)=%zd %s", size, sent,
				strerror(errno));
			return 0;
		}
		usleep(100000);
			// let it arrive before the wait starts
		readable = wait_readable(pair->receiver, useSelect, 0, waitDetail,
			sizeof(waitDetail));
	}

	ssize_t received = receive_datagram(pair->receiver);
	int receiveErrno = errno;
	if (received < 0) {
		snprintf(detail, detailSize, "%s recv=%zd (%s)", waitDetail,
			received, strerror(receiveErrno));
	} else
		snprintf(detail, detailSize, "%s recv=%zd", waitDetail, received);
	return readable == 1 && received == (ssize_t)size;
}


static int
check_drained(int fd, char* detail, size_t detailSize)
{
	return wait_readable(fd, 0, 0, detail, detailSize) == 0;
}


static void
test_sequence(const struct setup* setup, int connectMode, int waitMode)
{
	static const size_t kSizes[] = { 0, 3, 0 };
	char what[96];
	char detail[160];
	struct pair pair;

	if (open_pair(setup, connectMode, &pair) != 0) {
		snprintf(what, sizeof(what), "%s %s %s: setup", setup->name,
			kConnectNames[connectMode], kWaitNames[waitMode]);
		report(what, 0, "could not open the sockets");
		return;
	}

	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		snprintf(what, sizeof(what), "%s %s %s: %zu bytes", setup->name,
			kConnectNames[connectMode], kWaitNames[waitMode], kSizes[i]);
		int ok = check_one(&pair, waitMode, kSizes[i], detail,
			sizeof(detail));
		report(what, ok, detail);
	}

	snprintf(what, sizeof(what), "%s %s %s: drained", setup->name,
		kConnectNames[connectMode], kWaitNames[waitMode]);
	report(what, check_drained(pair.receiver, detail, sizeof(detail)),
		detail);

	close_pair(&pair);
}


/*!	Sends all of \a sizes before looking, then checks each is reported by a
	fresh poll() and read in order, and that nothing is left after.
*/
static void
test_queued(const struct setup* setup, const size_t* sizes, int count)
{
	char what[96];
	char detail[160];
	char label[32] = "";
	struct pair pair;

	for (int i = 0; i < count; i++) {
		size_t length = strlen(label);
		snprintf(label + length, sizeof(label) - length, "%s%zu",
			i > 0 ? "+" : "", sizes[i]);
	}

	if (open_pair(setup, UNCONNECTED, &pair) != 0) {
		snprintf(what, sizeof(what), "%s queued %s: setup", setup->name,
			label);
		report(what, 0, "could not open the sockets");
		return;
	}

	for (int i = 0; i < count; i++) {
		ssize_t sent = send_datagram(&pair, sizes[i]);
		if (sent != (ssize_t)sizes[i]) {
			snprintf(what, sizeof(what), "%s queued %s: send %zu",
				setup->name, label, sizes[i]);
			snprintf(detail, sizeof(detail), "send=%zd %s", sent,
				strerror(errno));
			report(what, 0, detail);
			close_pair(&pair);
			return;
		}
	}
	usleep(100000);

	for (int i = 0; i < count; i++) {
		char waitDetail[64];
		int readable = wait_readable(pair.receiver, 0, 0, waitDetail,
			sizeof(waitDetail));
		ssize_t received = receive_datagram(pair.receiver);
		snprintf(what, sizeof(what), "%s queued %s: #%d (%zu bytes)",
			setup->name, label, i + 1, sizes[i]);
		snprintf(detail, sizeof(detail), "%s recv=%zd", waitDetail,
			received);
		report(what, readable == 1 && received == (ssize_t)sizes[i],
			detail);
	}

	snprintf(what, sizeof(what), "%s queued %s: drained", setup->name,
		label);
	report(what, check_drained(pair.receiver, detail, sizeof(detail)),
		detail);
	close_pair(&pair);
}


/*!	Node's connected send of an empty array: sendmsg() with no iovecs. */
static void
test_sendmsg_empty(const struct setup* setup)
{
	char what[96];
	char detail[160];
	char waitDetail[64];
	struct pair pair;

	snprintf(what, sizeof(what), "%s sendmsg(msg_iovlen=0) then poll",
		setup->name);
	if (open_pair(setup, CONNECTED, &pair) != 0) {
		report(what, 0, "could not open the sockets");
		return;
	}

	struct msghdr message;
	memset(&message, 0, sizeof(message));
	ssize_t sent = sendmsg(pair.sender, &message, 0);
	if (sent != 0) {
		snprintf(detail, sizeof(detail), "sendmsg=%zd %s", sent,
			strerror(errno));
		report(what, 0, detail);
		close_pair(&pair);
		return;
	}
	usleep(100000);
	int readable = wait_readable(pair.receiver, 0, 0, waitDetail,
		sizeof(waitDetail));
	ssize_t received = receive_datagram(pair.receiver);
	snprintf(detail, sizeof(detail), "sendmsg=0 %s recv=%zd", waitDetail,
		received);
	report(what, readable == 1 && received == 0, detail);
	close_pair(&pair);
}


/*!	SO_RCVLOWAT does not apply to datagrams: one smaller than it is still
	readable (Linux ignores the option for UDP poll()).
*/
static void
test_low_water_mark(const struct setup* setup)
{
	char what[96];
	char detail[160];
	char waitDetail[64];
	struct pair pair;

	snprintf(what, sizeof(what), "%s SO_RCVLOWAT=8, 3 bytes queued",
		setup->name);
	if (open_pair(setup, UNCONNECTED, &pair) != 0) {
		report(what, 0, "could not open the sockets");
		return;
	}

	int lowat = 8;
	if (setsockopt(pair.receiver, SOL_SOCKET, SO_RCVLOWAT, &lowat,
			sizeof(lowat)) != 0) {
		snprintf(detail, sizeof(detail), "SO_RCVLOWAT: %s", strerror(errno));
		report(what, 0, detail);
		close_pair(&pair);
		return;
	}

	ssize_t sent = send_datagram(&pair, 3);
	usleep(100000);
	int readable = wait_readable(pair.receiver, 0, 0, waitDetail,
		sizeof(waitDetail));
	ssize_t received = receive_datagram(pair.receiver);
	snprintf(detail, sizeof(detail), "send=%zd %s recv=%zd", sent,
		waitDetail, received);
	report(what, sent == 3 && readable == 1 && received == 3, detail);
	close_pair(&pair);
}


// #pragma mark - TCP control


static void
test_stream(void)
{
	char detail[160];
	char waitDetail[64];

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_storage address;
	socklen_t length = make_address(AF_INET, "127.0.0.1", 0, &address);
	if (listener < 0 || bind(listener, (struct sockaddr*)&address, length) != 0
		|| listen(listener, 1) != 0) {
		report("tcp: setup", 0, strerror(errno));
		if (listener >= 0)
			close(listener);
		return;
	}
	length = make_address(AF_INET, "127.0.0.1", local_port(listener),
		&address);
	int client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0
		|| connect(client, (struct sockaddr*)&address, length) != 0) {
		report("tcp: connect", 0, strerror(errno));
		if (client >= 0)
			close(client);
		close(listener);
		return;
	}
	int server = accept(listener, NULL, NULL);
	close(listener);
	if (server < 0) {
		report("tcp: accept", 0, strerror(errno));
		close(client);
		return;
	}

	int readable = wait_readable(server, 0, 0, waitDetail,
		sizeof(waitDetail));
	report("tcp: idle connection is not readable", readable == 0,
		waitDetail);

	send(client, "x", 1, 0);
	usleep(100000);
	readable = wait_readable(server, 0, 0, waitDetail, sizeof(waitDetail));
	ssize_t received = receive_datagram(server);
	snprintf(detail, sizeof(detail), "%s recv=%zd", waitDetail, received);
	report("tcp: 1 byte queued is readable", readable == 1 && received == 1,
		detail);

	readable = wait_readable(server, 0, 0, waitDetail, sizeof(waitDetail));
	report("tcp: drained connection is not readable", readable == 0,
		waitDetail);

	shutdown(client, SHUT_WR);
	usleep(100000);
	readable = wait_readable(server, 0, 0, waitDetail, sizeof(waitDetail));
	received = receive_datagram(server);
	snprintf(detail, sizeof(detail), "%s recv=%zd", waitDetail, received);
	report("tcp: peer shutdown(SHUT_WR) is readable, recv 0 (EOF)",
		readable == 1 && received == 0, detail);

	close(client);
	close(server);
}


int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	const int setupCount = sizeof(kSetups) / sizeof(kSetups[0]);
	for (int s = 0; s < setupCount; s++) {
		for (int c = UNCONNECTED; c <= SELF_CONNECTED; c++) {
			for (int w = POLL_BEFORE; w <= SELECT_AFTER; w++)
				test_sequence(&kSetups[s], c, w);
		}

		static const size_t kNonEmptyFirst[] = { 3, 0 };
		static const size_t kEmptyTwice[] = { 0, 0 };
		test_queued(&kSetups[s], kNonEmptyFirst, 2);
		test_queued(&kSetups[s], kEmptyTwice, 2);
		test_sendmsg_empty(&kSetups[s]);
		test_low_water_mark(&kSetups[s]);
	}

	test_stream();

	printf("udp899: %d passed, %d failed\n", sPassed, sFailed);
	return sFailed;
}
