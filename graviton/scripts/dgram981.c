/*
 * dgram981 -- the socket calls OpenSSL's BIO_s_datagram makes when "local
 * address" support is on (test/bio_dgram_test.c iterations 3 and 4, issue 981),
 * one row per call, so the failing one can be read off directly.
 *
 * For each of AF_INET and AF_INET6 over the loopback:
 *   recv   sender plain sendmsg(), receiver has the destination-address option on
 *          (IP_RECVDSTADDR / IP_PKTINFO, IPV6_RECVPKTINFO) and recvmsg()s with
 *          a control buffer: which control message arrives?
 *   send   sender sendmsg()s with the source-address control message OpenSSL
 *          packs (IP_PKTINFO, else IP_SENDSRCADDR; IPV6_PKTINFO): accepted?
 *          delivered?
 *
 * Build: cc -o dgram981 dgram981.c   (add -lnetwork on Haiku)
 */

#define _GNU_SOURCE
	/* struct in6_pktinfo on glibc */
#define __APPLE_USE_RFC_3542

#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


static int sFailures = 0;


static void
row(const char* name, int ok, const char* detail)
{
	printf("%-44s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
	if (!ok)
		sFailures++;
}


static int
make_socket(int af, struct sockaddr_storage* bound, socklen_t* boundLength)
{
	int fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0)
		return -1;

	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;
	if (af == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		in6->sin6_addr = in6addr_loopback;
		length = sizeof(*in6);
	}
	if (bind(fd, (struct sockaddr*)&address, length) != 0) {
		close(fd);
		return -1;
	}

	*boundLength = sizeof(*bound);
	getsockname(fd, (struct sockaddr*)bound, boundLength);

	struct timeval timeout = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	return fd;
}


static int
enable_receive_option(int fd, int af, char* detail, size_t detailSize)
{
	int one = 1;
	int result;
	if (af == AF_INET) {
#if defined(IP_PKTINFO)
		result = setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
		snprintf(detail, detailSize, "IP_PKTINFO=%d", result);
#elif defined(IP_RECVDSTADDR)
		result = setsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof(one));
		snprintf(detail, detailSize, "IP_RECVDSTADDR=%d", result);
#else
		result = -1;
		snprintf(detail, detailSize, "no IPv4 option defined");
#endif
	} else {
		result = setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one,
			sizeof(one));
		snprintf(detail, detailSize, "IPV6_RECVPKTINFO=%d", result);
	}
	if (result != 0) {
		size_t used = strlen(detail);
		snprintf(detail + used, detailSize - used, " (%s)", strerror(errno));
	}
	return result;
}


/* Receives one datagram; describes its control messages into detail. */
static ssize_t
receive_one(int fd, char* data, size_t dataSize, char* detail,
	size_t detailSize)
{
	union {
		struct cmsghdr align;
		char buffer[64];
	} control;
	struct iovec vector = { data, dataSize };
	struct msghdr header;
	memset(&header, 0, sizeof(header));
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.buffer;
	header.msg_controllen = sizeof(control.buffer);

	ssize_t received = recvmsg(fd, &header, 0);
	if (received < 0) {
		snprintf(detail, detailSize, "recvmsg=-1 (%s)", strerror(errno));
		return received;
	}

	int used = snprintf(detail, detailSize, "recvmsg=%zd controllen=%u flags=0x%x",
		received, (unsigned)header.msg_controllen, header.msg_flags);
	struct cmsghdr* message;
	for (message = CMSG_FIRSTHDR(&header); message != NULL;
			message = CMSG_NXTHDR(&header, message)) {
		char address[INET6_ADDRSTRLEN] = "?";
		const char* name = "other";
		if (message->cmsg_level == IPPROTO_IP) {
#if defined(IP_PKTINFO)
			if (message->cmsg_type == IP_PKTINFO) {
				name = "IP_PKTINFO";
				inet_ntop(AF_INET,
					&((struct in_pktinfo*)CMSG_DATA(message))->ipi_addr,
					address, sizeof(address));
			}
#endif
#if defined(IP_RECVDSTADDR)
			if (message->cmsg_type == IP_RECVDSTADDR) {
				name = "IP_RECVDSTADDR";
				inet_ntop(AF_INET, CMSG_DATA(message), address,
					sizeof(address));
			}
#endif
		} else if (message->cmsg_level == IPPROTO_IPV6
			&& message->cmsg_type == IPV6_PKTINFO) {
			name = "IPV6_PKTINFO";
			inet_ntop(AF_INET6,
				&((struct in6_pktinfo*)CMSG_DATA(message))->ipi6_addr,
				address, sizeof(address));
		}
		used += snprintf(detail + used, detailSize - used, " [%s %d/%d %s]",
			name, message->cmsg_level, message->cmsg_type, address);
	}
	return received;
}


/* Sends "send" with the source-address control message OpenSSL packs. */
static ssize_t
send_with_source(int fd, int af, const struct sockaddr_storage* to,
	socklen_t toLength, const struct sockaddr_storage* from, char* detail,
	size_t detailSize)
{
	union {
		struct cmsghdr align;
		char buffer[64];
	} control;
	memset(&control, 0, sizeof(control));
	struct iovec vector = { (void*)"send", 4 };
	struct msghdr header;
	memset(&header, 0, sizeof(header));
	header.msg_name = (void*)to;
	header.msg_namelen = toLength;
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.buffer;

	struct cmsghdr* message = (struct cmsghdr*)control.buffer;
	const char* name;
	if (af == AF_INET) {
#if defined(IP_PKTINFO)
		struct in_pktinfo info;
		memset(&info, 0, sizeof(info));
		info.ipi_spec_dst = ((struct sockaddr_in*)from)->sin_addr;
		message->cmsg_len = CMSG_LEN(sizeof(info));
		message->cmsg_level = IPPROTO_IP;
		message->cmsg_type = IP_PKTINFO;
		memcpy(CMSG_DATA(message), &info, sizeof(info));
		header.msg_controllen = CMSG_SPACE(sizeof(info));
		name = "IP_PKTINFO";
#elif defined(IP_SENDSRCADDR)
		message->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
		message->cmsg_level = IPPROTO_IP;
		message->cmsg_type = IP_SENDSRCADDR;
		memcpy(CMSG_DATA(message), &((struct sockaddr_in*)from)->sin_addr,
			sizeof(struct in_addr));
		header.msg_controllen = CMSG_SPACE(sizeof(struct in_addr));
		name = "IP_SENDSRCADDR";
#else
		snprintf(detail, detailSize,
			"neither IP_PKTINFO nor IP_SENDSRCADDR is defined");
		errno = ENOPROTOOPT;
		return -1;
#endif
	} else {
		struct in6_pktinfo info;
		memset(&info, 0, sizeof(info));
		info.ipi6_addr = ((struct sockaddr_in6*)from)->sin6_addr;
		message->cmsg_len = CMSG_LEN(sizeof(info));
		message->cmsg_level = IPPROTO_IPV6;
		message->cmsg_type = IPV6_PKTINFO;
		memcpy(CMSG_DATA(message), &info, sizeof(info));
		header.msg_controllen = CMSG_SPACE(sizeof(info));
		name = "IPV6_PKTINFO";
	}

	ssize_t sent = sendmsg(fd, &header, 0);
	snprintf(detail, detailSize, "sendmsg(%s)=%zd%s%s", name, sent,
		sent < 0 ? " " : "", sent < 0 ? strerror(errno) : "");
	return sent;
}


static void
run(int af)
{
	const char* family = af == AF_INET ? "ipv4" : "ipv6";
	char name[64], detail[512], data[64];
	struct sockaddr_storage address1, address2;
	socklen_t length1, length2;

	int fd1 = make_socket(af, &address1, &length1);
	int fd2 = make_socket(af, &address2, &length2);
	if (fd1 < 0 || fd2 < 0) {
		snprintf(name, sizeof(name), "%s setup", family);
		row(name, 0, strerror(errno));
		return;
	}

	snprintf(name, sizeof(name), "%s enable receive option", family);
	row(name, enable_receive_option(fd2, af, detail, sizeof(detail)) == 0,
		detail);

	/* recv: plain send, receiver asks for the destination address */
	ssize_t sent = sendto(fd1, "recv", 4, 0, (struct sockaddr*)&address2,
		length2);
	ssize_t received = receive_one(fd2, data, sizeof(data), detail,
		sizeof(detail));
	snprintf(name, sizeof(name), "%s recv: destination address cmsg", family);
	row(name, sent == 4 && received == 4
		&& (strstr(detail, "PKTINFO ") != NULL
			|| strstr(detail, "RECVDSTADDR ") != NULL), detail);

	/* send: sender passes the source address as a control message */
	snprintf(name, sizeof(name), "%s send: source address cmsg", family);
	sent = send_with_source(fd1, af, &address2, length2, &address1, detail,
		sizeof(detail));
	row(name, sent == 4, detail);
	if (sent == 4) {
		received = receive_one(fd2, data, sizeof(data), detail,
			sizeof(detail));
		snprintf(name, sizeof(name), "%s send: delivered", family);
		row(name, received == 4 && memcmp(data, "send", 4) == 0, detail);
	}

	close(fd1);
	close(fd2);
}


int
main(void)
{
	run(AF_INET);
	run(AF_INET6);
	printf("%s: %d failure(s)\n", sFailures == 0 ? "PASS" : "FAIL", sFailures);
	return sFailures == 0 ? 0 : 1;
}
