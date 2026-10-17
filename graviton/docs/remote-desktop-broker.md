# remote_broker — the remote desktop's internet front door

Issue #415 (follow-on to #403/#404, URP/1 M0). This document describes how the
DeBeOS remote desktop is made safe to expose on the open internet without an
SSH tunnel, what runs where, and how each client authenticates.

## Architecture: auth and TLS live in front of app_server, never inside it

```
                       public interface                loopback
 browser  ──wss──┐
 native client ──┼──▶  remote_broker :10902  ──tcp──▶  app_server RP :10900
 rdcapture ──wss─┘     (TLS + WebSocket +
                        token auth, OpenSSL)
```

- **app_server carries no crypto library at all.** Its `RemoteHWInterface`
  listener binds loopback and *refuses* a public or wildcard address, so the
  plaintext RP stream is unreachable from outside the machine. It is not
  unauthenticated, though: since #423 it requires a per-boot session cookie as
  the first frame, which needs no crypto library — 32 bytes from
  `/dev/urandom`, published 0600, compared in constant time (see *the per-boot
  session cookie* below). A deliberately awkward opt-in
  (`unsafe-bind:<a.b.c.d>:<port>` as the target, logged loudly) exists for a
  genuinely isolated private segment; it is not the supported path, because
  on a cloud instance the only NIC address is RFC 1918 with a 1:1-NAT public
  address in front of it, so "the private address" *is* the internet-facing
  interface.
- **`remote_broker`** (`src/servers/remote_broker/`) is a small separate
  daemon that terminates TLS, speaks WebSocket (RFC 6455, subprotocol
  `binary`) toward every client, verifies a shared token **before proxying a
  single byte**, and then relays the RP byte stream to the loopback session
  port. It replaces the external `websockify` dependency for the browser
  client.

### Loopback is no longer trusted: the per-boot session cookie

Until #423 the candidate gate proved "speaks the protocol", not "is
authorized" — the first frame it required (`RP_INIT_CONNECTION`) was six
constant bytes, so any local process that could reach `127.0.0.1:10900` could
take a broker-authenticated session over, screen and keystrokes, without TLS,
token or rate limiting, and the broker never learned its client was displaced.

The first frame is now **`RP_SESSION_COOKIE`**, carrying a per-boot secret:

| | |
|---|---|
| minted by | **app_server**, in `RemoteHWInterface`, before the listening socket is created |
| published in | `/boot/system/settings/remote_desktop/session_cookie.<port>`, mode 0600 |
| content | 64 hex characters (256 bits from `/dev/urandom`), newline-terminated |
| lifetime | rewritten whenever a remote listener is created, removed when it goes away |
| checked by | the candidate gate in `NetReceiver`, in constant time |
| presented by | `remote_broker` (per connection, after *its* auth succeeds), and by any direct client — `RemoteDesktop --cookie-file`, `rdcapture.py --cookie-file`, the HTML5 client's *Session cookie* field |

Reaching the port is therefore no longer enough; a local process must also be
able to **read a 0600 file owned by the user app_server runs as**. That is the
whole of the property, and it is the same one X11's
`MIT-MAGIC-COOKIE-1` rests on.

Three decisions worth stating, because each had a plausible alternative:

- **app_server mints it, not the broker and not `launch_daemon`.** The
  requirement that made the choice is ordering: whoever mints the cookie must
  do so before the listener accepts anything, or there is a window in which the
  listener must either accept unauthorized connections or refuse legitimate
  ones. The broker cannot satisfy that — although it is now a boot-time
  `launch_daemon` service (`x-vnd.Haiku-remote_broker` in `data/launch/system`),
  it can still come up before app_server's remote listener exists, because that
  listener is created lazily when a Desktop for that `TARGET_SCREEN` first
  appears; the broker copes by re-reading the cookie per session rather than
  caching one at start. `launch_daemon` could mint it instead, but only by making a
  boot-critical process responsible for a file another process validates, and a
  mismatch there (an image whose `launch_daemon` predates the change, an
  app_server started by hand) fails closed into "no remote desktop at all".
  Minting it in `RemoteHWInterface` makes "before the listener exists" a
  statement about two adjacent lines rather than about process start order, and
  leaves exactly one process owning the secret's lifetime.
- **Fail closed, and fail by not listening.** If the cookie cannot be minted or
  published, `RemoteHWInterface` fails to initialize and the port is never
  opened; `NetReceiver` refuses to `listen()` without one as a structural
  backstop. An app_server that could not find a cookie and therefore accepted
  everything would restore the gap silently — and one that listened while
  refusing everything would be a port that exists only to confuse. The
  consequence to know about: on a host where `/boot/system/settings` is not
  writable, the remote desktop is unavailable rather than unauthenticated.
- **The cookie replaces the frame-shape heuristic rather than joining it.**
  Proving authorization implies speaking the protocol, so the gate now decides
  on the cookie frame alone. Everything else about the candidate machinery is
  unchanged: a new connection is parked, has 10 s to present the cookie, and is
  dropped without the live session noticing — the property that keeps this from
  being a remote kill switch.

The gate reads **exactly** the cookie frame and no further (six header bytes,
then precisely the length they declare), the #436 discipline: a connection that
sends a valid frame followed by junk in the same segment cannot get the junk
forwarded to the parser. It also consumes the frame, so nothing above the gate
sees it and a session's stream still begins with `RP_INIT_CONNECTION`.

Still owed (not a gap in the mechanism, a gap in its reach):

- The **CrossPlatform C++/Swift clients** in the remote-client repository
  outside this tree need a cookie option of their own for direct (non-broker)
  connections; through the broker they are unaffected. That repository is out
  of scope here.
- If the broker ever runs as a **different user** from app_server, it cannot
  read a 0600 file in the system settings directory. On this fleet both run as
  the same user; a genuinely multi-user host needs a per-uid cookie or a handoff
  that does not go through the filesystem. `remote_broker -c <path>` exists as
  the escape hatch in the meantime, and the broker answers
  `RP_AUTH_RESULT` status **3** when it authenticated a client but could not
  read the cookie, so that case is diagnosable rather than silent.

Why a broker process instead of TLS inside app_server:

1. The browser is the primary client and its TLS (`wss://`) has to terminate
   in front of the RP listener anyway — in-server TLS would mean double
   encryption or two divergent transports.
2. Pre-authentication parsing (TLS records, HTTP upgrade, auth message) is
   the attack surface; in a separate process it is crash- and
   exploit-isolated from the display server that owns every keystroke.
3. The gate is structural: app_server cannot accept a public connection, so
   no code path inside it needs to re-check "is this peer allowed".

## Wire handshake (all inside TLS)

1. HTTP/1.1 WebSocket upgrade (`Sec-WebSocket-Protocol: binary` honoured).
2. The client's **first** payload bytes must form one RP-framed
   `RP_AUTHENTICATE` (=10) message: `uint32 method` (1 = shared token) and a
   length-prefixed token string. Opcode values are reserved in
   `RemoteMessage.h`; app_server never sees these messages.
3. The broker answers `RP_AUTH_RESULT` (=11): `uint32 status` — 0 success,
   1 denied, 2 session port unreachable, 3 the broker could not read
   app_server's session cookie. Only after success does it connect to the
   session port, present the session cookie as that connection's first frame,
   and forward traffic (bytes pipelined after the auth message are forwarded
   then, so clients may send `RP_INIT_CONNECTION` / `RP_HELLO` without waiting a
   round trip — they must NOT send a cookie of their own, the broker's is the
   one that counts).

Failure handling: token comparison is hash-then-compare
(SHA-256 + `CRYPTO_memcmp`, constant time, no length oracle); a failed
attempt is answered after a 2 s delay and counted per source address with
exponential backoff; the reply does not say why authentication
failed. TLS and WebSocket handshake failures are deliberately *not* counted —
the documented first-use flow produces them (a browser declining the
self-signed certificate, a plaintext probe), and counting them would back off
the user's own address before their first real connection. An unauthenticated
connection can neither observe nor displace a live
session; a newly *authenticated* client takes the session over
(newest-wins, matching the session port's reconnect model).

Flood handling, which is a separate control from the backoff above: a
handshake holds a slot from `accept()` until it either authenticates or runs
out of its 15 s budget, and there are **8 slots in total, of which one source
address may hold at most 3** (#695). The global ceiling bounds how much
concurrent TLS work the broker will do; the per-address share is what stops one
host from owning the whole ceiling — which it could do without ever being
recorded as an authentication failure, since a peer that connects and sends
nothing never reaches the token check. Refused connections are logged, rate
limited to one line a minute with a suppressed count, because the flood being
refused is the only thing that generates them in volume.

In app_server, independently of the broker: a connection to the RP listener
no longer preempts a live session merely by connecting. It is parked as a
candidate and must present the session cookie within 10 s to take the
session over; port scans, health probes, stray connects and unauthorized local
processes are dropped without the session noticing.

## Certificates: self-signed + pinning by default, real CA as a drop-in

On first run the broker generates into its settings directory
(`/boot/system/settings/remote_desktop/`):

| file | mode | content |
|---|---|---|
| `broker.pem` | 0644 | self-signed P-256 certificate, 10 years |
| `broker.key` | 0600 | private key |
| `broker.fingerprint` | 0644 | SHA-256 of the certificate (the **pin**) |
| `token` | 0600 | 64 hex chars of randomness (the shared token — the **controller** token) |

`token.viewer` lives in the same directory and is the operator's to create; it
is never generated. See *viewer and controller roles* below.

`session_cookie.<port>` lives in the same directory but is **not** the broker's
file: app_server writes it and the broker only reads it (`-c <path>` overrides
where it looks). It is the only file here that is not the broker's to rotate.

The trust model for the fleet default is **key pinning**: clients do not
chase a CA chain, they verify the certificate's SHA-256 fingerprint against
the value read out-of-band (e.g. over SSM: `cat
/boot/system/settings/remote_desktop/broker.fingerprint`). A CA-issued
certificate is a drop-in: replace `broker.pem` (loaded as a chain, so include
intermediates) and `broker.key` and restart the broker; pinning clients keep
working if re-pinned, browsers then trust it natively.

### subjectAltName: the host's own addresses, and when it is re-rolled

A pinning client never looks at the certificate's names, but a browser has
nothing else to go on — it checks the address it dialled against the
subjectAltName. The generated certificate therefore names the host name,
`127.0.0.1`, **and every IPv4 address configured on an up interface** (asked of
the network stack at generation time), so dialling a Graviton instance by its
VPC address verifies instead of failing with `certificate verify failed`.
Only IPv4: the broker's listener is a single `AF_INET` socket, so an IPv6 name
would assert an address that goes nowhere.

An address set does change — a new ENI, a different subnet, a rename — and a
stored certificate keeps the names it was born with. So every start compares
the stored subjectAltName against the host's current addresses and regenerates
when the host answers at an address the certificate does not name. Two
deliberate limits on that:

- **It regenerates only when a name is missing, never because one went stale.**
  A name for an address the host no longer has asserts nothing anyone can use
  (the private key never left the host), and dropping it would cost every pin
  for no gain.
- **A certificate that is not self-signed is never touched.** Issuer ≠ subject
  means an operator dropped in a CA-issued pair, and its names are the CA's
  business.

> **Regenerating changes the pin.** It is a new certificate, so
> `broker.fingerprint` changes and every pinning client (`RemoteDesktop --pin`,
> `rdcapture.py --pin`) fails until re-pinned from the new value. This is the
> cross-client trade-off in the open: the browser needs the names right, the
> native client needs the fingerprint stable, and they cannot both be had when
> the host's addresses change. The broker logs the refresh and the new
> fingerprint at startup.

Two consequences worth knowing. The broker is not sequenced after the network,
so a first boot where it starts before the DHCP lease generates a certificate
with no interface addresses; the next start notices and refreshes it. And an
address set changing *while* the broker runs is not picked up until it restarts.

The token is created random on first run; replace the file to set your own
(min 16 characters, whitespace-trimmed). Rotate by rewriting the file and
restarting the broker. The broker keeps only the token's hash in memory and
never logs it.

## Viewer and controller roles

Issue #551, first step. The token a client presents decides what that session
may do, for the whole of the session:

| token file | role | may send input |
|---|---|---|
| `token` | controller | yes — unchanged in every respect |
| `token.viewer` (0600, same rules, **never generated**) | viewer | no: dropped at the broker |

A viewer completes the identical handshake, gets `RP_AUTH_RESULT` status 0 and
receives the display stream like any other client. What differs is one
direction of the relay: the broker frames the client → server byte stream
itself and swallows every message in the input block — exactly the contiguous
opcode range app_server dispatches to its event stream, `RP_MOUSE_MOVED`
(220) through `RP_MODIFIERS_CHANGED` (244). Nothing else is touched:
`RP_INIT_CONNECTION`, `RP_HELLO`, `RP_RESYNC`, query replies and frame
acknowledgements all go through, so a viewer is a normal participant in the
display half of the protocol and flow control still works for it.

Why the broker and not app_server: app_server has one session and no notion
of who is driving it, while the broker already decides per connection whether
this peer may be there at all. Dropping input in front of the loopback hop
also means a viewer's keystrokes do not exist anywhere inside the display
server, which is the property worth having.

Four consequences to know about:

- **No viewer token, no change at all.** The file is never created, so an
  existing deployment keeps exactly one token and every session is a
  controller. A `token.viewer` that exists but is unreadable or too weak is an
  error at start-up rather than a silent demotion to "no viewer access".
- **Sessions are still one at a time.** The session port serves one
  connection and the broker is newest-wins, so a viewer connecting *replaces*
  a live controller session rather than joining it. Watching alongside a
  controller needs the display fan-out that #551's later steps own; what this
  step buys is that the credential you hand out cannot drive. It is strictly
  less power than the alternative available before it, which was handing over
  the controller token.
- **The viewer is not told.** `RP_AUTH_RESULT` carries status only, so a
  viewer client sees its input vanish without a message saying why; the
  broker's log is where the role is visible. Telling the client is a wire
  change and belongs with the client-side UX, not here.
- **"Watch-only" is about input events, not the display mode.**
  `RP_UPDATE_DISPLAY_MODE` is not an input event and is still forwarded, so a
  viewer can still resize the shared desktop. Hand a viewer token to someone
  you would let change your resolution, not to an arbitrary stranger; closing
  this is the first thing #551's next step should decide.

Observability is the broker's own log: `authenticated as viewer|controller`
per session, one line the first time input is dropped, and a per-session
summary when it ends (`dropped N input messages (M bytes), forwarded K other
messages`). A viewer stream whose framing cannot be followed ends the session
— input that cannot be delimited cannot be dropped — and app_server's parser
calls the same two cases a desync, so nothing legitimate is lost.

## Clients

- **Browser (HTML5, primary)** — `src/tools/html5_remote_desktop/`: connect
  straight to `wss://<host>:10902` and paste the token; no websockify, no
  tunnel. With the self-signed default the browser has to trust the
  certificate once first (open `https://<host>:10902/` in a tab, compare
  what it shows against `broker.fingerprint`, accept). The token field is
  never persisted.
- **Native client** — `RemoteDesktop <host> --wss --token-file <path>
  --pin <sha256>`: TLS with mandatory pinning (or explicit `--insecure`),
  the same WebSocket + token handshake, then the unchanged RP client on a
  local tunnel end. Built when the `openssl` build feature is enabled.
  Directly against the session port instead: `RemoteDesktop <host>
  --cookie-file <path>`.
- **Test instrument** — `graviton/scripts/rdcapture.py --wss --host <ip>
  --port 10902 --token-file <path> --pin <sha256>`: same transport,
  stdlib-only; prints `AUTH=OK|DENIED` and `TLS_PEER_SHA256=` for scripted
  verification. Raw-TCP mode (no `--wss`) remains for loopback testing, and
  needs `--cookie-file` there — this is the path used over an SSM port-forward
  to `127.0.0.1:10900`, and the one that produced the first real desktop render
  on Graviton, so it is the path a cookie change must not break. It **draws
  text** (#475): it rasterises `RP_DRAW_STRING` / `RP_DRAW_STRING_WITH_OFFSETS`
  through libfreetype and reports a text census — runs received, runs
  rasterised, glyphs, ink pixels written and ink pixels that changed the
  picture. Before that it drew a flat guessed box per run, which on an already
  painted background drew *nothing*, so a capture with every label and the clock
  missing still reported two million pixels touched and still passed. Text
  ground truth is now the default posture (exit 5 otherwise), and
  `--min-text-runs` / `--min-glyph-ink` / `--expect-text TEXT@X,Y` are the
  assertions (exit 6). What the census supports: that text drew, how many runs
  and glyphs, their origins, advances and ink box. What it does **not**: pixel
  equality with app_server — the wire carries a FontManager id, not a family
  name, and hinting and subpixel filtering differ. Compare the census between
  two clients; do not diff the PNGs and conclude the other client is wrong.
  For an image baked before the cookie gate, `--no-cookie`: that gate accepts
  only `RP_INIT_CONNECTION` or `RP_HELLO` first and drops a cookie frame.
  The A/B on a live `c7g.large` from the canonical AMI, one SSM port-forward,
  same 25 s budget, the only difference being `--no-glyphs`:

  | | text runs rasterised | visible glyph ink | pixels touched | exit |
  |---|---|---|---|---|
  | rasterising | 6 | 922 | 2,792,219 | 0 |
  | `--no-glyphs` (the old behaviour) | 0 | 0 | 2,059,398 | 6 |

  ![labels and the clock, rasterised](images/rdcapture-475-text-rasterised.png)
  ![the same desktop, text-blind](images/rdcapture-475-text-blind.png)

  Note what the second picture is *not*: it is not "boxes where the labels go".
  The estimated box only paints pixels that are still pure black, so on a
  desktop that has already painted its background it paints nothing at all —
  which is why two million touched pixels looked healthy.
- **Gate harness** — `graviton/scripts/rd423.c`: native arm64, run on the
  machine under test. Asserts that the correct cookie is served, that no
  cookie / a wrong cookie / a pre-cookie `RP_INIT_CONNECTION` / a malformed
  cookie frame are each refused *while the live session keeps being served*,
  and that a legitimate reconnect still takes over. Prints `RD423=PASS
  CHECKS=<n>`; its header documents the file-corruption mutation that makes
  arm 1 fail on purpose.

Each secret goes to exactly one hop, and passing the wrong one is refused
rather than ignored: `--token` only to the broker, `--cookie` only to the
session port. Prefer the `--token-file` / `--cookie-file` forms — a secret in
an argument is visible in `ps` and in shell history.

## QUIC front door (M5a)

Issue #766: the first, deliberately narrow, step of the QUIC/WebTransport
transport that `remote-desktop-unified-design.md` §6 *Transport* and §10 M5
carry as the design of record (retired #418). M5a puts QUIC in front of the
broker **as a second way to reach the same relay**, and changes nothing else:
one QUIC stream carries exactly the bytes a `wss` connection carries today.
It gives TLS 1.3 without TCP, and a stream that bypasses DeBeOS's own TCP
receive path (§7, #57). It does **not** give head-of-line-blocking or
datagram wins, nor connection migration (see *Stack*). Those need the
multi-stream layout below, which waits for Tier P (M3).

```
 native client ──quic (UDP)──┐
                             ├──▶  remote_broker :10902  ──tcp──▶  app_server RP :10900
 browser / native ──wss──────┘     (same token gate, same relay, same roles)
```

Implementation: `src/servers/remote_broker/QuicStream.{h,cpp}`. The token
check, session cookie, relay and roles are shared with the WebSocket front
door through `serve_session()` in `RemoteBroker.cpp`. The client is
DeBeOS-RDP's `QuicTransport` (`--url quic://HOST[:PORT]`, or a profile's
`quic` mode).

### Stack: OpenSSL's own QUIC, not a new dependency

The broker already links OpenSSL, and the image ships `openssl3` **3.5.7**,
whose QUIC implementation includes the server side (3.5 is the first release
with it). The arm64 `openssl3-3.5.7-1` package in the bake pool exports
`OSSL_QUIC_server_method`, `SSL_new_listener`, `SSL_accept_connection`,
`SSL_accept_stream` and `SSL_poll`, so the quiche/ngtcp2 port the design
anticipated is not needed. 3.5 has no 0-RTT, no RFC 9221 datagrams, and **no
connection migration** ("future versions", `ossl-guide-quic-introduction(7)`).
A client whose address changes loses the connection and reconnects, as over
`wss`. Datagrams and migration are why the multi-stream step may still need
ngtcp2.

The listener runs in OpenSSL's thread-assisted concurrency model with
reliable blocking. Every connection is served from its own thread, as on the
TCP front door; the relay uses two threads per session, because `SSL_poll()`
cannot wait on the session port's socket.

Three things in 3.5 needed handling, each commented at its site in
`QuicStream.cpp`. Two are OpenSSL defects with an upstream or recipe fix and
are fenced as **temporary** blocks that are deleted when that fix ships; the
third is a missing API and stays.

- **Haiku's negative `errno` (temporary, #790).** OpenSSL packs a system error
  as `errno & INT_MAX`. On Haiku `EAGAIN` is `B_WOULD_BLOCK` (`0x8000000b`), so
  it packs to 11, and `BIO_err_is_non_fatal()` reads the first empty
  non-blocking read as a dead network. Unpatched, no QUIC handshake ever
  completes on DeBeOS (`system library::No Error (11)`, then `quic network
  error`). Until the `openssl3` recipe carries the #790 patch, the datagram
  BIO's filter re-raises transient failures as `BIO_R_NON_FATAL`; DeBeOS-RDP's
  client carries the same block, on Haiku only. Building with
  `QUIC_ERRNO_WORKAROUND=0` (`jam -sQUIC_ERRNO_WORKAROUND=0`, or
  `-DQUIC_ERRNO_WORKAROUND=0` for the broker and for `rd766.c`) compiles it
  out; that build is #790's acceptance check 2, and on today's 3.5.7 it fails
  every positive check of `rd766`, which is what makes the check meaningful.
- **`SSL_poll()` reporting failure on readiness (temporary, #792).** 3.5.7's
  `poll_block()` (`ssl/rio/poll_immediate.c`) returns failure, with nothing on
  the error queue, when the polled object becomes ready while it enters its
  blocking section. The relay's two threads wait on one stream and make each
  other's object ready constantly, so taken as fatal it ended a healthy session
  every few hundred kilobytes (`rd766 --soak` fails with the handling removed).
  Fixed upstream in 3.5.8 (openssl/openssl#31743). Until the recipe has it, a
  failure with an empty error queue is polled again at once -- no pause and no
  retry budget; any failure that raised an error stays fatal.
- **No peer address for a QUIC connection.** 3.5 has no `SSL_get_peer_addr`.
  The same filter hands the stack one datagram per read and records its
  source. The new-pending-connection callback, which runs while the datagram
  that creates the connection is processed, stores that address on the
  connection. That datagram is the client's Initial carrying a valid Retry
  token, so the address is a validated one. A connection whose source is
  unknown is refused, not let in unaccounted.

### Wire rules

1. **UDP 10902**: the TCP listener's port number, bound to the same address
   (`-l`), IPv4 only like the TCP listener. **Off unless the broker is started
   with `-q`**, so an existing deployment opens no new port. The image's launch
   entry does not pass it.
2. **ALPN `debeos-urp/1`**, required. A handshake without it fails with
   `no_application_protocol`. The `/1` versions the *QUIC stream layout*, not
   the RP protocol, whose features are still negotiated by `RP_HELLO`. The
   multi-stream layout will be `debeos-urp/2`, chosen per connection by ALPN,
   so old clients keep working.
3. **Same certificate.** The broker serves `broker.pem` / `broker.key` on both
   listeners, so `broker.fingerprint` is the pin for both, and a client's
   known-brokers entry for `host:10902` covers both. Pinning or trust on first
   use applies exactly as on `wss`.
4. **Exactly one stream**: client-initiated bidirectional stream 0 (the ID
   alone means both, RFC 9000 §2.1). Every stream the peer opens is accepted
   into the queue; the broker takes the first, and anything else in the queue,
   before or after, closes the connection with application error 2. 3.5 cannot
   advertise a smaller stream limit than its default, so the rule is enforced
   rather than negotiated.
5. **Stream 0 carries the `wss` payload with no WebSocket framing.** The
   client's first bytes are one `RP_AUTHENTICATE`; the broker answers
   `RP_AUTH_RESULT`; after that it is the raw RP byte stream relayed to the
   session port. Every rule of *Wire handshake* above is unchanged: pipelining
   after the auth message, no client-sent cookie, the 2 s delay and backoff on
   failure, newest-wins takeover, viewer/controller filtering.
6. **No 0-RTT.** Early data could replay `RP_AUTHENTICATE` or input. 3.5
   cannot accept it, but it does advertise it in its session tickets when
   `max_early_data` is set, so the broker pins it to 0 explicitly and `rd766`
   asserts that the tickets it receives allow none.
7. **Close.** A FIN on stream 0, from either side, is the orderly close, the
   same "peer closed cleanly" the TCP and `wss` transports report. The broker
   then closes with application error 0. Error 1 means the broker gave up on
   the session: the session port went away, a write stalled past 10 s, a
   newer client took the session over, or the token was refused (the reason is
   in `RP_AUTH_RESULT` first). Error 2 means a stream-rule violation. The idle
   timeout is OpenSSL's: 30 s by default and the minimum of the two sides'.
   A server connection object only exists once the connection is established,
   so the broker cannot change it. The stack keeps a quiet session alive on
   its own; `rd766 --long` holds an idle session past it.

### What the broker's existing controls become over UDP

- **Address validation stays on.** A UDP source address can be spoofed until
  QUIC validates it, so OpenSSL's Retry round trip (its default) is left
  enabled. The per-address backoff and the handshake-slot share (#695) see
  the address recorded at validation (above). The 3× anti-amplification
  limit is the stack's own.
- **Handshake slots are shared** between the two listeners: 8 in total, 3 per
  source address, whichever transport the handshake arrives on. A QUIC slot
  is taken when the stack hands the connection over and held until
  authentication, as on TCP.
- **No migration means no migrated accounting.** The address a connection
  was validated from is its address for life.

### Clients and discovery

There is no Alt-Svc; a client that fetches the broker's token and
certificate over SSH learns about QUIC the same way, from **`broker.quic`**
in the broker's settings directory: one ALPN per line, today exactly
`debeos-urp/1`. The broker removes the file at every start and writes it
only once the QUIC listener is up, so it always describes the running
broker: a broker without `-q` has none, and never promises a port that is
not open.

DeBeOS-RDP has `--url quic://HOST[:PORT]` (default port 10902), a forced
profile mode `quic`, and in its **auto** mode (`direct`) tries the broker over
QUIC first, then over `wss`, then the SSH tunnel. The QUIC route is taken only
when the client was built with QUIC (OpenSSL 3.5+) *and* the fetched
`broker.quic` lists `debeos-urp/1`; otherwise it fails at once, visibly, and
`wss` goes ahead on the same credentials. A blocked UDP path is silent rather
than refused, so auto gives the QUIC handshake 3 s before it falls back (a
forced `quic` route waits 10 s). The browser stays on `wss` until
WebTransport (which needs HTTP/3) is its own step.

### Measured: QUIC vs wss (the gate for the multi-stream step)

Measured with a Linux client in the build VPC against a c7g.large DeBeOS
broker: one connection per transport to the same broker and the same echoing
session port, `tc netem` delay and loss on both directions of that one host
pair, 300 sequential 6-byte round trips, then a bulk echo. Two runs per row.

| path | transport | RTT p50 | RTT p99 | bulk echo |
|---|---|---|---|---|
| VPC, no impairment | quic | 0.34 ms | 0.46 ms | 1 MiB in 0.072–0.077 s |
| | wss | 0.27 ms | 0.38–0.49 ms | 1 MiB in 0.030 s |
| 40 ms RTT | quic | 40.5 ms | 40.6 ms | 256 KiB in 0.21–0.31 s |
| | wss | 40.4–40.6 ms | 40.6–40.9 ms | 256 KiB in 0.20–0.24 s |
| 40 ms RTT, 1% loss | quic | 40.5 ms | 172–251 ms | 256 KiB in 1.5–1.7 s |
| | wss | 40.5 ms | 299–325 ms | 256 KiB in 0.36–0.87 s |
| 40 ms RTT, 3% loss | quic | 40.5 ms | 254–528 ms | 256 KiB in 2.4–2.6 s |
| | wss | 40.4 ms | 330 ms | 256 KiB in 0.56–0.71 s |

What this says. On a clean path the two are the same to a fraction of a
millisecond. Under 1% loss QUIC's tail latency is somewhat better, because
loss recovery reacts faster than a TCP retransmission timeout. Under loss,
OpenSSL 3.5's QUIC moves bulk data 2–4× slower than the kernel's TCP. With a
single stream, the HoL-blocking argument for QUIC does not apply yet.

So the broker side ships opt-in (`-q`), and a client prefers QUIC only where
a broker has been opted in, falling back to `wss` -- the product decision is
to run on QUIC where it is offered, with these numbers as its known cost under
loss. The multi-stream step should not be started on the OpenSSL 3.5 stack. If it is started, the
first experiment is its datagram and congestion behaviour against this
table, on whichever stack (ngtcp2) can carry datagrams at all.

### Infrastructure

The public NLB (`ops-stack.ts`) carries QUIC only on request: with
`-c brokerEndpointQuic=true`, its listener and target group become a single
`TCP_UDP` pair on 10902. An NLB cannot hold a TCP and a UDP listener on one
port. The endpoint SG gains a `udp` rule beside every `tcp` one, and the
builder SG opens to the endpoint SG for both. Without the flag the endpoint
is exactly what #611 shipped, so an existing endpoint is not replaced on its
next deploy. The same shape, internal and VPN-only, was built for the
measurements above and carried both `wss` and `quic` to a real desktop.
Bakes for this work go through `HaikuGravitonQuicOvenPipeline`, a test-only
copy of the oven.

### Verifying it

`graviton/scripts/rd766.c` is the harness, built for the target (see its
header). It starts its own broker with `-q` on a free loopback port, with a
scratch settings directory and a mock session port it serves itself, and
asserts each wire rule from both ends:

- the `broker.quic` advertisement, written with `-q` and removed without it;
- ALPN refusal and the pin;
- the cookie arriving first, then pipelined bytes, then 30 kB both ways
  intact; session tickets that allow no 0-RTT;
- the tarpit, and per-address backoff (127.0.0.2/.3 where the loopback has
  them, else the host's own address against 127.0.0.1, as on Haiku);
- the stream rules, the viewer filter, both orderly ends, and takeover;
- with `--long`, an idle session outliving the idle timeout;
- with `--soak N`, N sessions of 300 kB each relayed intact.

It prints `RD766=PASS CHECKS=<n> ... WORKAROUND=harness:in|out,broker:in|out`.
Each check has a mutation that turns it red: accept any ALPN, record a
constant peer address, drop the viewer filter, ignore extra streams, skip the
takeover interrupt, misreport either FIN, remove the tarpit, write or keep a
wrong advertisement, allow early data in tickets, or (soak) take #792's
`SSL_poll()` report as fatal.

#790's acceptance check 2, once a patched `openssl3` is in the sysroot:

```
jam -sQUIC_ERRNO_WORKAROUND=0 remote_broker    # or -DQUIC_ERRNO_WORKAROUND=0
$CROSS -O1 -DQUIC_ERRNO_WORKAROUND=0 -o rd766 rd766.c -lssl -lcrypto -lnetwork
rd766 ./remote_broker --long --soak 30         # expect WORKAROUND=harness:out,broker:out
```

### Not in M5a: the multi-stream layout (`debeos-urp/2`)

§6's stream map has control/input and the display list on separate streams,
and Tier P tiles as datagrams. It needs four decisions this step does not
make, and the measurement above argues for making them on another stack:

- **The split.** app_server emits one byte stream. Splitting it means either
  app_server writing channels separately or the broker parsing opcodes, which
  today it does only for the viewer filter.
- **Cross-stream ordering.** `RP_STRING_WIDTH` request/reply, `RP_RESYNC`
  barriers and input interleaved with state ops all assume one order.
- **Datagram framing.** Tiles must fit ~1200-byte datagrams, so they need
  their own fragmentation and loss rules.
- **The stack.** OpenSSL 3.5 has no datagrams.

It is sequenced with M3, where there is something for the datagram half to
carry.

## Running it

```
remote_broker                          # listen *:10902 → 127.0.0.1:10900
remote_broker -q                       # ... and QUIC on UDP *:10902 too (#766)
remote_broker -l 10443 -t 127.0.0.1:10900 -s /custom/settings/dir
```

The session port default matches the fleet's `TARGET_SCREEN=10900`. The
binary ships in the image (`remote_broker@openssl` in the system servers
list); it is started on demand, not at boot — exposing the desktop remains a
deliberate act.

## SSH tunnel: demoted to the rescue path

The previous prerequisite — `ssh -L`/SSM tunnel to the loopback port, plus
websockify for the browser (`graviton/scripts/haiku-remote-desktop`) — keeps
working and needs no token (the tunnel itself is the authentication). It does
need the session cookie, because it talks to app_server directly:
`haiku-remote-desktop` reads it over the same SSH login that carries the tunnel,
uses it in its handshake probe, and prints it to paste into the client's
*Session cookie* field. Use this path when the broker is not running or its
settings are lost; it is no longer the recommended path.
