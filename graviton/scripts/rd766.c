/*
 * rd766.c -- issue #766 harness for remote_broker's QUIC front door (M5a,
 * graviton/docs/remote-desktop-broker.md, "QUIC front door").
 *
 * Self-contained: it starts its own remote_broker (-q, on a free loopback
 * port, with a scratch settings directory holding its own token, viewer token
 * and session cookie) in front of a mock session port it serves itself, so
 * every rule can be asserted from both ends of the relay. The mock checks the
 * cookie frame the broker must present first, then echoes what it receives,
 * which is how a client-side check sees what reached the session port.
 *
 * WHAT IT ASSERTS (each is a wire rule of the spec)
 *    0  broker.quic, the QUIC advertisement, says exactly debeos-urp/1;
 *    1  a handshake offering any ALPN but debeos-urp/1 fails;
 *    2  the broker's certificate is the pin in broker.fingerprint;
 *    3  the right token gets RP_AUTH_RESULT 0, the session port gets the
 *       cookie frame first and the client's pipelined bytes after it, and
 *       bytes flow both ways;
 *    4  a wrong token gets RP_AUTH_RESULT 1, after the tarpit delay;
 *    5  failures are accounted per *source address*: an address in backoff is
 *       refused, a different address is not (this is what proves the broker
 *       learned the validated peer address OpenSSL 3.5 does not expose);
 *    6  a second stream closes the connection with application error 2;
 *    7  a unidirectional first stream closes it with application error 2;
 *    8  a viewer's input events never reach the session port, the rest does;
 *    9  the client's FIN ends the session orderly: the session port sees EOF,
 *       the client sees CONNECTION_CLOSE with application error 0;
 *   10  the session port closing ends it orderly the other way: FIN, error 0;
 *   11  a newly authenticated client takes the session over, and the old
 *       connection is closed with application error 1.
 *   14  a broker started without -q removes the advertisement it left.
 *   With --long, also:
 *   12  a session left idle past the idle timeout (30 s) is still alive.
 *   With --soak N, also:
 *   13  N sessions in a row each relay 300 kB both ways, every byte intact.
 *
 * Check 5 needs two usable source addresses: 127.0.0.2/127.0.0.3 where the
 * loopback has them (Linux), otherwise the host's own address and 127.0.0.1
 * (Haiku). Where neither pair is usable, the check says SKIP.
 *
 * USAGE: rd766 <path to remote_broker> [--long] [--soak N] [--keep]
 * EXIT:  0 all checks passed, 3 a check failed, 2 setup failure.
 *
 * BUILD
 *   Haiku (cross): $CROSS -O1 -o rd766 rd766.c -lssl -lcrypto -lnetwork
 *   Linux:         cc -O1 -o rd766 rd766.c -lssl -lcrypto -lpthread
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>


#define RP_INIT_CONNECTION		1
#define RP_UPDATE_DISPLAY_MODE	2
#define RP_AUTHENTICATE			10
#define RP_AUTH_RESULT			11
#define RP_SESSION_COOKIE		12
#define RP_MOUSE_MOVED			220
#define RP_MODIFIERS_CHANGED	244

// A code the mock session port treats as "close the session now".
#define MOCK_CLOSE_REQUEST		0x7fff

static const char kALPN[] = "\x0c" "debeos-urp/1";

static const char *sBrokerPath;
static char sDirectory[256];
static char sControllerToken[65];
static char sViewerToken[65];
static char sCookie[65];
static char sPin[65];
static int sPort;
static pid_t sBroker = -1;

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


static void
skip(const char *label, const char *why)
{
	printf("  SKIP  %s  %s\n", label, why);
	fflush(stdout);
}


static double
now()
{
	struct timeval time;
	gettimeofday(&time, NULL);
	return time.tv_sec + time.tv_usec / 1000000.0;
}


static void
put32(uint8_t *out, uint32_t value)
{
	for (int i = 0; i < 4; i++)
		out[i] = (uint8_t)(value >> (8 * i));
}


static uint32_t
get32(const uint8_t *in)
{
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
		| ((uint32_t)in[3] << 24);
}


static size_t
frame(uint8_t *out, uint16_t code, const void *payload, size_t payloadSize)
{
	out[0] = (uint8_t)(code & 0xff);
	out[1] = (uint8_t)(code >> 8);
	put32(out + 2, (uint32_t)(6 + payloadSize));
	if (payloadSize > 0)
		memcpy(out + 6, payload, payloadSize);
	return 6 + payloadSize;
}


static size_t
authFrame(uint8_t *out, const char *token)
{
	uint8_t payload[8 + 128];
	size_t length = strlen(token);
	put32(payload, 1);
	put32(payload + 4, (uint32_t)length);
	memcpy(payload + 8, token, length);
	return frame(out, RP_AUTHENTICATE, payload, 8 + length);
}


static void
randomHex(char *out)
{
	static const char kDigits[] = "0123456789abcdef";
	FILE *random = fopen("/dev/urandom", "r");
	for (int i = 0; i < 64; i++) {
		int value = random != NULL ? fgetc(random) : rand();
		out[i] = kDigits[value & 0xf];
	}
	out[64] = '\0';
	if (random != NULL)
		fclose(random);
}


static bool
writeFile(const char *name, const char *content)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", sDirectory, name);
	int handle = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (handle < 0)
		return false;
	bool ok = write(handle, content, strlen(content))
		== (ssize_t)strlen(content);
	close(handle);
	return ok;
}


// #pragma mark - mock session port


/*	One connection at a time, like app_server's session port. The first frame
	must be the broker's RP_SESSION_COOKIE carrying sCookie; then it sends a
	greeting (RP_INIT_CONNECTION) and echoes every byte it receives, except
	that a complete MOCK_CLOSE_REQUEST frame makes it close the connection.
*/

static int sMockListener = -1;
static int sMockPort;

static pthread_mutex_t sMockLock = PTHREAD_MUTEX_INITIALIZER;
static int sMockSessions = 0;
static int sMockCookieFailures = 0;
// Per session, by the order the mock accepted their cookies: whether that
// session's connection ended with a clean EOF from the broker.
#define MOCK_MAX_SESSIONS 256
static bool sMockEOF[MOCK_MAX_SESSIONS];


static bool
readFully(int handle, uint8_t *buffer, size_t size)
{
	while (size > 0) {
		ssize_t read = recv(handle, buffer, size, 0);
		if (read <= 0)
			return false;
		buffer += read;
		size -= read;
	}
	return true;
}


static void *
mockSession(void *data)
{
	int handle = (int)(intptr_t)data;

	uint8_t header[6 + 8];
	bool cookieOK = readFully(handle, header, sizeof(header))
		&& header[0] == RP_SESSION_COOKIE && header[1] == 0
		&& get32(header + 6) == 1 && get32(header + 10) == 64
		&& get32(header + 2) == 6 + 8 + 64;
	uint8_t cookie[64];
	cookieOK = cookieOK && readFully(handle, cookie, sizeof(cookie))
		&& memcmp(cookie, sCookie, 64) == 0;

	int session = -1;
	pthread_mutex_lock(&sMockLock);
	if (cookieOK)
		session = ++sMockSessions;
	else
		sMockCookieFailures++;
	pthread_mutex_unlock(&sMockLock);

	if (!cookieOK) {
		close(handle);
		return NULL;
	}

	uint8_t greeting[6];
	frame(greeting, RP_INIT_CONNECTION, NULL, 0);
	send(handle, greeting, sizeof(greeting), 0);

	// Echo, watching frame boundaries only to spot the close request.
	uint8_t buffer[16384];
	uint8_t pending[6];
	size_t pendingUsed = 0;
	uint32_t bodyRemaining = 0;
	while (true) {
		ssize_t read = recv(handle, buffer, sizeof(buffer), 0);
		if (read <= 0) {
			if (read == 0 && session < MOCK_MAX_SESSIONS) {
				pthread_mutex_lock(&sMockLock);
				sMockEOF[session] = true;
				pthread_mutex_unlock(&sMockLock);
			}
			break;
		}

		bool closeRequested = false;
		for (ssize_t i = 0; i < read; i++) {
			if (bodyRemaining > 0) {
				bodyRemaining--;
				continue;
			}
			pending[pendingUsed++] = buffer[i];
			if (pendingUsed == 6) {
				uint16_t code = pending[0] | (pending[1] << 8);
				bodyRemaining = get32(pending + 2) - 6;
				pendingUsed = 0;
				if (code == MOCK_CLOSE_REQUEST)
					closeRequested = true;
			}
		}

		if (closeRequested)
			break;
		send(handle, buffer, read, 0);
	}

	close(handle);
	return NULL;
}


static void *
mockListener(void *data)
{
	while (true) {
		int handle = accept(sMockListener, NULL, NULL);
		if (handle < 0)
			continue;
		pthread_t thread;
		pthread_create(&thread, NULL, mockSession, (void *)(intptr_t)handle);
		pthread_detach(thread);
	}
	return NULL;
}


static bool
startMock()
{
	sMockListener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sMockListener, (struct sockaddr *)&address, sizeof(address)) != 0
		|| listen(sMockListener, 8) != 0) {
		return false;
	}

	socklen_t length = sizeof(address);
	getsockname(sMockListener, (struct sockaddr *)&address, &length);
	sMockPort = ntohs(address.sin_port);

	pthread_t thread;
	pthread_create(&thread, NULL, mockListener, NULL);
	pthread_detach(thread);
	return true;
}


// #pragma mark - broker


static int
freePort()
{
	// A port free for both TCP and UDP, since the broker binds both.
	for (int attempt = 0; attempt < 50; attempt++) {
		int port = 20000 + (rand() % 20000);
		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);

		int tcp = socket(AF_INET, SOCK_STREAM, 0);
		int udp = socket(AF_INET, SOCK_DGRAM, 0);
		bool free = bind(tcp, (struct sockaddr *)&address, sizeof(address))
				== 0
			&& bind(udp, (struct sockaddr *)&address, sizeof(address)) == 0;
		close(tcp);
		close(udp);
		if (free)
			return port;
	}
	return -1;
}


static bool
brokerLogHas(const char *text)
{
	char path[512], line[512];
	snprintf(path, sizeof(path), "%s/broker.log", sDirectory);
	FILE *file = fopen(path, "r");
	if (file == NULL)
		return false;
	bool found = false;
	while (!found && fgets(line, sizeof(line), file) != NULL)
		found = strstr(line, text) != NULL;
	fclose(file);
	return found;
}


static bool
startBroker(bool quic)
{
	sPort = freePort();
	if (sPort < 0)
		return false;

	char listen[64], target[64], cookiePath[512], logPath[512];
	snprintf(listen, sizeof(listen), "127.0.0.1:%d", sPort);
	snprintf(target, sizeof(target), "127.0.0.1:%d", sMockPort);
	snprintf(cookiePath, sizeof(cookiePath), "%s/cookie", sDirectory);
	snprintf(logPath, sizeof(logPath), "%s/broker.log", sDirectory);

	sBroker = fork();
	if (sBroker < 0)
		return false;
	if (sBroker == 0) {
		int log = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(log, 1);
		dup2(log, 2);
		if (quic) {
			execl(sBrokerPath, sBrokerPath, "-q", "-l", listen, "-t", target,
				"-s", sDirectory, "-c", cookiePath, (char *)NULL);
		} else {
			execl(sBrokerPath, sBrokerPath, "-l", listen, "-t", target,
				"-s", sDirectory, "-c", cookiePath, (char *)NULL);
		}
		_exit(127);
	}

	// Ready when it says it is listening for QUIC, which it does once both
	// listeners are up -- after it has written the pin -- not after a guess
	// at how long that takes.
	char pinPath[512];
	snprintf(pinPath, sizeof(pinPath), "%s/broker.fingerprint", sDirectory);
	for (int i = 0; i < 200; i++) {
		usleep(50 * 1000);
		if (waitpid(sBroker, NULL, WNOHANG) != 0)
			return false;
		if (!brokerLogHas(quic ? "listening for QUIC" : "listening on port"))
			continue;
		FILE *file = fopen(pinPath, "r");
		if (file == NULL)
			return false;
		bool read = fgets(sPin, sizeof(sPin), file) != NULL;
		fclose(file);
		return read && strlen(sPin) == 64;
	}
	return false;
}


static void
stopBroker()
{
	if (sBroker > 0) {
		kill(sBroker, SIGTERM);
		waitpid(sBroker, NULL, 0);
		sBroker = -1;
	}
}


static void
showLog(const char *name, const char *title)
{
	char path[512], line[512];
	snprintf(path, sizeof(path), "%s/%s", sDirectory, name);
	FILE *file = fopen(path, "r");
	if (file == NULL)
		return;
	printf("---- %s ----\n", title);
	while (fgets(line, sizeof(line), file) != NULL)
		printf("  | %s", line);
	fclose(file);
}


static void
showBrokerLog()
{
	showLog("broker-q.log", "broker log (-q)");
	showLog("broker.log", "broker log");
}


// #pragma mark - QUIC client


/*	The harness needs an OpenSSL that keeps Haiku's negative errno intact
	(#790, fixed in DeBeOS's openssl3-3.5.9-2); on one that strips it, no QUIC
	handshake completes and every check would fail for that reason alone. */
static bool
errnoSignSurvives()
{
	ERR_set_mark();
	ERR_raise(ERR_LIB_SYS, EAGAIN);
	unsigned long packed = ERR_peek_last_error();
	bool intact = ERR_GET_REASON(packed) == EAGAIN
		&& BIO_err_is_non_fatal(packed) == 1;
	ERR_pop_to_mark();
	return intact;
}


// Session tickets the broker issued to this harness, and the largest
// max_early_data any of them allowed (0 means "no 0-RTT on resumption").
static int sTickets = 0;
static uint32_t sTicketEarlyData = 0;


static int
onTicket(SSL *ssl, SSL_SESSION *session)
{
	sTickets++;
	uint32_t early = SSL_SESSION_get_max_early_data(session);
	if (early > sTicketEarlyData)
		sTicketEarlyData = early;
	return 0;
		// not kept: the callback only records what was offered
}


typedef struct {
	SSL_CTX *context;
	SSL *connection;
	SSL *stream;
	int socket;
} Client;


static void
clientFree(Client *client)
{
	// Close like a real client would, so that the broker's session ends now
	// rather than at the idle timeout -- and does not end later, in the middle
	// of whichever check comes next.
	if (client->connection != NULL) {
		SSL_SHUTDOWN_EX_ARGS args;
		memset(&args, 0, sizeof(args));
		double deadline = now() + 0.5;
		while (SSL_shutdown_ex(client->connection, SSL_SHUTDOWN_FLAG_NO_BLOCK,
				&args, sizeof(args)) != 1 && now() < deadline) {
			SSL_handle_events(client->connection);
			usleep(2000);
		}
		ERR_clear_error();
	}
	SSL_free(client->stream);
	SSL_free(client->connection);
	SSL_CTX_free(client->context);
	if (client->socket >= 0)
		close(client->socket);
	memset(client, 0, sizeof(*client));
	client->socket = -1;
}


/*!	Connects from \a sourceAddress (NULL: any) offering \a alpn; handshake
	only. Returns false if the handshake did not complete within 5 s. */
static bool
clientConnect(Client *client, const char *sourceAddress, const char *alpn,
	size_t alpnLength)
{
	memset(client, 0, sizeof(*client));
	client->socket = -1;

	// The plain client method (contentive model), driven by this harness's
	// own SSL_handle_events() calls. Not the thread-assisted one: with it,
	// OpenSSL 3.5's client stopped taking server-to-client data after ~5.8 kB
	// (reproduced on Linux 3.5.8 and DeBeOS 3.5.7; openssl s_client, which
	// uses this method, receives the same stream from the same broker whole).
	client->context = SSL_CTX_new(OSSL_QUIC_client_method());
	if (client->context == NULL)
		return false;
	SSL_CTX_set_verify(client->context, SSL_VERIFY_NONE, NULL);
	SSL_CTX_set_session_cache_mode(client->context, SSL_SESS_CACHE_CLIENT);
	SSL_CTX_sess_set_new_cb(client->context, onTicket);

	client->socket = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	if (sourceAddress != NULL)
		inet_pton(AF_INET, sourceAddress, &local.sin_addr);
	if (bind(client->socket, (struct sockaddr *)&local, sizeof(local)) != 0)
		return false;

	client->connection = SSL_new(client->context);
	BIO *datagram = BIO_new_dgram(client->socket, BIO_NOCLOSE);
	SSL_set_bio(client->connection, datagram, datagram);
	SSL_set_alpn_protos(client->connection, (const unsigned char *)alpn,
		(unsigned int)alpnLength);
	SSL_set_default_stream_mode(client->connection,
		SSL_DEFAULT_STREAM_MODE_NONE);

	struct in_addr server;
	inet_pton(AF_INET, "127.0.0.1", &server);
	BIO_ADDR *peer = BIO_ADDR_new();
	BIO_ADDR_rawmake(peer, AF_INET, &server, sizeof(server), htons(sPort));
	SSL_set1_initial_peer_addr(client->connection, peer);
	BIO_ADDR_free(peer);

	SSL_set_blocking_mode(client->connection, 0);
	double deadline = now() + 5;
	while (true) {
		int result = SSL_connect(client->connection);
		if (result == 1)
			return true;
		int error = SSL_get_error(client->connection, result);
		if ((error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
			|| now() > deadline) {
			ERR_clear_error();
			return false;
		}
		SSL_handle_events(client->connection);
		usleep(2000);
	}
}


static bool
clientPinMatches(Client *client)
{
	X509 *certificate = SSL_get1_peer_certificate(client->connection);
	if (certificate == NULL)
		return false;
	unsigned char *der = NULL;
	int length = i2d_X509(certificate, &der);
	X509_free(certificate);
	if (length <= 0)
		return false;
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(der, length, digest);
	OPENSSL_free(der);
	char hex[65];
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
		sprintf(hex + 2 * i, "%02x", digest[i]);
	return strcmp(hex, sPin) == 0;
}


static bool
clientOpenStream(Client *client, bool unidirectional)
{
	client->stream = SSL_new_stream(client->connection,
		unidirectional ? SSL_STREAM_FLAG_UNI : 0);
	if (client->stream == NULL)
		return false;
	SSL_set_blocking_mode(client->stream, 0);
	return true;
}


static bool
clientWrite(Client *client, SSL *stream, const void *data, size_t size)
{
	const uint8_t *position = data;
	double deadline = now() + 5;
	while (size > 0) {
		size_t written = 0;
		if (SSL_write_ex(stream, position, size, &written) == 1) {
			position += written;
			size -= written;
			continue;
		}
		ERR_clear_error();
		if (now() > deadline
			|| SSL_get_stream_write_state(stream) != SSL_STREAM_STATE_OK) {
			return false;
		}
		SSL_handle_events(client->connection);
		usleep(2000);
	}
	SSL_handle_events(client->connection);
	return true;
}


/*!	Reads exactly \a size bytes within \a seconds. Returns the bytes read;
	*fin is set when the peer's FIN arrived (or the connection ended). */
static size_t
clientRead(Client *client, void *buffer, size_t size, double seconds,
	bool *fin)
{
	uint8_t *position = buffer;
	size_t total = 0;
	double deadline = now() + seconds;
	if (fin != NULL)
		*fin = false;
	while (total < size) {
		// Success with nothing read happens on DeBeOS's 3.5.7; looping on it
		// without servicing the connection starves the client's own event
		// processing (it is this harness that drives it), so it counts as
		// "nothing yet".
		size_t read = 0;
		if (SSL_read_ex(client->stream, position + total, size - total,
				&read) == 1 && read > 0) {
			total += read;
			continue;
		}
		ERR_clear_error();
		int state = SSL_get_stream_read_state(client->stream);
		if (state != SSL_STREAM_STATE_OK) {
			if (fin != NULL)
				*fin = true;
			break;
		}
		if (now() > deadline)
			break;
		SSL_handle_events(client->connection);
		usleep(2000);
	}
	return total;
}


/*!	Waits up to \a seconds for the connection to be closed by the peer.
	Returns the application error code, or -1 when it was not closed, -2 when
	it closed with a transport-level error instead. */
static int64_t
clientWaitClosed(Client *client, double seconds)
{
	double deadline = now() + seconds;
	while (now() < deadline) {
		SSL_CONN_CLOSE_INFO info;
		if (SSL_get_conn_close_info(client->connection, &info, sizeof(info))
				== 1) {
			if ((info.flags & SSL_CONN_CLOSE_FLAG_TRANSPORT) != 0)
				return -2;
			return (int64_t)info.error_code;
		}
		ERR_clear_error();
		SSL_handle_events(client->connection);
		usleep(5000);
	}
	return -1;
}


/*!	Connects, opens the stream, authenticates with \a token and pipelines
	\a extra behind the auth message. Returns the RP_AUTH_RESULT status, or -1
	if none arrived. */
static int
clientSession(Client *client, const char *source, const char *token,
	const uint8_t *extra, size_t extraSize)
{
	if (!clientConnect(client, source, kALPN, sizeof(kALPN) - 1)
		|| !clientOpenStream(client, false)) {
		return -1;
	}

	uint8_t message[512];
	size_t length = authFrame(message, token);
	if (extraSize > 0) {
		memcpy(message + length, extra, extraSize);
		length += extraSize;
	}
	if (!clientWrite(client, client->stream, message, length))
		return -1;

	uint8_t reply[10];
	if (clientRead(client, reply, sizeof(reply), 5, NULL) != sizeof(reply)
		|| reply[0] != RP_AUTH_RESULT || reply[1] != 0
		|| get32(reply + 2) != 10) {
		return -1;
	}
	return (int)get32(reply + 6);
}


static bool
sourceUsable(const char *address)
{
	int handle = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	inet_pton(AF_INET, address, &local.sin_addr);
	bool usable = bind(handle, (struct sockaddr *)&local, sizeof(local)) == 0;
	close(handle);
	return usable;
}


/*!	This host's primary IPv4 address, the source the stack would pick toward
	a non-local destination: a connected UDP socket sends nothing, it only
	selects a route. Used as a second source address where 127.0.0.2 is not
	one (Haiku's loopback holds 127.0.0.1 alone). */
static bool
primaryAddress(char *out, size_t size)
{
	int handle = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in remote;
	memset(&remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_port = htons(9);
	inet_pton(AF_INET, "10.255.255.254", &remote.sin_addr);
	struct sockaddr_in local;
	socklen_t length = sizeof(local);
	bool found = connect(handle, (struct sockaddr *)&remote, sizeof(remote))
			== 0
		&& getsockname(handle, (struct sockaddr *)&local, &length) == 0
		&& local.sin_addr.s_addr != htonl(INADDR_ANY)
		&& (ntohl(local.sin_addr.s_addr) >> 24) != 127
		&& inet_ntop(AF_INET, &local.sin_addr, out, size) != NULL;
	close(handle);
	return found && sourceUsable(out);
}


// #pragma mark - the checks


/*!	Reads broker.quic, the QUIC advertisement; returns false if it is not
	there. */
static bool
readAdvert(char *out, size_t size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/broker.quic", sDirectory);
	FILE *file = fopen(path, "r");
	if (file == NULL)
		return false;
	size_t length = fread(out, 1, size - 1, file);
	out[length] = '\0';
	fclose(file);
	return true;
}


static void
checkAdvert()
{
	char advert[64];
	bool present = readAdvert(advert, sizeof(advert));
	check("0 broker.quic advertises exactly debeos-urp/1",
		present && strcmp(advert, "debeos-urp/1\n") == 0,
		present ? advert : "absent");
}


/*!	Restarts the broker without -q in the same settings directory, where the
	-q run left broker.quic behind: it must go, since that broker serves no
	QUIC. */
static void
checkAdvertWithdrawn()
{
	stopBroker();
	char from[512], to[512];
	snprintf(from, sizeof(from), "%s/broker.log", sDirectory);
	snprintf(to, sizeof(to), "%s/broker-q.log", sDirectory);
	rename(from, to);
	bool started = startBroker(false);
	char advert[64];
	bool present = readAdvert(advert, sizeof(advert));
	stopBroker();
	check("14 a broker started without -q removes broker.quic",
		started && !present, started ? (present ? advert : NULL)
			: "broker did not come up");
}


static void
checkALPN()
{
	static const char kOther[] = "\x02" "h3";
	Client client;
	bool connected = clientConnect(&client, NULL, kOther, sizeof(kOther) - 1);
	check("1 handshake with another ALPN fails", !connected, NULL);
	clientFree(&client);
}


static void
checkSessionAndRelay()
{
	int before = sMockSessions;
	uint8_t init[6];
	frame(init, RP_INIT_CONNECTION, NULL, 0);

	Client client;
	bool connected = clientConnect(&client, NULL, kALPN, sizeof(kALPN) - 1);
	check("2 certificate matches broker.fingerprint",
		connected && clientPinMatches(&client), NULL);
	clientFree(&client);

	int status = clientSession(&client, NULL, sControllerToken, init,
		sizeof(init));
	char detail[128];
	snprintf(detail, sizeof(detail), "auth status %d", status);
	check("3a right token: RP_AUTH_RESULT 0", status == 0, detail);

	// The greeting the mock sends once it saw the cookie, then the echo of
	// the RP_INIT_CONNECTION pipelined behind the auth message.
	uint8_t received[12];
	size_t got = clientRead(&client, received, sizeof(received), 5, NULL);
	check("3b session port got the cookie first, then the pipelined bytes",
		got == sizeof(received) && sMockSessions == before + 1
			&& received[0] == RP_INIT_CONNECTION
			&& memcmp(received + 6, init, 6) == 0, NULL);

	uint8_t payload[30000];
	for (size_t i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(i * 7);
	uint8_t big[6 + sizeof(payload)];
	size_t length = frame(big, RP_UPDATE_DISPLAY_MODE, payload,
		sizeof(payload));
	uint8_t echo[sizeof(big)];
	bool sent = clientWrite(&client, client.stream, big, length);
	got = clientRead(&client, echo, length, 5, NULL);
	size_t same = 0;
	while (same < got && echo[same] == big[same])
		same++;
	snprintf(detail, sizeof(detail), "sent %d, %zu of %zu back, first %zu"
		" equal", sent, got, length, same);
	check("3c bytes flow both ways intact", sent && got == length
		&& same == length, detail);
	clientFree(&client);

	// Tickets arrive after the handshake, so by now the session above has
	// seen any the broker sends.
	snprintf(detail, sizeof(detail), "%d tickets, max_early_data %u",
		sTickets, sTicketEarlyData);
	// At least one ticket, or the check would pass on a broker that never
	// got as far as issuing one.
	check("3d no 0-RTT: the broker's tickets allow no early data",
		sTickets > 0 && sTicketEarlyData == 0, detail);
	printf("        (%s)\n", detail);
	usleep(200 * 1000);
}


static void
checkWrongTokenAndBackoff()
{
	// Two distinct source addresses: 127.0.0.2 and .3 where the loopback
	// has them (Linux), otherwise this host's own address and 127.0.0.1.
	static char sPrimary[INET_ADDRSTRLEN];
	const char *offender = NULL;
	const char *bystander = NULL;
	if (sourceUsable("127.0.0.2") && sourceUsable("127.0.0.3")) {
		offender = "127.0.0.2";
		bystander = "127.0.0.3";
	} else if (primaryAddress(sPrimary, sizeof(sPrimary))) {
		offender = sPrimary;
		bystander = "127.0.0.1";
	}
	bool split = offender != NULL;

	Client client;
	double start = now();
	int status = clientSession(&client, offender, "0123456789abcdef-wrong",
		NULL, 0);
	double took = now() - start;
	clientFree(&client);
	char detail[128];
	snprintf(detail, sizeof(detail), "status %d after %.2f s", status, took);
	check("4 wrong token: RP_AUTH_RESULT 1 after the tarpit delay",
		status == 1 && took >= 1.5, detail);

	if (!split) {
		skip("5 per-address backoff", "no second usable source address");
		return;
	}

	// A second failure from the same source, after the first backoff
	// (2 s) ran out: it is now blocked for 4 s.
	usleep(500 * 1000);
	status = clientSession(&client, offender, "0123456789abcdef-wrong", NULL,
		0);
	clientFree(&client);

	status = clientSession(&client, offender, sControllerToken, NULL, 0);
	int64_t closed = clientWaitClosed(&client, 2);
	clientFree(&client);
	snprintf(detail, sizeof(detail), "status %d, close %lld", status,
		(long long)closed);
	check("5a the address in backoff is refused before the token check",
		status == -1, detail);

	status = clientSession(&client, bystander, sControllerToken, NULL, 0);
	clientFree(&client);
	snprintf(detail, sizeof(detail), "status %d", status);
	check("5b another address is not", status == 0, detail);

	// Let the offender's backoff run out before anything else uses it.
	sleep(4);
}


static void
checkStreamRules()
{
	Client client;
	int status = clientSession(&client, NULL, sControllerToken, NULL, 0);
	uint8_t greeting[6];
	clientRead(&client, greeting, sizeof(greeting), 5, NULL);

	SSL *second = SSL_new_stream(client.connection, 0);
	uint8_t byte = 0;
	if (second != NULL) {
		SSL_set_blocking_mode(second, 0);
		clientWrite(&client, second, &byte, 1);
	}
	int64_t closed = clientWaitClosed(&client, 3);
	SSL_free(second);
	clientFree(&client);
	char detail[128];
	snprintf(detail, sizeof(detail), "auth %d, close %lld", status,
		(long long)closed);
	check("6 a second stream closes the connection with error 2",
		status == 0 && closed == 2, detail);

	bool connected = clientConnect(&client, NULL, kALPN, sizeof(kALPN) - 1)
		&& clientOpenStream(&client, true);
	uint8_t message[512];
	size_t length = authFrame(message, sControllerToken);
	if (connected)
		clientWrite(&client, client.stream, message, length);
	closed = connected ? clientWaitClosed(&client, 3) : -3;
	clientFree(&client);
	snprintf(detail, sizeof(detail), "close %lld", (long long)closed);
	check("7 a unidirectional first stream closes it with error 2",
		closed == 2, detail);
	usleep(200 * 1000);
}


static void
checkViewer()
{
	// input, display mode, input, display mode: only the display-mode
	// frames may come back.
	uint8_t frames[64];
	size_t length = 0;
	uint8_t move[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t mode[4] = {9, 9, 9, 9};
	length += frame(frames + length, RP_MOUSE_MOVED, move, sizeof(move));
	length += frame(frames + length, RP_UPDATE_DISPLAY_MODE, mode, 4);
	length += frame(frames + length, RP_MODIFIERS_CHANGED, move, 4);
	length += frame(frames + length, RP_UPDATE_DISPLAY_MODE, mode, 2);

	uint8_t expected[64];
	size_t expectedLength = 0;
	expectedLength += frame(expected, RP_UPDATE_DISPLAY_MODE, mode, 4);
	expectedLength += frame(expected + expectedLength, RP_UPDATE_DISPLAY_MODE,
		mode, 2);

	Client client;
	int status = clientSession(&client, NULL, sViewerToken, NULL, 0);
	uint8_t greeting[6];
	clientRead(&client, greeting, sizeof(greeting), 5, NULL);
	clientWrite(&client, client.stream, frames, length);

	uint8_t echo[64];
	size_t got = clientRead(&client, echo, expectedLength, 3, NULL);
	// Nothing more may follow: the input frames must not trail in late.
	uint8_t extra[8];
	size_t more = clientRead(&client, extra, sizeof(extra), 0.5, NULL);
	clientFree(&client);
	char detail[128];
	snprintf(detail, sizeof(detail), "auth %d, %zu+%zu bytes back", status,
		got, more);
	check("8 a viewer's input never reaches the session port",
		status == 0 && got == expectedLength && more == 0
			&& memcmp(echo, expected, expectedLength) == 0, detail);
	usleep(200 * 1000);
}


static void
checkOrderlyEnds()
{
	Client client;
	int status = clientSession(&client, NULL, sControllerToken, NULL, 0);
	uint8_t greeting[6];
	clientRead(&client, greeting, sizeof(greeting), 5, NULL);
	// The greeting is sent once the mock accepted the cookie, so this
	// session is the latest one it counted.
	pthread_mutex_lock(&sMockLock);
	int session = sMockSessions;
	pthread_mutex_unlock(&sMockLock);
	SSL_stream_conclude(client.stream, 0);
	SSL_handle_events(client.connection);
	int64_t closed = clientWaitClosed(&client, 3);
	clientFree(&client);
	usleep(200 * 1000);
	char detail[128];
	pthread_mutex_lock(&sMockLock);
	bool eof = session > 0 && session < MOCK_MAX_SESSIONS && sMockEOF[session];
	pthread_mutex_unlock(&sMockLock);
	snprintf(detail, sizeof(detail), "auth %d, close %lld, eof %d", status,
		(long long)closed, eof);
	check("9 client FIN: session port sees EOF, close is error 0",
		status == 0 && closed == 0 && eof, detail);

	status = clientSession(&client, NULL, sControllerToken, NULL, 0);
	clientRead(&client, greeting, sizeof(greeting), 5, NULL);
	uint8_t request[6];
	frame(request, MOCK_CLOSE_REQUEST, NULL, 0);
	clientWrite(&client, client.stream, request, sizeof(request));
	bool fin = false;
	uint8_t rest[8];
	clientRead(&client, rest, sizeof(rest), 3, &fin);
	closed = clientWaitClosed(&client, 3);
	clientFree(&client);
	snprintf(detail, sizeof(detail), "auth %d, fin %d, close %lld", status,
		fin, (long long)closed);
	check("10 session port closing: client sees FIN, close is error 0",
		status == 0 && fin && closed == 0, detail);
	usleep(200 * 1000);
}


static void
checkTakeover()
{
	Client first, second;
	int status1 = clientSession(&first, NULL, sControllerToken, NULL, 0);
	uint8_t greeting[6];
	clientRead(&first, greeting, sizeof(greeting), 5, NULL);
	int status2 = clientSession(&second, NULL, sControllerToken, NULL, 0);
	int64_t closed = clientWaitClosed(&first, 3);
	size_t got = clientRead(&second, greeting, sizeof(greeting), 3, NULL);
	clientFree(&first);
	clientFree(&second);
	char detail[128];
	snprintf(detail, sizeof(detail), "auth %d/%d, old close %lld", status1,
		status2, (long long)closed);
	check("11 takeover: the old connection is closed with error 1",
		status1 == 0 && status2 == 0 && closed == 1 && got == 6, detail);
	usleep(200 * 1000);
}


static void
checkIdle()
{
	Client client;
	int status = clientSession(&client, NULL, sControllerToken, NULL, 0);
	uint8_t greeting[6];
	clientRead(&client, greeting, sizeof(greeting), 5, NULL);

	// Idle, but keep servicing the connection the way any client's event
	// loop does; what is under test is whether either side lets it lapse.
	double until = now() + 40;
	while (now() < until) {
		SSL_handle_events(client.connection);
		usleep(50 * 1000);
	}

	uint8_t ping[6];
	frame(ping, RP_INIT_CONNECTION, NULL, 0);
	bool sent = clientWrite(&client, client.stream, ping, sizeof(ping));
	uint8_t echo[6];
	size_t got = clientRead(&client, echo, sizeof(echo), 3, NULL);
	clientFree(&client);
	char detail[128];
	snprintf(detail, sizeof(detail), "auth %d, sent %d, echo %zu", status,
		sent, got);
	check("12 an idle session outlives the idle timeout",
		status == 0 && sent && got == 6, detail);
}


/*!	--soak N: N sessions in a row, each relaying 300 kB through the broker
	and back in 16 kB round trips, every byte compared. A long-running relay
	is where a wait failing spuriously inside the broker shows up: it ends a
	healthy session early, which here reads as a short or failed echo. */
static void
checkSoak(int sessions)
{
	enum { kChunk = 16 * 1024, kTotal = 300 * 1024 };
	static uint8_t sent[6 + kChunk], echo[6 + kChunk];
	int good = 0;
	char detail[160] = "";
	for (int session = 0; session < sessions; session++) {
		Client client;
		int status = clientSession(&client, NULL, sControllerToken, NULL, 0);
		uint8_t greeting[6];
		bool ok = status == 0
			&& clientRead(&client, greeting, sizeof(greeting), 5, NULL) == 6;
		size_t relayed = 0;
		while (ok && relayed < kTotal) {
			uint8_t payload[kChunk];
			for (size_t i = 0; i < kChunk; i++)
				payload[i] = (uint8_t)(i * 13 + relayed / kChunk + session);
			size_t length = frame(sent, RP_UPDATE_DISPLAY_MODE, payload,
				kChunk);
			ok = clientWrite(&client, client.stream, sent, length)
				&& clientRead(&client, echo, length, 5, NULL) == length
				&& memcmp(echo, sent, length) == 0;
			if (ok)
				relayed += kChunk;
		}
		clientFree(&client);
		if (ok)
			good++;
		else if (detail[0] == '\0') {
			snprintf(detail, sizeof(detail), "session %d: auth %d, ended"
				" after %zu of %d bytes", session + 1, status, relayed,
				(int)kTotal);
		}
		usleep(100 * 1000);
	}

	char label[96];
	snprintf(label, sizeof(label), "13 soak: %d sessions of 300 kB each relay"
		" every byte", sessions);
	check(label, good == sessions, detail);
}


int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <remote_broker> [--long] [--soak N]"
			" [--keep]\n", argv[0]);
		return 2;
	}

	sBrokerPath = argv[1];
	bool runLong = false, keep = false;
	int soakSessions = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--long") == 0)
			runLong = true;
		else if (strcmp(argv[i], "--soak") == 0 && i + 1 < argc)
			soakSessions = atoi(argv[++i]);
		else if (strcmp(argv[i], "--keep") == 0)
			keep = true;
	}

	if (!errnoSignSurvives()) {
		printf("RD766=SETUP-FAILED (this OpenSSL strips the sign of Haiku's"
			" errno, #790: openssl3 >= 3.5.9-2 is required)\n");
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);
	srand((unsigned)(getpid() ^ (unsigned)now()));

	snprintf(sDirectory, sizeof(sDirectory), "/tmp/rd766.XXXXXX");
	if (mkdtemp(sDirectory) == NULL) {
		perror("mkdtemp");
		return 2;
	}

	randomHex(sControllerToken);
	randomHex(sViewerToken);
	randomHex(sCookie);
	if (!writeFile("token", sControllerToken)
		|| !writeFile("token.viewer", sViewerToken)
		|| !writeFile("cookie", sCookie) || !startMock()) {
		printf("RD766=SETUP-FAILED (scratch files or mock session port)\n");
		return 2;
	}

	if (!startBroker(true)) {
		printf("RD766=SETUP-FAILED (broker did not come up)\n");
		showBrokerLog();
		stopBroker();
		return 2;
	}

	printf("rd766: broker pid %d on 127.0.0.1:%d (tcp+udp), mock session port"
		" %d, scratch %s\n", (int)sBroker, sPort, sMockPort, sDirectory);

	checkAdvert();
	checkALPN();
	checkSessionAndRelay();
	checkWrongTokenAndBackoff();
	checkStreamRules();
	checkViewer();
	checkOrderlyEnds();
	checkTakeover();
	if (runLong)
		checkIdle();
	if (soakSessions > 0)
		checkSoak(soakSessions);

	check("the mock never saw a wrong or missing cookie",
		sMockCookieFailures == 0, NULL);

	// Last, since it replaces the broker the checks above ran against; the
	// log shown on failure is the restarted broker's.
	checkAdvertWithdrawn();
	if (sFailures > 0 || keep)
		showBrokerLog();

	printf("RD766=%s CHECKS=%d FAILED=%d\n", sFailures == 0 ? "PASS" : "FAIL",
		sChecks, sFailures);
	return sFailures == 0 ? 0 : 3;
}
