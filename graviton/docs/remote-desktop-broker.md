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

- **app_server carries no crypto at all.** Its `RemoteHWInterface` listener
  binds loopback and *refuses* a public or wildcard address. The plaintext,
  unauthenticated RP stream is therefore unreachable from outside the
  machine — note "outside the machine", not "by unauthorized peers": see
  *Known limitation* below. A deliberately awkward opt-in
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

### Known limitation: loopback is trusted

The candidate gate in app_server proves "speaks the protocol", not "is
authorized" — the first frame it requires (`RP_INIT_CONNECTION`) is six
constant bytes. So **any local process that can reach `127.0.0.1:10900` can
still take a broker-authenticated session over**, screen and keystrokes,
without TLS, token or rate limiting, and the broker never learns its client
was displaced. On the single-user test fleet this is an accepted boundary
(anything running locally already has the user's privileges), but it is a
real gap for a multi-user or untrusted-local-code host. The fix is a
per-boot shared secret between the broker and app_server (a 0600 cookie
presented as the first frame), which would also make the frame-shape
heuristic redundant; it is deliberately **not** in this change and is
tracked separately.

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
   1 denied, 2 session port unreachable. Only after success does it connect
   to the session port and forward traffic (bytes pipelined after the auth
   message are forwarded then, so clients may send `RP_INIT_CONNECTION` /
   `RP_HELLO` without waiting a round trip).

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
candidate and must send a valid first protocol frame within 10 s to take the
session over; port scans, health probes and stray connects are dropped
without the session noticing.

## Certificates: self-signed + pinning by default, real CA as a drop-in

On first run the broker generates into its settings directory
(`/boot/system/settings/remote_desktop/`):

| file | mode | content |
|---|---|---|
| `broker.pem` | 0644 | self-signed P-256 certificate, 10 years |
| `broker.key` | 0600 | private key |
| `broker.fingerprint` | 0644 | SHA-256 of the certificate (the **pin**) |
| `token` | 0600 | 64 hex chars of randomness (the shared token) |

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
- **Test instrument** — `graviton/scripts/rdcapture.py --wss --host <ip>
  --port 10902 --token-file <path> --pin <sha256>`: same transport,
  stdlib-only; prints `AUTH=OK|DENIED` and `TLS_PEER_SHA256=` for scripted
  verification. Raw-TCP mode (no `--wss`) remains for loopback testing.

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
working unchanged and needs no token (the tunnel itself is the
authentication). Use it when the broker is not running or its settings are
lost; it is no longer the recommended path.
