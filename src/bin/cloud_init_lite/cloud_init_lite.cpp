/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * cloud_init_lite -- the smallest useful EC2 instance-metadata client.
 *
 * On a Nitro instance there is no framebuffer and no serial getty, so the only
 * way in is SSH, and the only key the image knows about is whichever one was
 * baked in at build time. That makes the image personal to whoever built it:
 * anybody else has to rebuild to get in. This closes that gap by doing the two
 * things a full cloud-init would do that actually matter here -- install the
 * launch-time SSH key, and adopt the assigned hostname.
 *
 * Deliberately not cloud-init: no user-data script execution, no modules, no
 * YAML, no network configuration (DHCP already handled that). See the README in
 * the companion repository for what is and is not covered.
 *
 * Design constraints, all of which come from where this runs:
 *
 * - **IMDSv2 only.** IMDSv1's unauthenticated GET is disabled on many accounts
 *   by default and is being retired, so assuming it would produce an image that
 *   works for us and silently fails for others. Every request here carries a
 *   token obtained by PUT first.
 * - **Short timeouts, and silent when there is nothing there.** The same image
 *   must boot under QEMU, where 169.254.169.254 is simply not routed. A missing
 *   IMDS is the normal case off EC2, not an error worth printing.
 * - **Idempotent.** launch_daemon may run this again, and the merge must not
 *   append a duplicate key or corrupt authorized_keys.
 * - **Merge, never overwrite.** The baked-in key stays as a fallback, so an
 *   instance launched with no key pair (or with IMDS blocked) is still reachable
 *   by whoever built the image.
 */


#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


#define IMDS_ADDRESS		"169.254.169.254"
#define IMDS_PORT			80

/* Per-attempt budget. Generous enough for a loaded instance, small enough that
   the whole run is over quickly when there is no IMDS at all. */
#define IMDS_TIMEOUT_SECONDS	2

/* The interface gets its lease from DHCP after this job is started, so the
   first attempts are expected to fail. Roughly 20 seconds of patience, then
   give up for good -- this runs as its own launch_daemon job, so waiting here
   delays nothing else, least of all sshd. */
#define WAIT_ATTEMPTS		20

#define AUTHORIZED_KEYS		"/boot/home/config/settings/ssh/authorized_keys"

#define TOKEN_PATH			"/latest/api/token"
#define KEY_PATH			"/latest/meta-data/public-keys/0/openssh-key"
#define HOSTNAME_PATH		"/latest/meta-data/local-hostname"

#define RESPONSE_MAX		8192


/*!	Connects to the metadata service, with a bounded wait.

	A blocking connect() to an unrouted link-local address can sit there for a
	long time, which is exactly the "not on EC2" case, so the socket is put in
	non-blocking mode for the connect and switched back afterwards.
*/
static int
imds_connect()
{
	int socketFD = socket(AF_INET, SOCK_STREAM, 0);
	if (socketFD < 0)
		return -1;

	int flags = fcntl(socketFD, F_GETFL, 0);
	fcntl(socketFD, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(IMDS_PORT);
	address.sin_addr.s_addr = inet_addr(IMDS_ADDRESS);

	int result = connect(socketFD, (struct sockaddr*)&address, sizeof(address));
	if (result < 0 && errno == EINPROGRESS) {
		fd_set writeSet;
		FD_ZERO(&writeSet);
		FD_SET(socketFD, &writeSet);

		struct timeval timeout;
		timeout.tv_sec = IMDS_TIMEOUT_SECONDS;
		timeout.tv_usec = 0;

		if (select(socketFD + 1, NULL, &writeSet, NULL, &timeout) <= 0) {
			close(socketFD);
			return -1;
		}

		int error = 0;
		socklen_t length = sizeof(error);
		if (getsockopt(socketFD, SOL_SOCKET, SO_ERROR, &error, &length) < 0
			|| error != 0) {
			close(socketFD);
			return -1;
		}
	} else if (result < 0) {
		close(socketFD);
		return -1;
	}

	fcntl(socketFD, F_SETFL, flags);

	/* Belt and braces: bound the read and write too, so a half-open connection
	   cannot hang the job indefinitely. */
	struct timeval timeout;
	timeout.tv_sec = IMDS_TIMEOUT_SECONDS;
	timeout.tv_usec = 0;
	setsockopt(socketFD, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(socketFD, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	return socketFD;
}


static bool
write_all(int socketFD, const char* data, size_t length)
{
	size_t written = 0;
	while (written < length) {
		ssize_t result = write(socketFD, data + written, length - written);
		if (result <= 0)
			return false;
		written += result;
	}

	return true;
}


/*!	Performs one HTTP request and returns the response body.

	HTTP/1.0 with an explicit "Connection: close", so the body is simply
	everything up to EOF and no chunked or keep-alive handling is needed. IMDS
	is a fixed, tiny, local service; a general HTTP client would be many times
	the size of this whole program for no benefit.

	Returns the status code, or -1 if the exchange failed. On a 200 the body is
	copied into \a body, NUL terminated.
*/
static int
imds_request(const char* method, const char* path, const char* header,
	char* body, size_t bodySize)
{
	int socketFD = imds_connect();
	if (socketFD < 0)
		return -1;

	char request[1024];
	int length = snprintf(request, sizeof(request),
		"%s %s HTTP/1.0\r\n"
		"Host: " IMDS_ADDRESS "\r\n"
		"%s"
		"Connection: close\r\n"
		"\r\n",
		method, path, header != NULL ? header : "");
	if (length <= 0 || (size_t)length >= sizeof(request)) {
		close(socketFD);
		return -1;
	}

	if (!write_all(socketFD, request, length)) {
		close(socketFD);
		return -1;
	}

	char response[RESPONSE_MAX];
	size_t total = 0;
	while (total < sizeof(response) - 1) {
		ssize_t result = read(socketFD, response + total,
			sizeof(response) - 1 - total);
		if (result <= 0)
			break;
		total += result;
	}
	close(socketFD);
	response[total] = '\0';

	int status = 0;
	if (sscanf(response, "HTTP/%*d.%*d %d", &status) != 1)
		return -1;

	if (status != 200)
		return status;

	const char* separator = strstr(response, "\r\n\r\n");
	if (separator == NULL)
		return -1;
	separator += 4;

	size_t bodyLength = total - (separator - response);
	if (bodyLength >= bodySize)
		bodyLength = bodySize - 1;
	memcpy(body, separator, bodyLength);
	body[bodyLength] = '\0';

	/* Metadata values carry no trailing newline, but strip one defensively so a
	   key never lands in authorized_keys with stray whitespace. */
	while (bodyLength > 0
		&& (body[bodyLength - 1] == '\n' || body[bodyLength - 1] == '\r')) {
		body[--bodyLength] = '\0';
	}

	return status;
}


/*!	Appends \a key to authorized_keys unless it is already there.

	The comparison is on the key's type and base64 blob rather than the whole
	line, because EC2 appends the key-pair name as a comment and the same key
	baked into the image may carry a different one. Matching the whole line
	would append a duplicate every boot.
*/
static bool
merge_authorized_key(const char* key)
{
	/* Isolate "ssh-ed25519 AAAA...", dropping any trailing comment. */
	char blob[2048];
	strlcpy(blob, key, sizeof(blob));

	int spaces = 0;
	for (char* p = blob; *p != '\0'; p++) {
		if (*p != ' ')
			continue;
		if (++spaces == 2) {
			*p = '\0';
			break;
		}
	}

	if (spaces == 0) {
		/* Not a "type blob [comment]" line at all; refuse rather than write
		   something malformed into authorized_keys. */
		return false;
	}

	FILE* file = fopen(AUTHORIZED_KEYS, "r");
	if (file != NULL) {
		char line[2048];
		while (fgets(line, sizeof(line), file) != NULL) {
			if (strstr(line, blob) != NULL) {
				fclose(file);
				fprintf(stderr, "cloud_init_lite: key already present\n");
				return true;
			}
		}
		fclose(file);
	}

	file = fopen(AUTHORIZED_KEYS, "a");
	if (file == NULL) {
		fprintf(stderr, "cloud_init_lite: cannot open %s: %s\n",
			AUTHORIZED_KEYS, strerror(errno));
		return false;
	}

	/* A file that does not end in a newline would otherwise splice two keys into
	   one unusable line. ftell() on an append-mode stream is not dependable
	   before the first write, so read the last byte directly; the seek simply
	   fails on an empty file, which needs no newline anyway. */
	FILE* check = fopen(AUTHORIZED_KEYS, "r");
	if (check != NULL) {
		if (fseek(check, -1, SEEK_END) == 0 && fgetc(check) != '\n')
			fputc('\n', file);
		fclose(check);
	}

	fprintf(file, "%s\n", key);
	fclose(file);

	chmod(AUTHORIZED_KEYS, 0600);
	fprintf(stderr, "cloud_init_lite: installed the instance's SSH key\n");

	return true;
}


int
main(int argc, char** argv)
{
	char token[256];
	int status = -1;

	/* Wait for DHCP. Every attempt is a full token request, because a token
	   fetched before the interface was up is of no use anyway. */
	for (int attempt = 0; attempt < WAIT_ATTEMPTS; attempt++) {
		status = imds_request("PUT", TOKEN_PATH,
			"X-aws-ec2-metadata-token-ttl-seconds: 21600\r\n",
			token, sizeof(token));
		if (status == 200)
			break;
		sleep(1);
	}

	if (status != 200) {
		/* The ordinary case off EC2. Say so once, at a level nobody has to care
		   about, and exit successfully -- this is not a failure of the boot. */
		fprintf(stderr, "cloud_init_lite: no instance metadata service; "
			"leaving the baked-in configuration alone\n");
		return 0;
	}

	char tokenHeader[512];
	snprintf(tokenHeader, sizeof(tokenHeader),
		"X-aws-ec2-metadata-token: %s\r\n", token);

	/* The SSH key. A 404 means the instance was launched with no key pair,
	   which is legitimate: the baked-in key is still there. */
	char key[2048];
	status = imds_request("GET", KEY_PATH, tokenHeader, key, sizeof(key));
	if (status == 200)
		merge_authorized_key(key);
	else if (status == 404) {
		fprintf(stderr, "cloud_init_lite: instance has no key pair; "
			"keeping the baked-in key only\n");
	} else {
		fprintf(stderr, "cloud_init_lite: could not read the instance key "
			"(status %d)\n", status);
	}

	/* The hostname, best effort. Nothing depends on it, so a failure here is
	   not worth failing the job over. */
	char hostname[256];
	status = imds_request("GET", HOSTNAME_PATH, tokenHeader, hostname,
		sizeof(hostname));
	if (status == 200 && hostname[0] != '\0') {
		if (sethostname(hostname, strlen(hostname)) == 0)
			fprintf(stderr, "cloud_init_lite: hostname set to %s\n", hostname);
		else {
			fprintf(stderr, "cloud_init_lite: sethostname(%s) failed: %s\n",
				hostname, strerror(errno));
		}
	}

	return 0;
}
