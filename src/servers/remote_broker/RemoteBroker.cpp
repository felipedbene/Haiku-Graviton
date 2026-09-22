/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 *
 * remote_broker -- the internet-facing front door of the remote desktop.
 *
 * app_server's RemoteHWInterface deliberately serves its unauthenticated,
 * unencrypted drawing protocol only on loopback (or a configured private
 * address). This daemon is what makes the desktop reachable from anywhere
 * else: it terminates TLS, speaks WebSocket toward the client (browser,
 * native client and test instrumentation all use the same transport), and
 * requires shared-token authentication before it proxies a single byte to
 * the session port. The broker is a separate process on purpose: the
 * network-facing TLS/HTTP parsing and the authentication decision are
 * crash- and exploit-isolated from the display server that owns every
 * keystroke, and app_server itself carries no crypto at all.
 *
 * Client handshake, all inside TLS:
 *   1. HTTP/1.1 WebSocket upgrade (subprotocol "binary").
 *   2. First binary payload: an RP-framed RP_AUTHENTICATE message
 *      (uint32 method = 1, length-prefixed token string).
 *   3. Broker answers RP_AUTH_RESULT (uint32 status, 0 = success) and only
 *      then connects to the loopback session port and relays bytes.
 * Failed authentication is answered after a delay, counted per source
 * address with exponential backoff, and never touches the session. A newly
 * authenticated client replaces the current session (matching the
 * newest-wins reconnect model of the session port); an unauthenticated
 * connection can neither displace nor observe it.
 *
 * Toward the session port the broker presents app_server's per-boot session
 * cookie (RP_SESSION_COOKIE), read from the owner-only file app_server
 * published before it began listening. That is what makes the broker's
 * authentication mean something on a machine where other processes can also
 * reach loopback: reaching the port is no longer enough, reading the cookie
 * file is required too. The cookie is read per connection rather than once at
 * startup, because app_server mints it when its listener is created -- which
 * may be long after this daemon started -- and replaces it if that happens
 * again.
 */

#include "TLSStream.h"
#include "WebSocketStream.h"

#include <FindDirectory.h>
#include <Locker.h>
#include <Autolock.h>
#include <OS.h>
#include <Path.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <errno.h>
#include <new>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


#define TRACE_LOG(x...)		printf("remote_broker: " x)
#define TRACE_ERROR(x...)	fprintf(stderr, "remote_broker: " x)


// Wire constants shared with the remote drawing protocol; the values are
// reserved in src/servers/app/drawing/interface/remote/RemoteMessage.h.
static const uint16 kRPAuthenticate = 10;
static const uint16 kRPAuthResult = 11;
static const uint16 kRPSessionCookie = 12;

static const uint32 kAuthMethodSharedToken = 1;
static const uint32 kCookieMethodPerBoot = 1;

static const uint32 kAuthResultSuccess = 0;
static const uint32 kAuthResultDenied = 1;
static const uint32 kAuthResultNoSession = 2;
// The client authenticated, but this daemon cannot read the session cookie and
// so cannot open a session on its behalf. Reported separately from
// kAuthResultNoSession because the remedy is different and entirely
// server-side: app_server has not created its listener yet, or is running as a
// user whose cookie file this process may not read.
static const uint32 kAuthResultNoCookie = 3;

// The complete TLS + WebSocket + authentication handshake must finish within
// this budget or the connection is dropped.
static const bigtime_t kHandshakeTimeout = 15 * 1000 * 1000;

// Answering a failed authentication attempt is delayed by this much, on top
// of the per-address backoff that gates the next attempt.
static const bigtime_t kAuthFailureDelay = 2 * 1000 * 1000;

static const int kMaxConcurrentHandshakes = 8;

static const size_t kMaxAuthMessageSize = 4096;
static const size_t kMaxTokenLength = 1024;

// Matches RP_SESSION_COOKIE_MAX_LENGTH in RemoteMessage.h: a cookie longer than
// app_server would ever accept is rejected here rather than sent and refused.
static const size_t kMaxCookieLength = 256;
// A cookie this short is not one app_server minted (that is 64 hex characters),
// so it is refused rather than presented -- the same floor the token has.
static const size_t kMinCookieLength = 16;


static int32 sConcurrentHandshakes = 0;


// #pragma mark - rate limiting


/*!	Per source address failure accounting with exponential backoff. The token
	is 256 bits of randomness, so brute force is not a real threat; this
	exists to keep noise, scanners and mistakes from hammering the handshake
	path.
*/
class RateLimiter {
public:
	RateLimiter()
		:
		fLock("remote_broker rate limiter")
	{
		memset(fEntries, 0, sizeof(fEntries));
	}

	bool IsBlocked(in_addr_t address)
	{
		BAutolock lock(fLock);
		Entry* entry = _FindEntry(address);
		if (entry == NULL || entry->failures == 0)
			return false;

		return system_time() < entry->lastFailure + _Backoff(entry->failures);
	}

	void RecordFailure(in_addr_t address)
	{
		BAutolock lock(fLock);
		Entry* entry = _FindEntry(address);
		if (entry == NULL) {
			entry = _OldestEntry();
			entry->address = address;
			entry->failures = 0;
		}

		entry->failures++;
		entry->lastFailure = system_time();
	}

	void RecordSuccess(in_addr_t address)
	{
		BAutolock lock(fLock);
		Entry* entry = _FindEntry(address);
		if (entry != NULL)
			entry->failures = 0;
	}

private:
	struct Entry {
		in_addr_t	address;
		int32		failures;
		bigtime_t	lastFailure;
	};

	static bigtime_t _Backoff(int32 failures)
	{
		int32 shift = failures < 6 ? failures : 6;
		return ((bigtime_t)1 << shift) * 1000 * 1000;
	}

	Entry* _FindEntry(in_addr_t address)
	{
		bigtime_t now = system_time();
		for (size_t i = 0; i < B_COUNT_OF(fEntries); i++) {
			Entry& entry = fEntries[i];
			if (entry.failures == 0)
				continue;
			// Entries decay; a quarter hour of silence forgives everything.
			if (now - entry.lastFailure > (bigtime_t)15 * 60 * 1000 * 1000) {
				entry.failures = 0;
				continue;
			}
			if (entry.address == address)
				return &entry;
		}

		return NULL;
	}

	Entry* _OldestEntry()
	{
		Entry* oldest = &fEntries[0];
		for (size_t i = 0; i < B_COUNT_OF(fEntries); i++) {
			if (fEntries[i].failures == 0)
				return &fEntries[i];
			if (fEntries[i].lastFailure < oldest->lastFailure)
				oldest = &fEntries[i];
		}

		return oldest;
	}

	BLocker	fLock;
	Entry	fEntries[64];
};


// #pragma mark - session registry


/*!	At most one client session is live at a time; a newly *authenticated*
	client takes the session over. Takeover only shuts the previous session's
	sockets down (waking its relay thread, which then cleans up what it owns)
	-- it never closes another thread's file descriptors.
*/
class SessionRegistry {
public:
	SessionRegistry()
		:
		fLock("remote_broker session registry"),
		fClientSocket(-1),
		fBackendSocket(-1),
		fOwner(NULL)
	{
	}

	void Adopt(void* owner, int clientSocket, int backendSocket)
	{
		BAutolock lock(fLock);
		if (fOwner != NULL) {
			TRACE_LOG("replacing live session with newly authenticated "
				"client\n");
			shutdown(fClientSocket, SHUT_RDWR);
			shutdown(fBackendSocket, SHUT_RDWR);
		}

		fOwner = owner;
		fClientSocket = clientSocket;
		fBackendSocket = backendSocket;
	}

	void Remove(void* owner)
	{
		BAutolock lock(fLock);
		if (fOwner == owner) {
			fOwner = NULL;
			fClientSocket = -1;
			fBackendSocket = -1;
		}
	}

private:
	BLocker	fLock;
	int		fClientSocket;
	int		fBackendSocket;
	void*	fOwner;
};


// #pragma mark - configuration


struct BrokerConfiguration {
	in_addr_t	listenAddress;
	uint16		listenPort;
	in_addr_t	targetAddress;
	uint16		targetPort;
	BPath		settingsDirectory;
	uint8		tokenDigest[SHA256_DIGEST_LENGTH];
	// The file app_server publishes the session cookie for the port this
	// daemon proxies to. Read per connection, never cached: see the header
	// comment.
	BPath		cookiePath;
};

static BrokerConfiguration sConfiguration;
static TLSContext sTLSContext;
static RateLimiter sRateLimiter;
static SessionRegistry sSessions;


static status_t
write_file(const char* path, const void* data, size_t size, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return errno;

	ssize_t written = write(fd, data, size);
	close(fd);
	return written == (ssize_t)size ? B_OK : B_IO_ERROR;
}


/*!	Generates the first-boot self-signed server certificate: a P-256 key and
	a ten year certificate, both PEM. A real CA-issued certificate can later
	be dropped into the same files (the certificate file is loaded as a
	chain).
*/
static status_t
generate_certificate(const char* certificatePath, const char* keyPath)
{
	status_t result = B_ERROR;
	X509* certificate = NULL;
	FILE* file = NULL;
	int fd = -1;

	EVP_PKEY* key = EVP_EC_gen("P-256");
	if (key == NULL)
		return B_ERROR;

	certificate = X509_new();
	if (certificate == NULL)
		goto out;

	{
		X509_set_version(certificate, 2);

		// Random positive serial; some clients dislike serial 0.
		unsigned char serialBytes[8];
		RAND_bytes(serialBytes, sizeof(serialBytes));
		serialBytes[0] &= 0x7f;
		BIGNUM* serial = BN_bin2bn(serialBytes, sizeof(serialBytes), NULL);
		BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(certificate));
		BN_free(serial);

		X509_gmtime_adj(X509_getm_notBefore(certificate), 0);
		X509_gmtime_adj(X509_getm_notAfter(certificate),
			(long)10 * 365 * 24 * 60 * 60);

		char hostname[256] = "debeos-remote-desktop";
		gethostname(hostname, sizeof(hostname) - 1);

		X509_NAME* name = X509_get_subject_name(certificate);
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
			(const unsigned char*)hostname, -1, -1, 0);
		X509_set_issuer_name(certificate, name);

		// A subjectAltName, or modern clients reject the certificate outright
		// (CN-only has not been accepted for years). The address a client
		// dials is not known here, so the name is the host name plus
		// loopback; the trust decision is the fingerprint pin either way,
		// and a browser accepting the certificate once needs the extension
		// to exist at all.
		char altNames[512];
		snprintf(altNames, sizeof(altNames), "DNS:%s,IP:127.0.0.1",
			hostname);
		X509_EXTENSION* extension = X509V3_EXT_conf_nid(NULL, NULL,
			NID_subject_alt_name, altNames);
		if (extension != NULL) {
			X509_add_ext(certificate, extension, -1);
			X509_EXTENSION_free(extension);
		}

		if (X509_set_pubkey(certificate, key) != 1)
			goto out;

		if (X509_sign(certificate, key, EVP_sha256()) == 0)
			goto out;
	}

	// The private key never passes through a world-readable state.
	fd = open(keyPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		goto out;
	file = fdopen(fd, "w");
	if (file == NULL)
		goto out;
	fd = -1;
	if (PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL) != 1)
		goto out;
	fclose(file);

	file = fopen(certificatePath, "w");
	if (file == NULL)
		goto out;
	if (PEM_write_X509(file, certificate) != 1)
		goto out;

	result = B_OK;

out:
	if (file != NULL)
		fclose(file);
	if (fd >= 0)
		close(fd);
	if (certificate != NULL)
		X509_free(certificate);
	EVP_PKEY_free(key);

	if (result != B_OK) {
		unlink(keyPath);
		unlink(certificatePath);
	}

	return result;
}


/*!	Writes the certificate's SHA-256 fingerprint next to it, for the operator
	to distribute to clients as the pin.
*/
static status_t
write_fingerprint(const char* certificatePath, const char* fingerprintPath,
	char* fingerprint)
{
	FILE* file = fopen(certificatePath, "r");
	if (file == NULL)
		return errno;

	X509* certificate = PEM_read_X509(file, NULL, NULL, NULL);
	fclose(file);
	if (certificate == NULL)
		return B_BAD_DATA;

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digestLength = 0;
	int result = X509_digest(certificate, EVP_sha256(), digest,
		&digestLength);
	X509_free(certificate);
	if (result != 1 || digestLength != SHA256_DIGEST_LENGTH)
		return B_ERROR;

	for (unsigned int i = 0; i < digestLength; i++)
		sprintf(fingerprint + 2 * i, "%02x", digest[i]);

	char line[TLS_FINGERPRINT_HEX_LENGTH + 2];
	snprintf(line, sizeof(line), "%s\n", fingerprint);
	return write_file(fingerprintPath, line, strlen(line), 0644);
}


/*!	Loads the shared token, generating a random one on first run. Only its
	SHA-256 stays in memory; comparison against it cannot leak the token
	through timing (both sides are hashed, then compared in constant time).
*/
static status_t
ensure_token(const char* tokenPath, uint8* digest)
{
	int fd = open(tokenPath, O_RDONLY);
	if (fd < 0) {
		// First run: 32 bytes of randomness, stored as hex, owner-only.
		unsigned char randomBytes[32];
		if (RAND_bytes(randomBytes, sizeof(randomBytes)) != 1)
			return B_ERROR;

		char token[sizeof(randomBytes) * 2 + 2];
		for (size_t i = 0; i < sizeof(randomBytes); i++)
			sprintf(token + 2 * i, "%02x", randomBytes[i]);
		token[sizeof(randomBytes) * 2] = '\n';
		token[sizeof(randomBytes) * 2 + 1] = '\0';

		status_t result = write_file(tokenPath, token, strlen(token), 0600);
		if (result != B_OK)
			return result;

		TRACE_LOG("generated authentication token in %s\n", tokenPath);
		fd = open(tokenPath, O_RDONLY);
		if (fd < 0)
			return errno;
	}

	// One byte more than the maximum is read so an over-long token is
	// rejected rather than silently truncated -- a truncated token would
	// still "work" for a client that sends the same truncation and quietly
	// throw the rest of the operator's secret away.
	char buffer[kMaxTokenLength + 2];
	ssize_t length = read(fd, buffer, kMaxTokenLength + 1);
	close(fd);
	if (length < 0)
		return errno;

	if ((size_t)length > kMaxTokenLength) {
		TRACE_ERROR("token in %s is longer than %zu characters\n", tokenPath,
			kMaxTokenLength);
		OPENSSL_cleanse(buffer, sizeof(buffer));
		return B_BAD_DATA;
	}

	while (length > 0 && (buffer[length - 1] == '\n'
			|| buffer[length - 1] == '\r' || buffer[length - 1] == ' ')) {
		length--;
	}

	if (length < 16) {
		TRACE_ERROR("token in %s is shorter than 16 characters; refusing to "
			"serve with a weak token\n", tokenPath);
		return B_BAD_DATA;
	}

	SHA256((const unsigned char*)buffer, length, digest);
	OPENSSL_cleanse(buffer, sizeof(buffer));
	return B_OK;
}


/*!	Reads the session cookie app_server published for the port this daemon
	proxies to. Called once per connection, after the client authenticated and
	before the session port is dialled.

	Deliberately not cached: app_server mints the cookie when it creates its
	listener, which can happen after this daemon started and again if the
	listener is recreated, so a value read at startup would be the one value
	guaranteed to go stale. It is a small file read on a path that already does
	a TLS handshake.
*/
static status_t
read_session_cookie(char* cookie, size_t cookieSize, size_t& cookieLength)
{
	if (sConfiguration.cookiePath.InitCheck() != B_OK)
		return B_NO_INIT;

	int fd = open(sConfiguration.cookiePath.Path(), O_RDONLY);
	if (fd < 0)
		return errno;

	// One byte more than the maximum, so an over-long cookie is rejected
	// rather than silently truncated into something that cannot match.
	char buffer[kMaxCookieLength + 2];
	if (cookieSize < sizeof(buffer) - 2) {
		close(fd);
		return B_BUFFER_OVERFLOW;
	}

	ssize_t length = read(fd, buffer, kMaxCookieLength + 1);
	close(fd);
	if (length < 0)
		return errno;

	if ((size_t)length > kMaxCookieLength) {
		TRACE_ERROR("session cookie in %s is longer than %zu characters\n",
			sConfiguration.cookiePath.Path(), kMaxCookieLength);
		OPENSSL_cleanse(buffer, sizeof(buffer));
		return B_BAD_DATA;
	}

	while (length > 0 && (buffer[length - 1] == '\n'
			|| buffer[length - 1] == '\r' || buffer[length - 1] == ' ')) {
		length--;
	}

	if ((size_t)length < kMinCookieLength) {
		TRACE_ERROR("session cookie in %s is shorter than %zu characters\n",
			sConfiguration.cookiePath.Path(), kMinCookieLength);
		OPENSSL_cleanse(buffer, sizeof(buffer));
		return B_BAD_DATA;
	}

	memcpy(cookie, buffer, length);
	cookieLength = (size_t)length;
	OPENSSL_cleanse(buffer, sizeof(buffer));
	return B_OK;
}


static status_t
prepare_settings(BrokerConfiguration& configuration)
{
	BPath directory = configuration.settingsDirectory;
	if (directory.InitCheck() != B_OK) {
		if (find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &directory) != B_OK)
			return B_ERROR;
		directory.Append("remote_desktop");
		configuration.settingsDirectory = directory;
	}

	// Where app_server publishes the cookie for the port we proxy to, unless an
	// explicit path was given. Keyed on the port, because each remote listener
	// has its own cookie -- and derived from the *system* settings directory
	// even when -s moved this daemon's own files elsewhere, because the cookie
	// is app_server's file and app_server writes it there unconditionally.
	if (configuration.cookiePath.InitCheck() != B_OK) {
		BPath cookieDirectory;
		if (find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &cookieDirectory)
				!= B_OK) {
			return B_ERROR;
		}

		cookieDirectory.Append("remote_desktop");

		char name[64];
		snprintf(name, sizeof(name), "session_cookie.%u",
			configuration.targetPort);
		configuration.cookiePath.SetTo(cookieDirectory.Path(), name);
	}

	TRACE_LOG("expecting app_server's session cookie in %s\n",
		configuration.cookiePath.Path());

	mkdir(directory.Path(), 0755);

	BPath certificatePath(directory.Path(), "broker.pem");
	BPath keyPath(directory.Path(), "broker.key");
	BPath fingerprintPath(directory.Path(), "broker.fingerprint");
	BPath tokenPath(directory.Path(), "token");

	struct stat st;
	if (stat(certificatePath.Path(), &st) != 0
		|| stat(keyPath.Path(), &st) != 0) {
		TRACE_LOG("generating self-signed certificate in %s\n",
			directory.Path());
		status_t result = generate_certificate(certificatePath.Path(),
			keyPath.Path());
		if (result != B_OK) {
			TRACE_ERROR("certificate generation failed\n");
			return result;
		}
	}

	char fingerprint[TLS_FINGERPRINT_HEX_LENGTH + 1];
	status_t result = write_fingerprint(certificatePath.Path(),
		fingerprintPath.Path(), fingerprint);
	if (result != B_OK)
		return result;

	TRACE_LOG("certificate SHA-256 fingerprint (pin): %s\n", fingerprint);

	result = ensure_token(tokenPath.Path(), configuration.tokenDigest);
	if (result != B_OK)
		return result;

	return sTLSContext.InitServer(certificatePath.Path(), keyPath.Path());
}


// #pragma mark - authentication


/*!	Reads and verifies the RP_AUTHENTICATE message that must be the client's
	very first payload. On success any bytes the client pipelined after the
	message are left in \a buffer (\a extraOffset/\a extraLength) to be
	forwarded once the session is connected.
*/
static status_t
authenticate_client(WebSocketStream& ws, bigtime_t deadline, uint8* buffer,
	size_t bufferSize, size_t& extraOffset, size_t& extraLength)
{
	size_t used = 0;

	// Read at least the frame header, then the complete message.
	uint32 messageLength = 0;
	while (true) {
		if (used >= 6) {
			uint16 code = (uint16)buffer[0] | ((uint16)buffer[1] << 8);
			messageLength = (uint32)buffer[2] | ((uint32)buffer[3] << 8)
				| ((uint32)buffer[4] << 16) | ((uint32)buffer[5] << 24);

			if (code != kRPAuthenticate || messageLength < 6 + 8
				|| messageLength > kMaxAuthMessageSize) {
				return B_NOT_ALLOWED;
			}

			if (used >= messageLength)
				break;
		}

		if (used == bufferSize)
			return B_NOT_ALLOWED;

		ssize_t read = ws.ReadSome(buffer + used, bufferSize - used,
			deadline);
		if (read <= 0)
			return read < 0 ? (status_t)read : B_IO_ERROR;
		used += read;
	}

	uint32 method = (uint32)buffer[6] | ((uint32)buffer[7] << 8)
		| ((uint32)buffer[8] << 16) | ((uint32)buffer[9] << 24);
	uint32 tokenLength = (uint32)buffer[10] | ((uint32)buffer[11] << 8)
		| ((uint32)buffer[12] << 16) | ((uint32)buffer[13] << 24);

	if (method != kAuthMethodSharedToken || tokenLength > kMaxTokenLength
		|| 6 + 8 + tokenLength != messageLength) {
		return B_NOT_ALLOWED;
	}

	// Hash-then-compare: constant time, and independent of the length of
	// the correct token.
	uint8 digest[SHA256_DIGEST_LENGTH];
	SHA256(buffer + 14, tokenLength, digest);

	bool match = CRYPTO_memcmp(digest, sConfiguration.tokenDigest,
		SHA256_DIGEST_LENGTH) == 0;

	if (!match)
		return B_PERMISSION_DENIED;

	extraOffset = messageLength;
	extraLength = used - messageLength;
	return B_OK;
}


static status_t
send_auth_result(WebSocketStream& ws, uint32 status, bigtime_t deadline)
{
	// Explicitly little-endian, like all remote protocol framing.
	uint8 reply[10];
	reply[0] = (uint8)(kRPAuthResult & 0xff);
	reply[1] = (uint8)(kRPAuthResult >> 8);
	uint32 length = sizeof(reply);
	for (int i = 0; i < 4; i++) {
		reply[2 + i] = (uint8)(length >> (8 * i));
		reply[6 + i] = (uint8)(status >> (8 * i));
	}
	return ws.WriteAll(reply, sizeof(reply), deadline);
}


// #pragma mark - relaying


/*!	Connects to the session port; returns the socket or -1. */
static int
connect_backend()
{
	int backendSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (backendSocket < 0)
		return -1;

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = sConfiguration.targetAddress;
	address.sin_port = htons(sConfiguration.targetPort);

	if (connect(backendSocket, (struct sockaddr*)&address,
			sizeof(address)) != 0) {
		close(backendSocket);
		return -1;
	}

	int noDelay = 1;
	setsockopt(backendSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
		sizeof(noDelay));

	return backendSocket;
}


static status_t
write_fully(int fd, const uint8* buffer, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, buffer, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		buffer += written;
		size -= written;
	}

	return B_OK;
}


/*!	Presents the session cookie to the freshly connected session port. It must
	be the first thing written to that socket: app_server's candidate gate reads
	exactly this frame, decides on it, and reads nothing else until it has
	promoted the connection -- so anything sent ahead of it would be refused as
	"not a session cookie" instead.
*/
static status_t
send_session_cookie(int backendSocket, const char* cookie, size_t cookieLength)
{
	// Explicitly little-endian, like all remote protocol framing.
	uint8 frame[6 + 8 + kMaxCookieLength];
	uint32 length = (uint32)(6 + 8 + cookieLength);
	if (cookieLength > kMaxCookieLength)
		return B_BAD_VALUE;

	frame[0] = (uint8)(kRPSessionCookie & 0xff);
	frame[1] = (uint8)(kRPSessionCookie >> 8);
	for (int i = 0; i < 4; i++) {
		frame[2 + i] = (uint8)(length >> (8 * i));
		frame[6 + i] = (uint8)(kCookieMethodPerBoot >> (8 * i));
		frame[10 + i] = (uint8)((uint32)cookieLength >> (8 * i));
	}
	memcpy(frame + 14, cookie, cookieLength);

	status_t result = write_fully(backendSocket, frame, length);
	OPENSSL_cleanse(frame, sizeof(frame));
	return result;
}


// #pragma mark - per connection handler


struct ConnectionContext {
	int			socket;
	in_addr_t	peerAddress;
	char		peerName[INET_ADDRSTRLEN];
};


static status_t
handle_connection(void* data)
{
	ConnectionContext* context = (ConnectionContext*)data;
	bigtime_t deadline = system_time() + kHandshakeTimeout;

	TLSStream tls;
	status_t result = tls.Accept(sTLSContext, context->socket, deadline);
	if (result != B_OK) {
		// Deliberately NOT counted as an authentication failure. The
		// documented first-use flow produces exactly these: a browser
		// aborting the handshake on the not-yet-trusted self-signed
		// certificate, a plain http:// probe, a favicon fetch. Counting
		// them would block the user's address before their first real
		// connection, with nothing on screen to explain why. Brute force is
		// what the limiter is for, and brute force has to reach the token
		// check to make progress.
		TRACE_LOG("%s: TLS handshake failed\n", context->peerName);
		atomic_add(&sConcurrentHandshakes, -1);
		delete context;
		return result;
	}

	WebSocketStream ws(tls);
	result = ws.AcceptHandshake(deadline);
	if (result != B_OK) {
		TRACE_LOG("%s: WebSocket handshake failed\n", context->peerName);
		atomic_add(&sConcurrentHandshakes, -1);
		delete context;
		return result;
	}

	uint8 authBuffer[kMaxAuthMessageSize];
	size_t extraOffset = 0;
	size_t extraLength = 0;
	result = authenticate_client(ws, deadline, authBuffer,
		sizeof(authBuffer), extraOffset, extraLength);

	if (result != B_OK) {
		OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
		TRACE_LOG("%s: authentication failed\n", context->peerName);
		sRateLimiter.RecordFailure(context->peerAddress);

		// Give the handshake slot up BEFORE the tarpit delay: this thread
		// only has a fixed 10 byte denial left to write, and holding one of
		// the few concurrent-handshake slots through a deliberate delay
		// would spend the broker's capacity rather than the attacker's.
		atomic_add(&sConcurrentHandshakes, -1);

		snooze(kAuthFailureDelay);
		send_auth_result(ws, kAuthResultDenied, system_time() + 1000000);
		ws.SendClose(1008, system_time() + 1000000);
		delete context;
		return result;
	}

	sRateLimiter.RecordSuccess(context->peerAddress);

	// Read the cookie before dialling the session port: a broker that cannot
	// present one has nothing to open a session with, and saying so costs the
	// session port no connection at all.
	char cookie[kMaxCookieLength];
	size_t cookieLength = 0;
	status_t cookieResult = read_session_cookie(cookie, sizeof(cookie),
		cookieLength);
	if (cookieResult != B_OK) {
		TRACE_ERROR("%s: cannot read the session cookie from %s: %s\n",
			context->peerName, sConfiguration.cookiePath.Path(),
			strerror(cookieResult));
		send_auth_result(ws, kAuthResultNoCookie, system_time() + 1000000);
		ws.SendClose(1011, system_time() + 1000000);
		atomic_add(&sConcurrentHandshakes, -1);
		OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
		delete context;
		return cookieResult;
	}

	int backendSocket = connect_backend();
	if (backendSocket < 0) {
		TRACE_ERROR("%s: session port unreachable\n", context->peerName);
		send_auth_result(ws, kAuthResultNoSession, system_time() + 1000000);
		ws.SendClose(1011, system_time() + 1000000);
		atomic_add(&sConcurrentHandshakes, -1);
		OPENSSL_cleanse(cookie, sizeof(cookie));
		delete context;
		return B_ERROR;
	}

	// The cookie frame goes first, ahead of the client's own bytes: it is what
	// the session port's gate reads, and anything before it would be refused in
	// its place.
	status_t forwardResult = send_session_cookie(backendSocket, cookie,
		cookieLength);
	OPENSSL_cleanse(cookie, sizeof(cookie));

	if (forwardResult == B_OK) {
		forwardResult = send_auth_result(ws, kAuthResultSuccess,
			system_time() + 1000000);
	}

	if (forwardResult == B_OK && extraLength > 0) {
		forwardResult = write_fully(backendSocket, authBuffer + extraOffset,
			extraLength);
	}
	// The token (and everything else read pre-session) is no longer needed.
	OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
	if (forwardResult != B_OK) {
		close(backendSocket);
		atomic_add(&sConcurrentHandshakes, -1);
		delete context;
		return B_ERROR;
	}

	// From here on this thread owns a live session; it no longer counts
	// against the handshake budget.
	atomic_add(&sConcurrentHandshakes, -1);

	TRACE_LOG("%s: authenticated, session connected\n", context->peerName);
	sSessions.Adopt(context, tls.Socket(), backendSocket);

	WebSocketStream::Relay(ws, tls, backendSocket);

	sSessions.Remove(context);
	close(backendSocket);
	ws.SendClose(1000, system_time() + 1000000);

	TRACE_LOG("%s: session ended\n", context->peerName);
	delete context;
	return B_OK;
}


// #pragma mark - main


static void
print_usage(const char* program)
{
	printf("usage: %s [-l [<address>:]<port>] [-t <address>:<port>]"
		" [-s <directory>]\n       [-c <path>]\n\n", program);
	printf("TLS + WebSocket + authentication front door for the remote"
		" desktop.\n\n");
	printf("  -l   listen address (default: all interfaces, port 10902)\n");
	printf("  -t   session port to proxy to (default: 127.0.0.1:10900)\n");
	printf("  -s   settings directory (default: the system settings"
		" directory,\n       subdirectory remote_desktop)\n");
	printf("  -c   app_server's session cookie file for the session port\n"
		"       (default: <system settings>/remote_desktop/session_cookie."
		"<port>)\n\n");
	printf("First run generates a self-signed certificate (broker.pem /"
		" broker.key),\nits pinnable SHA-256 fingerprint"
		" (broker.fingerprint) and a random\nauthentication token (token,"
		" owner-readable only) in the settings\ndirectory. Replace"
		" broker.pem/broker.key with a CA-issued pair to use a\nreal"
		" certificate.\n\n");
	printf("The session cookie is NOT generated here: app_server mints it per"
		" listener\nbefore it starts listening, and this daemon presents it to"
		" the session port\nafter a client's token was accepted. Without it the"
		" session port refuses\nevery connection, including this one.\n");
}


static bool
parse_endpoint(const char* string, bool requireAddress, in_addr_t& address,
	uint16& port)
{
	const char* colon = strchr(string, ':');
	if (colon == NULL) {
		if (requireAddress)
			return false;
		unsigned int value;
		if (sscanf(string, "%u", &value) != 1 || value == 0 || value > 65535)
			return false;
		port = (uint16)value;
		return true;
	}

	char addressString[64];
	size_t length = colon - string;
	if (length == 0 || length >= sizeof(addressString))
		return false;
	memcpy(addressString, string, length);
	addressString[length] = '\0';

	struct in_addr parsed;
	if (inet_aton(addressString, &parsed) == 0)
		return false;

	unsigned int value;
	if (sscanf(colon + 1, "%u", &value) != 1 || value == 0 || value > 65535)
		return false;

	address = parsed.s_addr;
	port = (uint16)value;
	return true;
}


int
main(int argc, char** argv)
{
	sConfiguration.listenAddress = htonl(INADDR_ANY);
	sConfiguration.listenPort = 10902;
	sConfiguration.targetAddress = htonl(INADDR_LOOPBACK);
	sConfiguration.targetPort = 10900;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
			if (!parse_endpoint(argv[++i], false,
					sConfiguration.listenAddress,
					sConfiguration.listenPort)) {
				print_usage(argv[0]);
				return 1;
			}
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			if (!parse_endpoint(argv[++i], true,
					sConfiguration.targetAddress,
					sConfiguration.targetPort)) {
				print_usage(argv[0]);
				return 1;
			}
		} else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			sConfiguration.settingsDirectory.SetTo(argv[++i]);
		} else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			sConfiguration.cookiePath.SetTo(argv[++i]);
		} else {
			print_usage(argv[0]);
			return strcmp(argv[i], "--help") == 0 ? 0 : 1;
		}
	}

	signal(SIGPIPE, SIG_IGN);

	// Line-buffered even when stdout is a file: this is a long-running
	// daemon, and a block-buffered log that only appears when the buffer
	// fills is indistinguishable from a daemon that never logged.
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (prepare_settings(sConfiguration) != B_OK)
		return 1;

	int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (listenSocket < 0) {
		TRACE_ERROR("failed to create listen socket: %s\n", strerror(errno));
		return 1;
	}

	int reuse = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = sConfiguration.listenAddress;
	address.sin_port = htons(sConfiguration.listenPort);

	if (bind(listenSocket, (struct sockaddr*)&address, sizeof(address)) != 0
		|| listen(listenSocket, 16) != 0) {
		TRACE_ERROR("failed to bind/listen on port %u: %s\n",
			sConfiguration.listenPort, strerror(errno));
		return 1;
	}

	TRACE_LOG("listening on port %u, proxying to port %u\n",
		sConfiguration.listenPort, sConfiguration.targetPort);

	while (true) {
		struct sockaddr_in peer;
		socklen_t peerLength = sizeof(peer);
		int clientSocket = accept(listenSocket, (struct sockaddr*)&peer,
			&peerLength);
		if (clientSocket < 0) {
			// Never exit the accept loop on a per-connection or transient
			// resource error: a peer that connects and immediately resets
			// (ECONNABORTED), or momentary fd/buffer exhaustion, would
			// otherwise take the whole front door down -- and every live
			// session with it -- on one unauthenticated packet. Only a
			// broken listening socket is fatal.
			switch (errno) {
				case EINTR:
					continue;

				case EBADF:
				case EINVAL:
				case ENOTSOCK:
					TRACE_ERROR("listening socket failed: %s\n",
						strerror(errno));
					return 1;

				default:
					TRACE_ERROR("accept failed, continuing: %s\n",
						strerror(errno));
					// Back off briefly so a persistent resource shortage
					// does not spin this loop at full speed.
					snooze(100 * 1000);
					continue;
			}
		}

		// Cheap gates before any TLS work: per-address backoff and a global
		// cap on concurrent handshakes.
		if (sRateLimiter.IsBlocked(peer.sin_addr.s_addr)) {
			close(clientSocket);
			continue;
		}

		if (atomic_add(&sConcurrentHandshakes, 1) + 1
				> kMaxConcurrentHandshakes) {
			atomic_add(&sConcurrentHandshakes, -1);
			close(clientSocket);
			continue;
		}

		int noDelay = 1;
		setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
			sizeof(noDelay));
		int keepAlive = 1;
		setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &keepAlive,
			sizeof(keepAlive));

		ConnectionContext* context = new(std::nothrow) ConnectionContext;
		if (context == NULL) {
			atomic_add(&sConcurrentHandshakes, -1);
			close(clientSocket);
			continue;
		}

		context->socket = clientSocket;
		context->peerAddress = peer.sin_addr.s_addr;
		if (inet_ntop(AF_INET, &peer.sin_addr, context->peerName,
				sizeof(context->peerName)) == NULL) {
			strlcpy(context->peerName, "?", sizeof(context->peerName));
		}

		thread_id thread = spawn_thread((thread_func)handle_connection,
			"broker connection", B_NORMAL_PRIORITY, context);
		if (thread < 0) {
			atomic_add(&sConcurrentHandshakes, -1);
			close(clientSocket);
			delete context;
			continue;
		}

		resume_thread(thread);
	}

	return 0;
}
