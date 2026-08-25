# Deskbar alive in `ps` and drawing nothing: the remote send buffer had no reader

**Status:** root cause measured on hardware; fixed in
`src/servers/app/drawing/interface/remote/`.

## Symptom

On the headless (remote-display) image, `Deskbar` appears in `ps` from boot to
shutdown and paints nothing at all. On a connected client the top-right corner
is a black rectangle with a 1 px border. Tracker, Terminal and everything else
look normal. `quit application/x-vnd.Be-TSKB` makes `launch_daemon` relaunch it
and the replacement draws correctly and immediately.

`ps` is worthless as a check here — a wedged Deskbar and a healthy one look
identical in it. That is what made this expensive to find.

## What it is not

The image's own `remote-desktop.sh` used to assert that the cause was a missing
keymap: that `input_server` must be up before anything builds a menu, or
`BKeymap::GetModifiedCharacters()` dereferences nothing and the application dies
silently. **That explanation is wrong for Deskbar.** Two independent
falsifications:

- Deskbar contains no keymap API use whatsoever — no `BKeymap`, no
  `get_key_map()`, no `SetToCurrent()`. Its only keymap contact is indirect,
  through `BMenu`, and that path is bounded (`_control_input_server_()` passes
  explicit 5 s delivery and reply timeouts).
- The wedge was reproduced on a boot where `input_server` started **first**
  (team 113, before app_server at 126 and Deskbar at 142) and was measurably
  healthy: event loop live, every one of its ports drained to zero.

The keymap claim *is* still true of Terminal, which calls
`BKeymap::GetModifiedCharacters()` without checking `SetToCurrent()`'s status —
so `input_server` is still worth starting early. It just never explained this
bug.

Two further attempts to save an `input_server` explanation, both closed:

- **"`input_server` is degraded on this image, and that shifts when the keymap
  appears."** It is degraded — `input_server/devices/virtio` is absent and the
  `shortcut_catcher` and `screen_saver` filters fail on missing `libgame.so` and
  `libscreensaver.so`. But it cannot matter: the keymap is loaded by
  `_InitKeyboardMouseStates()` in the `InputServer` *constructor*
  (`InputServer.cpp:168`), and `fAddOnManager->LoadState()` — where every one of
  those failures happens — is called twelve lines later (`:180`). `fKeys` is
  populated before any add-on is touched, `IS_GET_KEY_MAP` is answered from
  `fKeys`, and messages are not served at all until the constructor returns and
  the looper runs. `_LoadSystemKeymap()` (`:281`) is a compiled-in fallback, so a
  keymap exists even with no `Key_map` file. There is no window in which
  `input_server` is answering and has no keymap.
- **"It is a low-vCPU scheduling race: on one CPU, Deskbar loses to
  `input_server` consistently."** Measured across three shapes, and it is not.
  On every one of them `input_server` was *already running before Deskbar
  started* — team 101 &lt; 139 on 1 vCPU, 113 &lt; 142 on 2 vCPU, 111 &lt; 148 on
  8 vCPU — so there is no ordering left to lose. And the failure reproduces at 8
  vCPU (see the shape matrix under Verification), pixel-for-pixel identical to 1
  vCPU. vCPU count is not the variable; **being on a first boot is**, because
  `default_deskbar_items.sh` adds four replicants and that is what pushes the
  Deskbar's output past 16 KiB before a client exists.

## What it is

`RemoteHWInterface` owns a **16 KiB** `StreamingRingBuffer` for outbound drawing
protocol. The only thing that ever drains it is the `NetSender` belonging to the
currently connected client, and that object is created in `_NewConnection()`.
Before the first client connects — and again after one dies, because a failed
`Send()` just returns from the sender thread — **nothing reads the buffer**.

`StreamingRingBuffer::Write()` on a full buffer waited on
`acquire_sem(fWriterNotifier)` with no timeout. So the first application to emit
more than 16 KiB of protocol before a client arrives blocks forever, inside
app_server, on the `ServerWindow` thread that serves it.

Deskbar is reliably that application: its leaf and tray icons cross the wire as
raw uncompressed bitmaps. Tracker's boot window is a flat fill and usually fits.

The full chain, every link of it directly observed on a wedged instance:

| # | Observation | Measured by |
|---|---|---|
| 1 | a thread is blocked writing to the send ring buffer | `listsem <app_server>`: `StreamingRingBuffer write notify` count **-1** |
| 2 | the blocked thread is app_server's ServerWindow for Deskbar | `ps -a`: `w:142:Deskbar` in `wait` |
| 3 | Deskbar's own window thread is blocked on the reply | `ps -a`: `w>Deskbar` in `wait` |
| 4 | it is holding the window looper lock | `listsem <Deskbar>`: sem `Deskbar` count **-1** (one waiter) |
| 5 | its window port has filled to capacity, so no update ever gets processed | `listport <Deskbar>`: port `Deskbar` **200 queued / 200 capacity** |
| 6 | Deskbar's app looper is stuck waiting for that lock | the waiter in (4) |
| 7 | `ProcessController -deskbar` is blocked forever in `BDeskbar::AddItem()` | resident single-thread team, minutes old |
| 8 | so `default_deskbar_items.sh` and the whole `first-login` PostInstallScript never finish | both still resident in `ps` |

`BDeskbar` uses `fMessenger->SendMessage(&request, &reply)` with no timeout
everywhere, which is why (7) and (8) follow from (6) rather than erroring out.

### Causal test

Connecting a client to a wedged session flipped every one of those at once.
`RemoteHWInterface::_NewConnection()` calls `fSendBuffer->MakeEmpty()`, which
cancels the blocked writer:

| | before connect | after connect |
|---|---|---|
| `StreamingRingBuffer write notify` | **-1** | 0 |
| Deskbar window port queued | **200 / 200** | 0 / 200 |
| `ProcessController` / `default_deskbar_items.sh` / `PostInstallScript` teams | all three resident | all gone |
| drawing ops from the top-right window token | none had ever been sent | 29, including bitmaps and clock text |

That also explains why `quit` + relaunch cures it: by then a client is
connected, so the replacement Deskbar never blocks.

## The fix

`StreamingRingBuffer` gains an opt-in `discardWithoutReader` mode plus
`SetReader()`/`ClearReader()`. In that mode a `Write()` that finds the buffer
full **and no registered reader** discards instead of waiting. `NetSender`
registers itself as the reader in its constructor and un-registers in its
destructor *and* when its sender thread exits, so registration tracks a live
drain rather than a live object. `RemoteHWInterface` asks for the mode on the
send buffer only; the receive buffer, and the `RemoteDesktop` client's own
buffers, keep the original blocking behaviour byte for byte.

`ClearReader()` is a no-op unless the caller is the current reader, so a sender
being torn down cannot un-register its successor — the ordering inside
`_NewConnection()` (`fSender.Unset()`, `MakeEmpty()`, new `NetSender`) leaves
exactly that window open.

Discarding is safe, not lossy in any way that matters: a client that connects
later gets `MakeEmpty()` and then a full repaint from `_NotifyScreenChanged()`,
which is already how reconnects work. Behaviour with a client attached — including
a slow one — is unchanged: writers still block until the socket drains.

### Why not fix the ordering instead

The obvious-looking alternatives were all worse:

- **Make `input_server` a `launch_daemon` service and have Deskbar `requires`
  it.** It is not a job in any launch file today, and `requires` naming an
  unknown job makes `launch_daemon` *delete the job that named it* — that would
  remove Deskbar from the session outright. Even done correctly it would be
  useless: `requires` releases when the required job's team has been spawned and
  resumed, not when it is ready, and the two are in different `launch_daemon`
  contexts (system vs user) which cannot see each other's jobs at all.
- **Wait on a readiness event.** `launch_daemon` does support external sticky
  events (`initial_volumes_mounted` is one), but a job carries a single event
  tree and there is no working `and` combinator — `and { }` parses and then
  silently degenerates into an external event named "and" that never fires. So
  Deskbar cannot wait on *both* `initial_volumes_mounted` and a new keymap
  event without new `launch_daemon` machinery in the boot path.
- **Have `remote-desktop.sh` quit and relaunch Deskbar after a `sleep`.** Treats
  the symptom, adds its own race, and leaves the failure live for every other
  application that draws early.

None of them would have worked anyway, because ordering was never the cause.
The buffer had no reader, and no arrangement of start-up order creates one.

## Verification

Two arms, **35 boots across three instance shapes (1, 2 and 8 vCPU)** -- 17 stock, 18 patched, all from
the canonical AMI. The control arm is the AMI's own `app_server`; the test arm is
that same binary with this patch applied, dropped into
`/boot/system/non-packaged/servers/` and selected by a
`~/config/settings/launch/` override that only replaces the `launch` line. Which
one ran was confirmed each boot from `listimage`'s image path, not assumed.

**The control needs no caveat.** Rebuilding this tree at `graviton` tip with the
patch reverted produced an `app_server` whose md5 is *bit-identical* to the one
in the AMI (`dba228479817b588899e644c90318671`). So exactly one thing differs
between the arms: these four files.

Pixels come from `graviton/scripts/rdcapture.py`, which completes the
remote-display handshake, renders the ops it receives into a software
framebuffer and counts pure-black pixels in the top-right 137x70. Its selftest
(38 assertions, including PNG round-trip and exact fill arithmetic) passes, and
a healthy Deskbar scores 213 on it — icon outlines plus a window-list row this
renderer does not fill.

### First boot — the condition the failure was reported under

The trigger is a **first boot**, not a shape: `default_deskbar_items.sh` adds four
replicants, and that is the load that carries Deskbar's output past 16 KiB before
any client exists. Re-armed between boots by removing
`~/config/settings/deskbar` and touching `~/config/settings/first_login`; the
first boot of each freshly launched instance is a genuine one, untouched.

| shape | vCPU | arm | boots | blocked send writer | window port 200/200 | `first_login` stuck | **black px in corner** |
|---|---|---|---|---|---|---|---|
| `c7g.medium` | 1 | stock | 6 | **6 / 6** | 6 / 6 | 6 / 6 | 3422, 3422, 197, 197, 3422, 3422 → **4 black** |
| `c7g.large` | 2 | stock | 5 | **5 / 5** | 5 / 5 | 5 / 5 | 6075, 213, 213, 6075, 6075 → **3 black** |
| `c8g.2xlarge` | 8 | stock | 1 | **1 / 1** | 1 / 1 | 1 / 1 | 3422 → **1 black** |
| `c7g.medium` | 1 | fixed | 5 | 0 / 5 | 0 / 5 | 0 / 5 | 197 ×5 |
| `c7g.large` | 2 | fixed | 5 | 0 / 5 | 0 / 5 | 0 / 5 | 213 ×5 |
| `c8g.2xlarge` | 8 | fixed | 3 | 0 / 3 | 0 / 3 | 0 / 3 | 213 ×3 |

**Stock: the defect is present in 12 of 12 first boots, on every shape.** Fixed:
0 of 13, with the `first_login` chain completing and the tray populated every
time. 6075 is not an approximation of the field report's figure for a wedged
Deskbar, it is the same number; 3422 and 197 are the same states measured on a
box with fewer desktop icons.

Note where the *race* lives, because it is not where it looks. The defect is
deterministic. What varies is only whether connecting a client happens to rescue
the Deskbar — 8 of 12 stock boots stayed black for good, 4 recovered — and that
ratio is **the same at 1, 2 and 8 vCPU**. A single stock boot proves nothing in
either direction on any shape, which is exactly what the field report warned.

The wedged capture at 8 vCPU is pixel-for-pixel identical to the one at 1 vCPU:
black where the leaf should be, an empty grey tray band, a black window-list row
with one stray icon. That is what a "1 vCPU only" hypothesis has to explain and
cannot.

### Warm reboot, tray already populated

| | blocked send writer | Deskbar window port |
|---|---|---|
| stock, 5 boots | yes, 5/5 | 200 / 200, 5/5 |
| fixed, 5 boots | no, 0/5 | 0 / 200, 5/5 |

Here the corner reads 213 on both arms, because the pre-connect wedge is
released by the act of connecting: `_NewConnection()` cancels the blocked write
and the fresh client asks for a full repaint. **A screenshot taken after
connecting cannot see this bug** unless the recovery fails — which is what the
first-boot load makes likely. That is why the semaphore and port-depth readings
above carry the result and the pixels only corroborate it.

**Verdict: closed for the mechanism, not merely narrowed.** The blocking wait
that caused it no longer exists, and the condition is a property of the buffer
rather than of start-up timing, so it covers "no client yet", "client died" and
"client too slow" alike. It was verified on the shape the failure was reported
from (1 vCPU) as well as on the shape where it was believed absent (8 vCPU).

What is *not* proven is the mid-session case: a disconnect-then-relaunch
experiment did not reproduce a wedge on the stock binary either, so that leg is
reasoned from the code, not measured.

### If you need to reproduce it

Launch from the canonical AMI and look **before connecting any client** — the act
of connecting is what releases the wedge, so a screenshot taken afterwards may
show a healthy Deskbar over a defect that was definitely there. Any shape will
do. Two readings settle it, and both were 12/12 on stock:

```
AS=$(ps | grep -m1 servers/app_server | tr -s ' ' | cut -d' ' -f2)
DB=$(ps | grep -m1 system/Deskbar     | tr -s ' ' | cut -d' ' -f2)
listsem  $AS | grep 'write notif'   # count -1 == a producer is blocked
listport $DB | grep ' Deskbar '     # 200 queued of 200 == the looper is wedged
```

To make it *visible* as well, force a first boot: `rm -rf
~/config/settings/deskbar; touch ~/config/settings/first_login; sync; shutdown
-r`. Expect roughly two boots in three to stay black.

## Loose ends worth a follow-up

- `BDeskbar`'s synchronous calls have no timeout, so any wedged Deskbar still
  hangs whoever talks to it — and, through `default_deskbar_items.sh`, the whole
  `first-login` chain. Bounded waits there would turn a hang into an error.
- `NetSender`'s destructor does not join its thread; a sender thread can outlive
  the object briefly. `ClearReader()`'s ownership check makes that harmless for
  the buffer, but the underlying race is still there.
- A client that survives the reconnect can still show a stale black corner from
  its own cached drawing state; that is a client-side issue, not this one.
- Unrelated but found while doing this: the image's sshd **silently truncates
  large writes**. `scp` of a 1.9 MB binary stopped at 522 KB, and piping into
  `ssh 'cat > file'` stopped at exactly 128 KiB. Splitting into 64 KiB chunks
  still lost roughly one chunk in three, with no error on either end — the
  transfer only succeeded because each chunk's size was verified and retried.
  Anything that copies a file onto one of these guests should checksum it.
- Useful side effect of the control build: `app_server` built from `graviton`
  tip is byte-identical to the one in the canonical AMI, so an AMI binary can be
  used directly as the control arm of an A/B without a provenance caveat.
