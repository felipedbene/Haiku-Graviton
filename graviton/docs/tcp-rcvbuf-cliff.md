# The SO_RCVBUF throughput cliff

A bulk receive on a `c7g.large` (MTU 9001, RTT 0.33 ms, 512 MiB from a Linux peer
on the same subnet) ran three to four times slower when the application asked for
a 64 KiB receive buffer than when it asked for nothing at all:

| receive buffer | measured rate |
|---|---|
| 65535 -- the built-in default, `SO_RCVBUF` never set | 4942, 4928 Mbit/s |
| **65536 -- one byte more, set with `setsockopt` before `connect`** | **1474, 1116 Mbit/s** |
| 262144 -- set with `setsockopt` before `connect` | 3953, 4949 Mbit/s |

Runs were interleaved, so drift and thermals cannot explain it.

## Root cause

**The one byte is a coincidence. The cliff is between "asked" and "did not ask".**

`TCPEndpoint::SetReceiveBufferSize()` turned off receive-window growth for the
lifetime of the socket:

```c
status_t
TCPEndpoint::SetReceiveBufferSize(size_t length)
{
	MutexLocker _(fLock);
	fReceiveQueue.SetMaxBytes(length);
	fFlags &= ~FLAG_AUTO_RECEIVE_BUFFER_SIZE;	// <-- here
	return B_OK;
}
```

That flag is the only thing that lets `_UpdateReceiveBuffer()`
(`TCPEndpoint.cpp:1385`, reached from `_AddData()` on every accepted segment)
raise `fReceiveQueue`'s maximum, and the receive queue's free space is what is
advertised to the peer (`_PrepareSendSegment()`, `fReceiveQueue.Free()` →
`SetAdvertisedWindow()`). With the flag set, the window climbs towards one
second of measured traffic, doubling at most once per interval and capped at
`UINT16_MAX << fReceiveWindowShift` = 16 MiB for the shift of 8 this stack always
uses on non-local connections. With the flag cleared, the window is pinned at
whatever the application asked for, forever.

65535 is the *default* (`net_socket.cpp`, `net_socket_private::net_socket_private`).
A default is assigned directly to `socket->receive.buffer_size` and never travels
through `setsockopt`, so it never cleared the flag. Every explicitly set value
did -- 65536, 262144, and 65535 itself would have. The boundary in the
window-shift loop lands on the same number by accident.

### The numbers agree quantitatively

A pinned window `W` cannot exceed `W * 8 / RTT`, and it does not reach that
either: Haiku only emits a window update once the reader has drained to half the
buffer (`ReadData()`, `TCPEndpoint.cpp:1054`), so the window in flight oscillates
between `W/2` and `W` and the achievable rate sits in a band from half the
ceiling to the ceiling.

| buffer | `W * 8 / 0.326 ms` | half of that | measured |
|---|---|---|---|
| 65536 | 1608 Mbit/s | 804 Mbit/s | 1116, 1474 |
| 262144 | 6432 Mbit/s | 3216 Mbit/s | 3953, 4949 |
| default, grown to 16 MiB | 412 Gbit/s | -- | 4942, 4928 (host ceiling) |

Both pinned cases land inside their band; the grown case is nowhere near its
band and is limited by the host instead, which is exactly the signature expected.
This is the same arithmetic that identified the send-buffer ceiling earlier in
`throughput-measurement.md` (1611 Mbit/s measured against 1608 predicted).

The ramp is fast enough to be invisible: `tcp_now()` ticks in milliseconds, the
smoothed RTT rounds to 0 ms on this path, and growth doubles per interval, so
65535 → 16 MiB takes on the order of 8 intervals, a few milliseconds out of a
512 MiB transfer.

### Ruled out

- **The window-shift loop** in `_PrepareSendPath()` (`TCPEndpoint.cpp:2505`,
  called `_PrepareSequenceNumbers` in the earlier note). `0xffff < 65535` is
  false and `0xffff < 65536` is true, so the loop does split on exactly the
  boundary in question -- but the next two lines force the shift to 8 for every
  non-local connection, so both cases advertise with a shift of 8 and both cap
  auto-sizing at the same 16 MiB. Reading the loop alone is what made this look
  like a one-byte bug.
- **The `tcp_setsockopt` fall-through.** It does call `SetReceiveBufferSize()`
  and then let the generic handler assign `socket->receive.buffer_size` again
  (`net_socket.cpp:1531`), and `fReceiveWindow`/`fReceiveQueue` *were* captured
  from the old value in the constructor. But `SO_RCVBUF` is set before
  `connect()`, so `_PrepareSendPath()` reads the new `buffer_size`, and
  `fReceiveWindow` catches up on the first segment through the slow path
  (`fReceiveWindow = max_c(fReceiveQueue.Free(), fReceiveWindow)`,
  `TCPEndpoint.cpp:1790`). The values do disagree transiently. It is not worth
  3× of throughput.
- **Silly-window-syndrome suppression** in `_ShouldSendSegment()`. Its threshold
  is inconsistent with `ReadData()`'s (see below), but the inconsistency makes
  window updates *more* eager, not less, and its first clause
  (`window >= 2 * MSS`) fires long before the buffer-size clause in every case
  measured here.

## The fix

```diff
 status_t
 TCPEndpoint::SetReceiveBufferSize(size_t length)
 {
 	MutexLocker _(fLock);
-	fReceiveQueue.SetMaxBytes(length);
-	fFlags &= ~FLAG_AUTO_RECEIVE_BUFFER_SIZE;
+
+	// Only a request for a smaller buffer than we already have may switch
+	// auto-sizing off: that is a request to bound memory use, which growing the
+	// window again would defeat. A request for a larger buffer merely states a
+	// minimum, and must never cost throughput -- the default size is applied by
+	// the socket layer and never passes through here, so pinning the window for
+	// every explicit SO_RCVBUF made asking for 64 KiB three times slower than
+	// asking for nothing at all, because only the untouched default was left
+	// free to grow with the bandwidth-delay product.
+	if (length < fReceiveQueue.Size())
+		fFlags &= ~FLAG_AUTO_RECEIVE_BUFFER_SIZE;
+
+	fReceiveQueue.SetMaxBytes(length);
 	return B_OK;
 }
```

Rationale, and why this shape rather than another:

- It fixes the class, not the value. The invariant it establishes is that **no
  `SO_RCVBUF` value can make a socket slower than not setting `SO_RCVBUF` at
  all**, for every size, not just 65536.
- It keeps the one legitimate reason to pin a window. An application asking for a
  *smaller* buffer is bounding memory, and re-growing behind its back would
  ignore it; that request is still honoured exactly.
- The flag is only ever cleared, never re-set, so a shrink stays sticky even if a
  later call asks for more. Auto-sizing off is a one-way door, as before.
- Memory exposure is unchanged from an ordinary socket: growth is still capped at
  16 MiB by `UINT16_MAX << fReceiveWindowShift` and still yields to
  `low_resource_state(B_KERNEL_RESOURCE_MEMORY)`.

**Deviation worth knowing about:** Linux (`SOCK_RCVBUF_LOCK`) and FreeBSD
(`SB_AUTOSIZE`) both let *any* explicit `SO_RCVBUF` pin the buffer, so this
behaviour is deliberately not theirs. An application that sets a large
`SO_RCVBUF` expecting it to be a ceiling gets a floor instead. That is the price
of the invariant above, and it is the right trade here: Haiku's default is 65535,
low enough that "set something bigger" is common advice, and the current
behaviour punishes exactly that.

Also amended in the same change, because they record the belief that produced the
confusion:

- `net_socket.cpp` -- the comment concluding that larger receive defaults
  "measured equal or worse". Where those comparisons were made by setting
  `SO_RCVBUF` they measured a pinned window, not a default. The default is left
  at 65535 regardless: with growth working it is only a starting point, worth a
  few milliseconds of ramp.
- `src/bin/nettput/nettput.cpp` -- the header comment claiming Haiku "hard-codes
  both socket buffers to 65535 and never grows them". True of the send side, not
  of the receive side.

Built for arm64 (`jam -q tcp`) clean, no new warnings. Not yet run on hardware.

## Confidence

**High on the mechanism, unmeasured on the fix.** The code path is not
ambiguous -- the flag is cleared on that line, it is the sole gate on window
growth, and growth is the only thing that can raise the advertised window above
the requested size -- and the measured rates for both pinned sizes fall inside
the band predicted by a pinned window while the unpinned case does not. What has
*not* been shown is a Haiku-side observation of the window itself, so the
possibility that something else also keys off the same call is not formally
excluded.

## Confirmation runs

The first one needs **no new build**: `nettput` is already in the canonical
image, and its `-w` flag sets `SO_SNDBUF`/`SO_RCVBUF` before `connect()`.

1. **Separate "the number 65536" from "the act of asking" (current image).**
   Against the same peer, interleaved, 512 MiB receives:

   ```
   nettput -c <peer> -r -n 512M -L default        # no -w at all
   nettput -c <peer> -r -n 512M -w 65535 -L asked-for-the-default
   ```

   Prediction: the second is slow, ~1.1-1.5 Gbit/s, even though it requests the
   exact value the first one gets by default. **If `-w 65535` comes out fast,
   this root cause is wrong** and the byte matters after all. (`-w` also sets
   `SO_SNDBUF`; on a receive run Haiku sends only acknowledgements, so that side
   cannot account for a difference.)

2. **See the window on the wire (current image, no Haiku change).** On the Linux
   peer, capturing from the SYN so the scale factor is known:

   ```
   sudo tcpdump -i <iface> -nn -S -s 96 -w /tmp/rcv.pcap 'tcp port 5301'
   tshark -r /tmp/rcv.pcap -Y 'tcp.srcport==5301 or tcp.dstport==5301' \
       -T fields -e frame.time_relative -e tcp.window_size
   ```

   Prediction: in the default run Haiku's advertised window climbs past 64 KiB
   within the first few milliseconds and heads for 16 MiB; in the `-w 65536` run
   it never exceeds 65536. This is the direct observation of the mechanism.

3. **After baking the fix.** Interleaved, 512 MiB receives:

   | run | prediction |
   |---|---|
   | no `-w` | ~4.9 Gbit/s, unchanged |
   | `-w 65535`, `-w 65536` | ~4.9 Gbit/s, i.e. the cliff is gone |
   | `-w 262144`, `-w 1M` | ~4.9 Gbit/s |
   | `-w 16384` | still pinned, roughly 200-400 Mbit/s |

   The last row is the one that proves the fix did not simply ignore the
   application: a request to shrink must still shrink.

## Related defects found while tracing, deliberately not fixed here

1. **A listening socket's `SO_RCVBUF`/`SO_SNDBUF` never reach an accepted
   socket's queues.** `socket_spawn_pending()` (`net_socket.cpp:686`) copies
   `parent->send` and `parent->receive` *after* `create_socket()` has already
   constructed the `TCPEndpoint`, whose `BufferQueue` maxima came from the
   defaults. So an accepted socket's queues are always default-sized, while
   `socket->receive.buffer_size` reports the inherited value. For receive this is
   currently benign and even lucky -- the child keeps auto-sizing, so servers
   never hit this cliff -- but `SO_SNDBUF` on a listener is silently ignored for
   the child's send queue, which has no auto-sizing to save it.
2. **Two different notions of "the buffer size" in the same file.**
   `_ShouldSendSegment()` computes its window-update threshold from
   `socket->receive.buffer_size` (`TCPEndpoint.cpp:2285`) while `ReadData()`
   uses `fReceiveQueue.Size()` (`:1054`). Once auto-sizing has run these differ
   by up to 256×. Left alone on purpose: making it consistent would send *fewer*
   window updates on a grown socket, which is a behaviour change with no
   measurement behind it.
3. **Loopback gets neither window scaling nor auto-sizing.**
   `_PrepareSendPath()` forces the shift to 8 only `!IsLocal()`, and
   `_UpdateReceiveBuffer()` returns immediately when the shift is 0, so a local
   connection is pinned to the 65535 default. Unmeasured, but on loopback the
   bandwidth-delay product is not obviously below 64 KiB either.
4. **A peer that does not offer SACK also loses window growth.**
   `_PrepareReceivePath()` clears `FLAG_AUTO_RECEIVE_BUFFER_SIZE` together with
   `FLAG_OPTION_SACK_PERMITTED` (`TCPEndpoint.cpp:1544`). Growth does not
   obviously depend on SACK; Linux always offers it, which is why this never
   showed up here.
5. **No send-side auto-sizing exists at all**, so `SetSendBufferSize()` has no
   flag to clear and this defect has no send-side twin. The trap is still there
   in a different shape: since DeBeOS raised the send default to 256 KiB, an
   application that sets `SO_SNDBUF` to 64 KiB now makes its own transmits three
   times slower, and the kernel is right to obey it. Send auto-sizing is the real
   answer and remains open.
