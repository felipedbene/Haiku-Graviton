/*
 * iov886.c -- issue 886: sendmsg()/recvmsg() with a zero-length iovec whose
 * base is NULL, { NULL, 0 }, which is how libuv sends an empty datagram.
 * Linux and the BSDs accept it; the 0-byte datagram must arrive.
 *
 * Controls: sendto(fd, NULL, 0) (worked before), { &byte, 0 } (worked
 * before), and the safety checks that must keep failing with EFAULT:
 * { NULL, 4 } and { (void*)1, 4 } through sendmsg(), and { NULL, 4 } through
 * recvmsg().
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int sFailed = 0;


static void
report(const char* what, int ok, ssize_t r, int e, const char* extra)
{
	printf("%-50s %s  r=%zd", what, ok ? "PASS" : "FAIL", r);
	if (r < 0)
		printf(" errno=%d (%s)", e, strerror(e));
	printf("%s\n", extra);
	if (!ok)
		sFailed++;
}


static int
udp_socket(struct sockaddr_in* address)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(address, 0, sizeof(*address));
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(*address);
	if (bind(fd, (struct sockaddr*)address, sizeof(*address)) != 0
		|| getsockname(fd, (struct sockaddr*)address, &length) != 0) {
		perror("bind");
		return -1;
	}
	return fd;
}


/*! Waits up to 500 ms for one datagram; returns its size, or -1 if none. */
static ssize_t
receive_one(int fd)
{
	struct pollfd p = { fd, POLLIN, 0 };
	if (poll(&p, 1, 500) <= 0)
		return -1;
	char buffer[64];
	return recv(fd, buffer, sizeof(buffer), 0);
}


static void
send_case(const char* what, int connected, void* base, size_t length,
	int expectFault)
{
	struct sockaddr_in address;
	int fd = udp_socket(&address);
	if (connected)
		connect(fd, (struct sockaddr*)&address, sizeof(address));

	struct iovec vec = { base, length };
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	if (!connected) {
		message.msg_name = &address;
		message.msg_namelen = sizeof(address);
	}
	message.msg_iov = &vec;
	message.msg_iovlen = 1;

	errno = 0;
	ssize_t r = sendmsg(fd, &message, 0);
	int e = errno;
	ssize_t got = receive_one(fd);
	char extra[64];
	snprintf(extra, sizeof(extra), "  received=%zd", got);
	if (expectFault)
		report(what, r < 0 && e == EFAULT && got < 0, r, e, extra);
	else
		report(what, r == (ssize_t)length && got == (ssize_t)length, r, e,
			extra);
	close(fd);
}


int
main()
{
	static char byte;

	// control: the plain zero-length sendto() that already worked
	{
		struct sockaddr_in address;
		int fd = udp_socket(&address);
		errno = 0;
		ssize_t r = sendto(fd, NULL, 0, 0, (struct sockaddr*)&address,
			sizeof(address));
		int e = errno;
		ssize_t got = receive_one(fd);
		char extra[64];
		snprintf(extra, sizeof(extra), "  received=%zd", got);
		report("sendto(NULL, 0) (control)", r == 0 && got == 0, r, e, extra);
		close(fd);
	}

	send_case("sendmsg iov{NULL,0}", 0, NULL, 0, 0);
	send_case("connected sendmsg iov{NULL,0}", 1, NULL, 0, 0);
	send_case("sendmsg iov{&byte,0} (control)", 0, &byte, 0, 0);
	send_case("sendmsg iov{NULL,4} must fault", 0, NULL, 4, 1);
	send_case("sendmsg iov{(void*)1,4} must fault", 0, (void*)1, 4, 1);

	// recvmsg() of a 0-byte datagram into { NULL, 0 }
	{
		struct sockaddr_in address;
		int fd = udp_socket(&address);
		sendto(fd, NULL, 0, 0, (struct sockaddr*)&address, sizeof(address));
		struct pollfd p = { fd, POLLIN, 0 };
		poll(&p, 1, 500);
		struct iovec vec = { NULL, 0 };
		struct msghdr message;
		memset(&message, 0, sizeof(message));
		message.msg_iov = &vec;
		message.msg_iovlen = 1;
		errno = 0;
		ssize_t r = recvmsg(fd, &message, MSG_DONTWAIT);
		int e = errno;
		report("recvmsg iov{NULL,0} of a 0-byte datagram", r == 0, r, e, "");

		// a datagram with data, into { NULL, 4 }: must fault, not crash
		sendto(fd, "abcd", 4, 0, (struct sockaddr*)&address, sizeof(address));
		poll(&p, 1, 500);
		vec.iov_len = 4;
		errno = 0;
		r = recvmsg(fd, &message, MSG_DONTWAIT);
		e = errno;
		report("recvmsg iov{NULL,4} must fault", r < 0 && e == EFAULT, r, e,
			"");
		close(fd);
	}

	printf("iov886: %d failed\n", sFailed);
	return sFailed;
}
