/*
 * lo1099 -- issue 1099: a socket bound to a loopback address (127.0.0.1,
 * ::1) must not reach a destination off this host. RFC 1122 3.2.1.3 and
 * RFC 4291 2.5.3 forbid such a packet on the wire; Linux refuses the send
 * with EINVAL. A loopback source to the loopback, or to another address of
 * this host, is fine.
 *
 * One action per call, so a shell can bracket it with the interface's
 * transmit counter and a tcpdump on the peer:
 *
 *   lo1099 udp <bind address> <destination> <port> [count]
 *       count datagrams (default 5) from a socket bound to <bind address>:0;
 *       prints each sendto()'s result
 *   lo1099 self <bind address> <local destination>
 *       one datagram from <bind address>:0 to a receiver bound to
 *       <local destination>:0 on this host; prints whether it arrived, and
 *       from where
 *   lo1099 tcp <bind address> <destination> <port>
 *       connect() from <bind address>:0 with a 10 s deadline; prints its
 *       result and how long it took
 *
 * Build: cc -o lo1099 lo1099.c   (add -lnetwork on Haiku)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


static const char*
errno_name(int error)
{
	switch (error) {
		case 0: return "0";
		case EINVAL: return "EINVAL";
		case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
		case ENETUNREACH: return "ENETUNREACH";
		case EHOSTUNREACH: return "EHOSTUNREACH";
		case ETIMEDOUT: return "ETIMEDOUT";
		case ECONNREFUSED: return "ECONNREFUSED";
		case EINPROGRESS: return "EINPROGRESS";
		case EAGAIN: return "EAGAIN";
		default: return strerror(error);
	}
}


static socklen_t
make_address(const char* text, int port, struct sockaddr_storage* out)
{
	memset(out, 0, sizeof(*out));
	if (strchr(text, ':') == NULL) {
		struct sockaddr_in* in = (struct sockaddr_in*)out;
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		return inet_pton(AF_INET, text, &in->sin_addr) == 1 ? sizeof(*in) : 0;
	}
	struct sockaddr_in6* in6 = (struct sockaddr_in6*)out;
	in6->sin6_family = AF_INET6;
	in6->sin6_port = htons(port);
	return inet_pton(AF_INET6, text, &in6->sin6_addr) == 1 ? sizeof(*in6) : 0;
}


static void
address_text(const struct sockaddr_storage* address, char* text, size_t size)
{
	char host[INET6_ADDRSTRLEN] = "?";
	if (address->ss_family == AF_INET) {
		inet_ntop(AF_INET, &((const struct sockaddr_in*)address)->sin_addr,
			host, sizeof(host));
	} else if (address->ss_family == AF_INET6) {
		inet_ntop(AF_INET6, &((const struct sockaddr_in6*)address)->sin6_addr,
			host, sizeof(host));
	}
	snprintf(text, size, "%s", host);
}


/* A socket of type type bound to text:0, or -1 (and a message). */
static int
bound_socket(int type, const char* text, struct sockaddr_storage* bound)
{
	struct sockaddr_storage address;
	socklen_t length = make_address(text, 0, &address);
	if (length == 0) {
		printf("bad address %s\n", text);
		return -1;
	}
	int fd = socket(address.ss_family, type, 0);
	if (fd < 0 || bind(fd, (struct sockaddr*)&address, length) != 0) {
		printf("bind(%s)=-1 %s\n", text, errno_name(errno));
		return -1;
	}
	if (bound != NULL) {
		socklen_t boundLength = sizeof(*bound);
		getsockname(fd, (struct sockaddr*)bound, &boundLength);
	}
	return fd;
}


static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


static int
do_udp(const char* from, const char* to, int port, int count)
{
	int fd = bound_socket(SOCK_DGRAM, from, NULL);
	if (fd < 0)
		return 2;
	struct sockaddr_storage destination;
	socklen_t length = make_address(to, port, &destination);
	int failed = 0;
	for (int i = 0; i < count; i++) {
		errno = 0;
		ssize_t sent = sendto(fd, "lo1099", 6, 0,
			(struct sockaddr*)&destination, length);
		printf("udp %s -> %s.%d: sendto=%zd %s\n", from, to, port, sent,
			errno_name(sent < 0 ? errno : 0));
		if (sent < 0)
			failed++;
	}
	close(fd);
	return failed != 0;
}


static int
do_self(const char* from, const char* to)
{
	struct sockaddr_storage receiverAddress;
	int receiver = bound_socket(SOCK_DGRAM, to, &receiverAddress);
	int fd = bound_socket(SOCK_DGRAM, from, NULL);
	if (receiver < 0 || fd < 0)
		return 2;
	struct timeval timeout = { 2, 0 };
	setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	socklen_t length = receiverAddress.ss_family == AF_INET
		? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
	errno = 0;
	ssize_t sent = sendto(fd, "lo1099", 6, 0,
		(struct sockaddr*)&receiverAddress, length);
	int sendError = sent < 0 ? errno : 0;

	char data[16];
	struct sockaddr_storage source;
	socklen_t sourceLength = sizeof(source);
	ssize_t received = sent < 0 ? -1 : recvfrom(receiver, data, sizeof(data),
		0, (struct sockaddr*)&source, &sourceLength);
	char sourceText[INET6_ADDRSTRLEN] = "-";
	if (received >= 0)
		address_text(&source, sourceText, sizeof(sourceText));
	printf("self %s -> %s: sendto=%zd %s recv=%zd from %s\n", from, to, sent,
		errno_name(sendError), received, sourceText);
	close(fd);
	close(receiver);
	return received == 6 ? 0 : 1;
}


static int
do_tcp(const char* from, const char* to, int port)
{
	int fd = bound_socket(SOCK_STREAM, from, NULL);
	if (fd < 0)
		return 2;
	struct sockaddr_storage destination;
	socklen_t length = make_address(to, port, &destination);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	double start = now();
	int result = connect(fd, (struct sockaddr*)&destination, length);
	int error = result == 0 ? 0 : errno;
	if (result != 0 && error == EINPROGRESS) {
		struct pollfd entry = { fd, POLLOUT, 0 };
		int ready = poll(&entry, 1, 10000);
		if (ready == 0)
			error = ETIMEDOUT;
		else {
			socklen_t errorLength = sizeof(error);
			getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength);
		}
	}
	printf("tcp %s -> %s.%d: connect %s after %.2f s%s\n", from, to, port,
		errno_name(error), now() - start,
		error == ETIMEDOUT ? " (deadline)" : "");
	close(fd);
	return error != 0;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 5 && strcmp(argv[1], "udp") == 0)
		return do_udp(argv[2], argv[3], atoi(argv[4]),
			argc > 5 ? atoi(argv[5]) : 5);
	if (argc >= 4 && strcmp(argv[1], "self") == 0)
		return do_self(argv[2], argv[3]);
	if (argc >= 5 && strcmp(argv[1], "tcp") == 0)
		return do_tcp(argv[2], argv[3], atoi(argv[4]));

	fprintf(stderr, "usage: lo1099 udp|self|tcp ... (see the source)\n");
	return 2;
}
