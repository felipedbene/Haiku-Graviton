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

## Loose ends worth a follow-up

- `BDeskbar`'s synchronous calls have no timeout, so any wedged Deskbar still
  hangs whoever talks to it — and, through `default_deskbar_items.sh`, the whole
  `first-login` chain. Bounded waits there would turn a hang into an error.
- `NetSender`'s destructor does not join its thread; a sender thread can outlive
  the object briefly. `ClearReader()`'s ownership check makes that harmless for
  the buffer, but the underlying race is still there.
- A client that survives the reconnect can still show a stale black corner from
  its own cached drawing state; that is a client-side issue, not this one.
