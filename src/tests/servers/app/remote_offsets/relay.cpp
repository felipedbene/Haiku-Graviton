/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	A TCP relay that carries app_server's remote session port out of a guest.

	RemoteHWInterface binds 127.0.0.1 deliberately -- the remote protocol has no
	encryption and only the per-boot session cookie for authentication -- and
	that is the configuration the fleet runs, so the #544 reconnect proof wants
	the server left exactly as it is. QEMU's user-mode networking forwards a host
	port to the guest's *interface* address, never to the guest's loopback, so
	the two cannot meet without something inside the guest joining them. This is
	that something, and it is a test tool: it is staged into a throwaway image,
	it exists only on the slirp segment that image boots on, and app_server's
	cookie gate still stands behind it.

	The alternative was RemoteHWInterface's "unsafe-bind:<addr>:<port>" opt-in,
	which was rejected on purpose: it would make the proof run against a
	different bind than production, and it binds before DHCP has necessarily
	produced the address, which turns a missing lease into a crash-looping
	Desktop.

	Sequential by design -- one connection at a time, and a fresh upstream
	connection per accept. That is what the proof needs: dropping the client has
	to drop app_server's side too, so that the next accept is a genuine
	reconnection and not a resumed stream.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>


static const int kBufferSize = 65536;


static void
pump(int a, int b)
{
	char buffer[kBufferSize];

	while (true) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(a, &readSet);
		FD_SET(b, &readSet);

		int maximum = a > b ? a : b;
		if (select(maximum + 1, &readSet, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}

		for (int i = 0; i < 2; i++) {
			int from = i == 0 ? a : b;
			int to = i == 0 ? b : a;
			if (!FD_ISSET(from, &readSet))
				continue;

			ssize_t read = recv(from, buffer, sizeof(buffer), 0);
			if (read <= 0)
				return;

			ssize_t written = 0;
			while (written < read) {
				ssize_t sent = send(to, buffer + written, read - written, 0);
				if (sent <= 0)
					return;
				written += sent;
			}
		}
	}
}


int
main(int argc, char** argv)
{
	uint16_t listenPort = argc > 1 ? (uint16_t)atoi(argv[1]) : 10910;
	uint16_t targetPort = argc > 2 ? (uint16_t)atoi(argv[2]) : 10900;

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		fprintf(stderr, "relay: socket: %s\n", strerror(errno));
		return 1;
	}

	int one = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(listenPort);

	if (bind(listener, (struct sockaddr*)&address, sizeof(address)) < 0) {
		fprintf(stderr, "relay: bind %u: %s\n", listenPort, strerror(errno));
		return 1;
	}
	if (listen(listener, 1) < 0) {
		fprintf(stderr, "relay: listen: %s\n", strerror(errno));
		return 1;
	}

	printf("relay: %u -> 127.0.0.1:%u\n", listenPort, targetPort);
	fflush(stdout);

	while (true) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "relay: accept: %s\n", strerror(errno));
			return 1;
		}

		int upstream = socket(AF_INET, SOCK_STREAM, 0);
		if (upstream < 0) {
			close(client);
			continue;
		}

		struct sockaddr_in target;
		memset(&target, 0, sizeof(target));
		target.sin_family = AF_INET;
		target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		target.sin_port = htons(targetPort);

		if (connect(upstream, (struct sockaddr*)&target, sizeof(target)) < 0) {
			fprintf(stderr, "relay: connect 127.0.0.1:%u: %s\n", targetPort,
				strerror(errno));
			close(upstream);
			close(client);
			continue;
		}

		printf("relay: connected\n");
		fflush(stdout);

		pump(client, upstream);

		close(upstream);
		close(client);
		printf("relay: closed\n");
		fflush(stdout);
	}

	return 0;
}
