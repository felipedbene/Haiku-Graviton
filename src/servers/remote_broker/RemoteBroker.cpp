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
 * A token carries a role, decided once at authentication time and for the
 * whole of that session. The "token" file is the controller token: full
 * input, exactly what the single shared token has always been. An optional
 * second file, "token.viewer", holds a viewer token: such a session receives
 * the display stream like any other, but the input events it sends are
 * dropped here, before the loopback hop, so they never reach app_server.
 * Dropping them at the broker rather than in app_server is the whole point of
 * the role -- app_server has one session and no notion of who is driving it,
 * while the broker already decides, per connection, whether this peer may be
 * there at all. The viewer token is never generated: without that file every
 * session is a controller and this daemon behaves as it did before roles.
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
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <OS.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <new>
#include <fcntl.h>
#include <net/if.h>
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

// The input event block: exactly what app_server dispatches to its event
// stream ("code >= RP_MOUSE_MOVED && code <= RP_MODIFIERS_CHANGED" in
// RemoteHWInterface::_EventThread()). Mirrored as the same contiguous range
// rather than an enumeration of the opcodes in it, so that the two tests
// cannot come to disagree about a value between the ends.
static const uint16 kRPMouseMoved = 220;
static const uint16 kRPModifiersChanged = 244;

// RP message framing: uint16 code, uint32 total length (header included),
// payload. The body maximum is RemoteMessage::NextMessage()'s own limit --
// what that parser calls a framing desync must be a desync here too, or this
// daemon and app_server would disagree about where a message ends.
static const size_t kMessageHeaderSize = 6;
static const uint32 kMaxMessageDataSize = 64 * 1024 * 1024;

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

// How many of those slots a single source address may hold at once. The broker
// serves one session at a time, so a second concurrent handshake from the same
// client never gains it anything -- but refusing one would break flows that are
// not an attack: a browser's speculative preconnect or favicon probe arrives
// alongside the WebSocket it is about to open, and a reload can overlap a
// handshake the user is still clicking a certificate warning through. Three
// absorbs those and still leaves five slots that no single address can touch,
// so saturating the global cap takes three cooperating sources instead of one.
static const int kMaxHandshakesPerAddress = 3;

// How often a refused connection may be reported. See
// report_refused_handshake(): the condition that produces refusals in bulk is
// the very flood being refused.
static const bigtime_t kRefusalReportInterval = 60 * 1000 * 1000;

static const size_t kMaxAuthMessageSize = 4096;
static const size_t kMaxTokenLength = 1024;

// Matches RP_SESSION_COOKIE_MAX_LENGTH in RemoteMessage.h: a cookie longer than
// app_server would ever accept is rejected here rather than sent and refused.
static const size_t kMaxCookieLength = 256;
// A cookie this short is not one app_server minted (that is 64 hex characters),
// so it is refused rather than presented -- the same floor the token has.
static const size_t kMinCookieLength = 16;

// Upper bound on the interface addresses a generated certificate names (the
// host name and loopback are always named, on top of this). A server
// certificate is not an inventory of the machine, and the subjectAltName is
// what every client parses before it has decided to trust anything, so a host
// with an unusual number of configured addresses should not produce an outsized
// one. Hosts in this fleet have one or two; reaching this bound means a
// configuration worth looking at, hence the log when it happens.
static const int32 kMaxCertificateAddresses = 24;


/*!	What the token a client presented entitles it to. A controller is what
	the shared token has always been; a viewer watches and cannot touch.
*/
enum SessionRole {
	kSessionRoleController,
	kSessionRoleViewer
};


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


/*!	In-flight handshake accounting: a ceiling on how much concurrent TLS work
	the broker will do at all, and a per-source-address share of that ceiling.

	The ceiling on its own was not enough (#695). A slot is held for up to
	kHandshakeTimeout, and a peer that completes the TCP connection and then
	sends nothing is deliberately never charged an authentication failure (see
	handle_connection()), so RateLimiter never learns its address exists. One
	source holding kMaxConcurrentHandshakes sockets open across the budget
	therefore kept the front door shut for everyone, indefinitely, at well
	under one connection per second -- and the limiter that would have blocked
	it only ever sees addresses that got as far as the token check, which this
	flood never does. Bounding total work and bounding one source's share of it
	are separate jobs; both live here, under one lock, so the two numbers cannot
	disagree with each other.

	The table is sized from an invariant rather than guessed: a row is occupied
	only while that address holds at least one of the global slots, so occupied
	rows can never outnumber the slots. One row per slot therefore cannot
	overflow and needs no eviction policy -- unlike RateLimiter, whose entries
	have to outlive the connections they describe.
*/
class HandshakeLimiter {
public:
	enum Admission {
		kAdmitted,
		kRefusedTotal,
		kRefusedSource
	};

	HandshakeLimiter()
		:
		fLock("remote_broker handshake limiter"),
		fTotal(0)
	{
		memset(fEntries, 0, sizeof(fEntries));
	}

	Admission Acquire(in_addr_t address)
	{
		BAutolock lock(fLock);
		if (fTotal >= kMaxConcurrentHandshakes)
			return kRefusedTotal;

		Entry* entry = _FindEntry(address);
		if (entry != NULL && entry->count >= kMaxHandshakesPerAddress)
			return kRefusedSource;

		if (entry == NULL) {
			entry = _FreeEntry();
			if (entry == NULL) {
				// Unreachable while the table has a row per slot and fTotal is
				// under the cap, both checked above. Refusing is the safe
				// answer to a table that cannot be: the alternative is handing
				// out a slot nothing is counting.
				return kRefusedTotal;
			}

			entry->address = address;
			entry->count = 0;
		}

		entry->count++;
		fTotal++;
		return kAdmitted;
	}

	void Release(in_addr_t address)
	{
		BAutolock lock(fLock);
		Entry* entry = _FindEntry(address);
		if (entry == NULL) {
			// A release with no claim behind it would mean a slot was handed
			// back twice. Not decrementing is the conservative half of that,
			// but the counters have drifted and the caps no longer mean what
			// they say, so it is worth a line rather than silence.
			TRACE_ERROR("handshake slot released without a matching claim\n");
			return;
		}

		entry->count--;
		fTotal--;
	}

private:
	struct Entry {
		in_addr_t	address;
		int32		count;
	};

	Entry* _FindEntry(in_addr_t address)
	{
		for (size_t i = 0; i < B_COUNT_OF(fEntries); i++) {
			if (fEntries[i].count > 0 && fEntries[i].address == address)
				return &fEntries[i];
		}

		return NULL;
	}

	Entry* _FreeEntry()
	{
		for (size_t i = 0; i < B_COUNT_OF(fEntries); i++) {
			if (fEntries[i].count == 0)
				return &fEntries[i];
		}

		return NULL;
	}

	BLocker	fLock;
	int32	fTotal;
	Entry	fEntries[kMaxConcurrentHandshakes];
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
	// The optional viewer token, read from token.viewer if that file exists
	// and never generated: a second credential nobody asked for should not
	// come into existence on upgrade, and its absence is precisely the
	// behaviour of a broker that has never heard of roles.
	bool		hasViewerToken;
	uint8		viewerTokenDigest[SHA256_DIGEST_LENGTH];
	// The file app_server publishes the session cookie for the port this
	// daemon proxies to. Read per connection, never cached: see the header
	// comment.
	BPath		cookiePath;
};

static BrokerConfiguration sConfiguration;
static TLSContext sTLSContext;
static RateLimiter sRateLimiter;
static HandshakeLimiter sHandshakes;
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


/*!	Collects the names a generated certificate must assert, each already in
	OpenSSL's subjectAltName syntax ("DNS:..." / "IP:...").

	A client dials this host by whatever address routes to it -- a VPC or LAN
	address, typically, not the host name and not loopback -- and verifies that
	name against the certificate. So every address the host actually answers at
	has to be in there, which means asking the network stack rather than
	guessing: BNetworkRoster over the interfaces, BNetworkInterface over each
	interface's addresses, the same iteration ifconfig and the Network
	preferences use.

	Deliberate restrictions:
	- Only IFF_UP interfaces. Not IFF_LINK as well, although the stock
	  "is there a usable interface" test adds it: an interface that is
	  configured but momentarily has no carrier still owns its address and will
	  serve on it when the carrier returns, and making the certificate depend on
	  carrier state would re-roll it on every cable event (see
	  certificate_is_current() for why re-rolling is expensive).
	- Only AF_INET. The listener is a single AF_INET socket, so this host cannot
	  be reached over IPv6 at all and asserting an IPv6 name would be a claim
	  about an address that goes nowhere. (It would also be unverifiable where
	  it matters most: a link-local address needs a zone index, which the
	  iPAddress form of a subjectAltName cannot carry.)
	- The addresses are sorted before kMaxCertificateAddresses is applied, so
	  that a given set always yields the same list regardless of the order the
	  stack enumerated them in. Without that, a host over the bound could
	  select a different subset on each start and certificate_is_current()
	  would re-roll the certificate forever.

	The list is widened from "host name plus loopback" to "every address this
	host answers at", which widens what the certificate asserts -- but every one
	of those addresses *is* this host, the key stays in a 0600 file here, and
	the default certificate is self-signed, so it asserts nothing any client
	trusts without being told to. No wildcard and no 0.0.0.0 catch-all: those
	would assert names that are not this host.
*/
static void
collect_alt_names(BStringList& altNames)
{
	char hostname[256] = "debeos-remote-desktop";
	gethostname(hostname, sizeof(hostname) - 1);

	// The entries are handed to OpenSSL as one comma-separated,
	// colon-delimited string, so a host name holding either character would
	// not widen the subjectAltName but corrupt it -- the whole extension is
	// then rejected and the certificate ends up without one, which is the
	// failure this function exists to prevent. Anything outside a host name's
	// alphabet costs the DNS entry rather than the extension.
	bool nameUsable = hostname[0] != '\0';
	for (const char* c = hostname; nameUsable && *c != '\0'; c++) {
		nameUsable = isalnum((unsigned char)*c) != 0 || *c == '-' || *c == '.'
			|| *c == '_';
	}

	if (nameUsable) {
		BString entry;
		altNames.Add(entry.SetToFormat("DNS:%s", hostname));
	} else {
		TRACE_ERROR("host name \"%s\" is not usable as a certificate name;"
			" the certificate will assert addresses only\n", hostname);
	}

	// Added unconditionally rather than left to the loopback interface showing
	// up in the enumeration below: the SSH-tunnel rescue path dials 127.0.0.1
	// and must keep working even if that enumeration comes up empty. It is also
	// why these two are added before the cap is applied and the interface
	// addresses after it -- the cap must never be able to evict loopback.
	altNames.Add("IP:127.0.0.1");

	BStringList addresses;
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_UP) == 0)
			continue;

		int32 count = interface.CountAddresses();
		for (int32 i = 0; i < count; i++) {
			BNetworkInterfaceAddress interfaceAddress;
			if (interface.GetAddressAt(i, interfaceAddress) != B_OK)
				break;

			const BNetworkAddress& address = interfaceAddress.Address();
			if (address.Family() != AF_INET || address.IsEmpty()
				|| address.IsWildcard()) {
				continue;
			}

			BString entry;
			entry.SetToFormat("IP:%s", address.ToString(false).String());
			if (entry.Length() > 3 && !altNames.HasString(entry)
				&& !addresses.HasString(entry)) {
				addresses.Add(entry);
			}
		}
	}

	addresses.Sort();

	if (addresses.CountStrings() > kMaxCertificateAddresses) {
		TRACE_ERROR("%" B_PRId32 " local addresses for the certificate, naming"
			" the first %" B_PRId32 "; clients dialling one of the rest will"
			" fail name verification\n", addresses.CountStrings(),
			kMaxCertificateAddresses);
		addresses.Remove(kMaxCertificateAddresses,
			addresses.CountStrings() - kMaxCertificateAddresses);
	}

	altNames.Add(addresses);
}


/*!	Renders one subjectAltName entry of a stored certificate in the same
	syntax collect_alt_names() produces, so the two can be compared. Entry
	types the broker never writes come back empty.
*/
static BString
alt_name_to_string(const GENERAL_NAME* name)
{
	BString entry;

	switch (name->type) {
		case GEN_DNS:
		{
			int length = ASN1_STRING_length(name->d.dNSName);
			const unsigned char* data
				= ASN1_STRING_get0_data(name->d.dNSName);
			if (data != NULL && length > 0)
				entry.SetToFormat("DNS:%.*s", length, (const char*)data);
			break;
		}

		case GEN_IPADD:
		{
			int length = ASN1_STRING_length(name->d.iPAddress);
			const unsigned char* data
				= ASN1_STRING_get0_data(name->d.iPAddress);
			char buffer[INET6_ADDRSTRLEN];
			int family = length == 4 ? AF_INET
				: (length == 16 ? AF_INET6 : AF_UNSPEC);
			if (data != NULL && family != AF_UNSPEC
				&& inet_ntop(family, data, buffer, sizeof(buffer)) != NULL) {
				entry.SetToFormat("IP:%s", buffer);
			}
			break;
		}

		default:
			break;
	}

	return entry;
}


/*!	Whether the stored certificate still asserts every name in \a altNames,
	i.e. whether it is usable for every address this host now answers at.

	Regenerating is not free: it changes the certificate, so it changes the
	SHA-256 fingerprint, and the fingerprint is the pin the native client and
	the test instrument verify. Every pinning client therefore breaks until it
	is re-pinned from the new broker.fingerprint. The browser client cannot pin
	and has the opposite problem -- it checks the name, so a certificate missing
	the dialled address breaks it outright. The policy that costs the least on
	both sides is to re-roll only when the certificate is genuinely unusable:

	- A name the host answers at that the certificate does not carry means a
	  client dialling it fails verification: regenerate.
	- A name the certificate carries that the host no longer has is harmless and
	  is left alone. It asserts an address nothing answers at, and since the
	  private key never left this host nobody else can present the certificate
	  for it. Dropping it would break every pin for no gain. The entry does
	  disappear the next time something else forces a regeneration.
	- A certificate that is not self-signed is never ours to rotate: the
	  operator dropped in a CA-issued pair, and a CA-issued name set is the
	  CA's business. Reported current whatever it contains.
	- A certificate that cannot be read or parsed is likewise reported current.
	  Overwriting a file we failed to understand is worse than letting
	  InitServer() refuse it with an error the operator can act on.
*/
static bool
certificate_is_current(const char* certificatePath,
	const BStringList& altNames)
{
	FILE* file = fopen(certificatePath, "r");
	if (file == NULL)
		return true;

	X509* certificate = PEM_read_X509(file, NULL, NULL, NULL);
	fclose(file);
	if (certificate == NULL)
		return true;

	bool current = true;
	if (X509_NAME_cmp(X509_get_subject_name(certificate),
			X509_get_issuer_name(certificate)) == 0) {
		BStringList stored;
		GENERAL_NAMES* names = (GENERAL_NAMES*)X509_get_ext_d2i(certificate,
			NID_subject_alt_name, NULL, NULL);
		if (names != NULL) {
			for (int i = 0; i < sk_GENERAL_NAME_num(names); i++) {
				BString entry
					= alt_name_to_string(sk_GENERAL_NAME_value(names, i));
				if (entry.Length() > 0)
					stored.Add(entry);
			}
			GENERAL_NAMES_free(names);
		}

		for (int32 i = 0; i < altNames.CountStrings(); i++) {
			// Names are compared case-insensitively: a DNS entry is
			// case-insensitive by definition, and nothing is gained by
			// re-rolling the certificate over the spelling of a host name.
			if (!stored.HasString(altNames.StringAt(i), true)) {
				current = false;
				break;
			}
		}
	}

	X509_free(certificate);
	return current;
}


/*!	Generates the self-signed server certificate: a P-256 key and a ten year
	certificate, both PEM, asserting \a altNames. A real CA-issued certificate
	can later be dropped into the same files (the certificate file is loaded as
	a chain).
*/
static status_t
generate_certificate(const char* certificatePath, const char* keyPath,
	const BString& altNames)
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
		// (CN-only has not been accepted for years), and it has to name every
		// address this host answers at or a client dialling one of them fails
		// name verification -- see collect_alt_names(). Built as a list of
		// whole entries joined by the caller, not formatted into a fixed
		// buffer: a subjectAltName truncated mid-entry is worse than a short
		// one, because it asserts a name that is a prefix of a real address.
		X509_EXTENSION* extension = X509V3_EXT_conf_nid(NULL, NULL,
			NID_subject_alt_name, altNames.String());
		if (extension != NULL) {
			X509_add_ext(certificate, extension, -1);
			X509_EXTENSION_free(extension);
		} else {
			TRACE_ERROR("could not build subjectAltName from \"%s\"\n",
				altNames.String());
			goto out;
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


/*!	Reads a token file and leaves only its SHA-256 in \a digest; comparison
	against that cannot leak the token through timing (both sides are hashed,
	then compared in constant time).
*/
static status_t
read_token(const char* tokenPath, uint8* digest)
{
	int fd = open(tokenPath, O_RDONLY);
	if (fd < 0)
		return errno;

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


/*!	Loads the controller token -- the shared token this daemon has always had
	-- generating a random one on first run.
*/
static status_t
ensure_token(const char* tokenPath, uint8* digest)
{
	int fd = open(tokenPath, O_RDONLY);
	if (fd >= 0)
		close(fd);
	else {
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
	}

	return read_token(tokenPath, digest);
}


/*!	Loads the viewer token if the operator created one. Absent is the normal
	case and means every session is a controller; a file that exists but is
	unusable is an error, because an operator who wrote it expects it to be in
	force and a silent demotion to "no viewer access at all" would be found out
	only by the person it was shared with.
*/
static status_t
load_viewer_token(const char* tokenPath, BrokerConfiguration& configuration)
{
	status_t result = read_token(tokenPath, configuration.viewerTokenDigest);
	if (result == B_OK) {
		configuration.hasViewerToken = true;
		TRACE_LOG("viewer token loaded from %s: a client presenting it gets a"
			" watch-only session\n", tokenPath);
		return B_OK;
	}

	if (result == ENOENT) {
		TRACE_LOG("no viewer token in %s; every session is a controller\n",
			tokenPath);
		return B_OK;
	}

	TRACE_ERROR("viewer token in %s is unusable: %s\n", tokenPath,
		strerror(result));
	return result;
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
	BPath viewerTokenPath(directory.Path(), "token.viewer");

	BStringList altNames;
	collect_alt_names(altNames);
	BString altNameList = altNames.Join(",");

	struct stat st;
	if (stat(certificatePath.Path(), &st) != 0
		|| stat(keyPath.Path(), &st) != 0) {
		TRACE_LOG("generating self-signed certificate in %s for %s\n",
			directory.Path(), altNameList.String());
		status_t result = generate_certificate(certificatePath.Path(),
			keyPath.Path(), altNameList);
		if (result != B_OK) {
			TRACE_ERROR("certificate generation failed\n");
			return result;
		}
	} else if (!certificate_is_current(certificatePath.Path(), altNames)) {
		// This host answers at an address the stored certificate does not
		// assert -- a new ENI, a different subnet, a renamed host -- so a
		// client dialling that address fails name verification. Re-roll it.
		//
		// Via temporaries and renames rather than over the files in place:
		// generate_certificate() removes what it was writing when it fails,
		// and a failed re-roll must leave the working pair behind rather than
		// take the whole daemon down with it.
		BPath newCertificatePath(directory.Path(), "broker.pem.new");
		BPath newKeyPath(directory.Path(), "broker.key.new");

		status_t result = generate_certificate(newCertificatePath.Path(),
			newKeyPath.Path(), altNameList);
		if (result == B_OK
			&& rename(newKeyPath.Path(), keyPath.Path()) == 0
			&& rename(newCertificatePath.Path(), certificatePath.Path())
				== 0) {
			// Loud: this changed the certificate, so it changed the pin.
			TRACE_LOG("certificate refreshed for %s -- the fingerprint below"
				" is a new one, pinning clients must re-pin from"
				" broker.fingerprint\n", altNameList.String());
		} else {
			unlink(newCertificatePath.Path());
			unlink(newKeyPath.Path());
			TRACE_ERROR("could not refresh the certificate, keeping the"
				" stored one: clients dialling an address it does not assert"
				" will fail name verification\n");
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

	result = load_viewer_token(viewerTokenPath.Path(), configuration);
	if (result != B_OK)
		return result;

	return sTLSContext.InitServer(certificatePath.Path(), keyPath.Path());
}


// #pragma mark - authentication


/*!	Reads and verifies the RP_AUTHENTICATE message that must be the client's
	very first payload. On success \a role says what the token presented
	entitles this session to, and any bytes the client pipelined after the
	message are left in \a buffer (\a extraOffset/\a extraLength) to be
	forwarded once the session is connected.
*/
static status_t
authenticate_client(WebSocketStream& ws, bigtime_t deadline, uint8* buffer,
	size_t bufferSize, size_t& extraOffset, size_t& extraLength,
	SessionRole& role)
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
	// the correct token. Both roles' digests are compared unconditionally --
	// not "controller, else viewer" -- so that which token was presented
	// cannot be read off how much work this took.
	uint8 digest[SHA256_DIGEST_LENGTH];
	SHA256(buffer + 14, tokenLength, digest);

	bool controllerMatch = CRYPTO_memcmp(digest, sConfiguration.tokenDigest,
		SHA256_DIGEST_LENGTH) == 0;
	bool viewerMatch = sConfiguration.hasViewerToken
		&& CRYPTO_memcmp(digest, sConfiguration.viewerTokenDigest,
			SHA256_DIGEST_LENGTH) == 0;

	if (!controllerMatch && !viewerMatch)
		return B_PERMISSION_DENIED;

	// Controller wins if the two files happen to hold the same token: the
	// operator already had that one secret and it already meant full control,
	// so a copy of it in token.viewer must not quietly take control away.
	role = controllerMatch ? kSessionRoleController : kSessionRoleViewer;

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
	if (cookieLength > kMaxCookieLength)
		return B_BAD_VALUE;

	// Explicitly little-endian, like all remote protocol framing.
	uint8 frame[6 + 8 + kMaxCookieLength];
	uint32 length = (uint32)(6 + 8 + cookieLength);

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


// #pragma mark - viewer input filter


/*!	Drops a viewer session's input events on their way to the session port.

	Everything else the client sends is forwarded untouched, because a viewer
	is a full participant in the display half of the protocol: it announces
	itself (RP_INIT_CONNECTION, RP_HELLO), may ask for a resync, answers the
	server's queries and paces frames. Only the input block is swallowed.

	The filter sees a byte stream, so it has to do the protocol's framing
	itself -- a WebSocket read boundary falls anywhere, including inside a
	message header. State therefore spans calls: a partial header is stashed
	until it completes, and the body of a message already judged is counted
	down across reads. Nothing is buffered beyond those few header bytes; a
	forwarded body is passed through as it arrives, so this adds no latency to
	the traffic it does allow.

	A stream whose framing stops making sense ends the session instead of
	being passed through: a viewer whose messages cannot be delimited is a
	viewer whose keystrokes cannot be stopped, and that is the one outcome
	this class exists to prevent. app_server's parser treats the same two
	cases as a desync, so nothing legitimate is lost.
*/
class ViewerInputFilter : public RelayFilter {
public:
	ViewerInputFilter()
		:
		fHeaderUsed(0),
		fForwardRemaining(0),
		fDropRemaining(0),
		fDroppedMessages(0),
		fDroppedBytes(0),
		fForwardedMessages(0)
	{
	}

	virtual	status_t		Forward(int socket, const uint8* buffer,
								size_t size) override;

			uint64			DroppedMessages() const
								{ return fDroppedMessages; }
			uint64			DroppedBytes() const { return fDroppedBytes; }
			uint64			ForwardedMessages() const
								{ return fForwardedMessages; }

private:
			uint8			fHeader[kMessageHeaderSize];
			size_t			fHeaderUsed;
			uint32			fForwardRemaining;
			uint32			fDropRemaining;

			uint64			fDroppedMessages;
			uint64			fDroppedBytes;
			uint64			fForwardedMessages;
};


status_t
ViewerInputFilter::Forward(int socket, const uint8* buffer, size_t size)
{
	while (size > 0) {
		if (fDropRemaining > 0) {
			size_t chunk = size < fDropRemaining ? size : fDropRemaining;
			fDropRemaining -= chunk;
			buffer += chunk;
			size -= chunk;
			continue;
		}

		if (fForwardRemaining > 0) {
			size_t chunk = size < fForwardRemaining ? size : fForwardRemaining;
			status_t result = write_fully(socket, buffer, chunk);
			if (result != B_OK)
				return result;

			fForwardRemaining -= chunk;
			buffer += chunk;
			size -= chunk;
			continue;
		}

		// At a message boundary: collect the header, which may be split
		// across reads, before anything about this message is decided.
		size_t missing = kMessageHeaderSize - fHeaderUsed;
		size_t chunk = size < missing ? size : missing;
		memcpy(fHeader + fHeaderUsed, buffer, chunk);
		fHeaderUsed += chunk;
		buffer += chunk;
		size -= chunk;
		if (fHeaderUsed < kMessageHeaderSize)
			break;

		fHeaderUsed = 0;

		// Explicitly little-endian, like all remote protocol framing.
		uint16 code = (uint16)fHeader[0] | ((uint16)fHeader[1] << 8);
		uint32 length = (uint32)fHeader[2] | ((uint32)fHeader[3] << 8)
			| ((uint32)fHeader[4] << 16) | ((uint32)fHeader[5] << 24);

		if (length < kMessageHeaderSize
			|| length - kMessageHeaderSize > kMaxMessageDataSize) {
			TRACE_ERROR("viewer session: message code %" B_PRIu16 " claims %"
				B_PRIu32 " bytes; framing lost, ending the session\n", code,
				length);
			return B_BAD_DATA;
		}

		if (code >= kRPMouseMoved && code <= kRPModifiersChanged) {
			fDropRemaining = length - (uint32)kMessageHeaderSize;
			fDroppedMessages++;
			fDroppedBytes += length;

			// Logged once: the point is that the role took effect, and a line
			// per keystroke would be a log amplifier a viewer controls.
			if (fDroppedMessages == 1) {
				TRACE_LOG("viewer session: dropping client input events, "
					"first was code %" B_PRIu16 "\n", code);
			}

			continue;
		}

		status_t result = write_fully(socket, fHeader, kMessageHeaderSize);
		if (result != B_OK)
			return result;

		fForwardedMessages++;
		fForwardRemaining = length - (uint32)kMessageHeaderSize;
	}

	return B_OK;
}


// #pragma mark - per connection handler


struct ConnectionContext {
	int			socket;
	in_addr_t	peerAddress;
	char		peerName[INET_ADDRSTRLEN];
	// Whether this connection still holds its handshake slot. Two paths give
	// the slot up before they are finished with the connection, so releasing
	// has to be harmless the second time; only this connection's own thread
	// ever touches the flag, so it needs no atomics.
	bool		holdsHandshakeSlot;
};


/*!	Gives this connection's handshake slot back, once.

	The slot is accounted per source address as well as globally now (#695),
	and the release used to be open-coded at ten separate sites. Adding a
	second counter to all ten is how such a fix leaks: miss one path and that
	address is permanently down a slot, with nothing in the log to say so. So
	the release lives here, and every exit from handle_connection() falls
	through connection_thread(), which calls it -- a forgotten path cannot
	leak, and the two paths that deliberately release early simply get here
	first.
*/
static void
release_handshake_slot(ConnectionContext* context)
{
	if (!context->holdsHandshakeSlot)
		return;

	context->holdsHandshakeSlot = false;
	sHandshakes.Release(context->peerAddress);
}


static status_t
handle_connection(ConnectionContext* context)
{
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
		return result;
	}

	WebSocketStream ws(tls);
	result = ws.AcceptHandshake(deadline);
	if (result != B_OK) {
		TRACE_LOG("%s: WebSocket handshake failed\n", context->peerName);
		return result;
	}

	uint8 authBuffer[kMaxAuthMessageSize];
	size_t extraOffset = 0;
	size_t extraLength = 0;
	SessionRole role = kSessionRoleController;
	result = authenticate_client(ws, deadline, authBuffer,
		sizeof(authBuffer), extraOffset, extraLength, role);

	if (result != B_OK) {
		OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
		TRACE_LOG("%s: authentication failed\n", context->peerName);
		sRateLimiter.RecordFailure(context->peerAddress);

		// Give the handshake slot up BEFORE the tarpit delay: this thread
		// only has a fixed 10 byte denial left to write, and holding one of
		// the few concurrent-handshake slots through a deliberate delay
		// would spend the broker's capacity rather than the attacker's.
		release_handshake_slot(context);

		snooze(kAuthFailureDelay);
		send_auth_result(ws, kAuthResultDenied, system_time() + 1000000);
		ws.SendClose(1008, system_time() + 1000000);
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
		OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
		return cookieResult;
	}

	int backendSocket = connect_backend();
	if (backendSocket < 0) {
		TRACE_ERROR("%s: session port unreachable\n", context->peerName);
		send_auth_result(ws, kAuthResultNoSession, system_time() + 1000000);
		ws.SendClose(1011, system_time() + 1000000);
		OPENSSL_cleanse(cookie, sizeof(cookie));
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

	// A viewer's input is dropped here, in front of the loopback hop. The
	// filter has to see this session's stream from its very first byte --
	// including whatever the client pipelined behind the auth message, which
	// is already part of the message framing it must stay aligned with.
	ViewerInputFilter viewerFilter;
	RelayFilter* clientFilter = role == kSessionRoleViewer
		? &viewerFilter : NULL;

	if (forwardResult == B_OK && extraLength > 0) {
		forwardResult = clientFilter != NULL
			? clientFilter->Forward(backendSocket, authBuffer + extraOffset,
				extraLength)
			: write_fully(backendSocket, authBuffer + extraOffset,
				extraLength);
	}
	// The token (and everything else read pre-session) is no longer needed.
	OPENSSL_cleanse(authBuffer, sizeof(authBuffer));
	if (forwardResult != B_OK) {
		close(backendSocket);
		return B_ERROR;
	}

	// From here on this thread owns a live session; it no longer counts
	// against the handshake budget.
	release_handshake_slot(context);

	TRACE_LOG("%s: authenticated as %s, session connected\n",
		context->peerName,
		role == kSessionRoleViewer ? "viewer" : "controller");
	sSessions.Adopt(context, tls.Socket(), backendSocket);

	WebSocketStream::Relay(ws, tls, backendSocket, clientFilter);

	sSessions.Remove(context);
	close(backendSocket);
	ws.SendClose(1000, system_time() + 1000000);

	if (clientFilter != NULL) {
		// What the role actually did, in numbers: the counters are how a test
		// (or an operator) sees that input was stopped here and that the rest
		// of the client's traffic still went through.
		TRACE_LOG("%s: viewer session ended, dropped %" B_PRIu64 " input "
			"messages (%" B_PRIu64 " bytes), forwarded %" B_PRIu64 " other "
			"messages\n", context->peerName, viewerFilter.DroppedMessages(),
			viewerFilter.DroppedBytes(), viewerFilter.ForwardedMessages());
	}

	TRACE_LOG("%s: session ended\n", context->peerName);
	return B_OK;
}


/*!	Thread entry. The handshake slot and the context are given back here, at
	the one point every path out of handle_connection() reaches, rather than
	being repeated at each of its early returns.
*/
static status_t
connection_thread(void* data)
{
	ConnectionContext* context = (ConnectionContext*)data;
	status_t result = handle_connection(context);

	release_handshake_slot(context);
	delete context;
	return result;
}


// #pragma mark - main


/*!	Reports a connection refused by the handshake caps, at most once per
	kRefusalReportInterval.

	A refusal used to be a bare close() with nothing written anywhere, which is
	how a flood could hold the door shut and leave the operator looking at a
	broker that simply "does not answer" (#695). It is reported now -- but a
	flood is also the only thing that produces refusals in any volume, so a
	line per refused connection would turn a cheap attack into a log-filling
	one. The first refusal after a quiet interval is reported at once, and the
	next line carries the count that was suppressed behind it, so the rate is
	bounded while the magnitude is not lost. Only the accept loop refuses
	connections, so this state is touched by one thread and needs no lock.
*/
static void
report_refused_handshake(const char* peerName,
	HandshakeLimiter::Admission reason)
{
	static bigtime_t sLastReport = 0;
	static int32 sSuppressed = 0;

	bigtime_t now = system_time();
	if (sLastReport != 0 && now - sLastReport < kRefusalReportInterval) {
		sSuppressed++;
		return;
	}

	const char* limit = reason == HandshakeLimiter::kRefusedSource
		? "this address holds every handshake slot it may at once"
		: "all handshake slots are in use";
	if (sSuppressed > 0) {
		TRACE_ERROR("%s: connection refused, %s (%" B_PRId32 " further"
			" refusals since the previous report were not logged)\n", peerName,
			limit, sSuppressed);
	} else
		TRACE_ERROR("%s: connection refused, %s\n", peerName, limit);

	sSuppressed = 0;
	sLastReport = now;
}


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
	printf("The generated certificate names this host's own interface"
		" addresses, so a\nclient may dial any of them. A later start that"
		" finds an address the stored\ncertificate does not name regenerates"
		" it -- which changes the fingerprint,\nso pinning clients must"
		" re-pin. A CA-issued pair is never regenerated.\n\n");
	printf("The token file is the controller token: a client presenting it"
		" drives the\ndesktop. Create a second token in token.viewer (same"
		" rules, 0600) to hand\nout watch-only access: such a session receives"
		" the display stream, but every\ninput event it sends is dropped here"
		" and never reaches app_server. It is\nnever generated -- without that"
		" file every session is a controller.\n\n");
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

		// Cheap gates before any TLS work: per-address failure backoff, then
		// admission against the global and per-address handshake caps.
		if (sRateLimiter.IsBlocked(peer.sin_addr.s_addr)) {
			close(clientSocket);
			continue;
		}

		// TCP_NODELAY: this socket carries interactive traffic (input echo,
		// cursor, small drawing updates) to a client across a WAN, and Haiku's
		// TCP now has Nagle (TCPEndpoint::_ShouldSendSegment(), #639), which
		// would hold every short tail for up to a round trip while earlier
		// data is unacknowledged. NODELAY used to cost a sub-MSS runt on every
		// window-limited ACK -- the reason this socket went without it (#630)
		// -- but sender SWS avoidance now applies to NODELAY sockets too
		// (#640), and a select()-driven writer is no longer let in for every
		// freed sliver (#639): 2.0% sub-MSS segments behind a 400 ms tbf
		// queue at 79 ms RTT, with or without the option.
		int noDelay = 1;
		setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
			sizeof(noDelay));

		int keepAlive = 1;
		setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &keepAlive,
			sizeof(keepAlive));

		ConnectionContext* context = new(std::nothrow) ConnectionContext;
		if (context == NULL) {
			close(clientSocket);
			continue;
		}

		context->socket = clientSocket;
		context->peerAddress = peer.sin_addr.s_addr;
		context->holdsHandshakeSlot = false;
		if (inet_ntop(AF_INET, &peer.sin_addr, context->peerName,
				sizeof(context->peerName)) == NULL) {
			strlcpy(context->peerName, "?", sizeof(context->peerName));
		}

		// Claimed only once the context exists, so that the slot has an owner
		// from the moment it is taken and is released through the one path
		// that owner has (release_handshake_slot()).
		HandshakeLimiter::Admission admission
			= sHandshakes.Acquire(context->peerAddress);
		if (admission != HandshakeLimiter::kAdmitted) {
			// Not recorded as an authentication failure: being refused is not
			// evidence about this peer, and an address shared by a NAT with a
			// flooding host must not inherit its backoff.
			report_refused_handshake(context->peerName, admission);
			close(clientSocket);
			delete context;
			continue;
		}

		context->holdsHandshakeSlot = true;

		thread_id thread = spawn_thread((thread_func)connection_thread,
			"broker connection", B_NORMAL_PRIORITY, context);
		if (thread < 0) {
			release_handshake_slot(context);
			close(clientSocket);
			delete context;
			continue;
		}

		resume_thread(thread);
	}

	return 0;
}
