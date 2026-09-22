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
  ones. The broker cannot satisfy that — it is started *on demand*, long after
  app_server, and the listener is created lazily when a Desktop for that
  `TARGET_SCREEN` first appears. `launch_daemon` could, but only by making a
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
exponential backoff (TLS/WebSocket handshake failures count too, so plaintext
probes back off the same way); the reply does not say why authentication
failed. An unauthenticated connection can neither observe nor displace a live
session; a newly *authenticated* client takes the session over
(newest-wins, matching the session port's reconnect model).

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
| `token` | 0600 | 64 hex chars of randomness (the shared token) |

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

The token is created random on first run; replace the file to set your own
(min 16 characters, whitespace-trimmed). Rotate by rewriting the file and
restarting the broker. The broker keeps only the token's hash in memory and
never logs it.

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
  on Graviton, so it is the path a cookie change must not break.
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

## Running it

```
remote_broker                          # listen *:10902 → 127.0.0.1:10900
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
