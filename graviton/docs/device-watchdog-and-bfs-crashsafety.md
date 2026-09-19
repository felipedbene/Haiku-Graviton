# Device-watchdog framework + BFS forced-stop crash-safety (#91)

Two threads live under issue #91. They share a theme — *what happens when a
device or the machine stops abruptly on a console-less cloud instance* — but they
are otherwise independent, so this document treats them separately.

- **(a) A reusable device-watchdog framework** so drivers other than ENA can get
  the "notice a wedged device and recover it" behaviour without re-deriving the
  hard parts, with the "don't reset a healthy device" lesson baked into the API.
- **(b) BFS forced-stop crash-safety** — what a hard power loss or an abrupt
  device removal actually costs a BFS volume today, and how to close the gaps.

Everything below was read out of the tree at the commit this document lands on.
Claims about *runtime* behaviour that have not been reproduced on hardware are
marked **UNVERIFIED**; they are hypotheses with a code pedigree, not measurements.

---

## Part A — Device-watchdog framework

### Current state (what already exists)

The framework already landed (PR #252, commit `4e16ab271e`, "kernel: add a
general device liveness watchdog framework"). It is not a proposal — it is
in-tree and wired up:

- `headers/private/kernel/device_watchdog.h` — the public contract.
- `src/system/kernel/device_watchdog.cpp` — the implementation (one monitor
  thread, a registration list, a `device_watchdogs` debugger command).
- `src/system/kernel/main.cpp` — `device_watchdog_init()` at line 188 and
  `device_watchdog_init_post_thread()` at line 251 bring it up during boot.
- `src/system/kernel/Jamfile` — builds `device_watchdog.cpp` into the kernel.

**It has zero adopters.** The ENA driver still runs its own watchdog thread
(`ena_watchdog` in `ena.cpp`); nothing in the tree calls
`register_device_watchdog()`. So the scaffolding is done and proven-to-compile;
the open work is (1) deciding whether ENA should adopt it and (2) any adoption
by a second driver.

### The API as landed

```c
device_watchdog* register_device_watchdog(const device_watchdog_parameters*);
void             unregister_device_watchdog(device_watchdog*);   // blocks out in-flight recover()
void             device_watchdog_pet(device_watchdog*);          // lock-free, IRQ-safe "observe"
void             device_watchdog_set_enabled(device_watchdog*, bool);
```

`device_watchdog_parameters` carries `name`, `timeout` (heartbeat deadline),
`interval` (check cadence), `misses_before_recover`, a required `recover(cookie)`
callback, an optional `probe(cookie)` liveness callback, and `cookie`.

The two structural decisions carried over verbatim from the ENA watchdog, both
of which were paid for on that driver, are exactly right and must stay:

1. **Observe is separated from decide.** `device_watchdog_pet()` only stamps a
   timestamp with `atomic_set64` — lock-free, safe from the interrupt context a
   driver's healthy datapath lives in. All blocking work (`recover`, `probe`)
   runs on the framework's own thread.
2. **A single stale sample never acts.** `misses_before_recover` defaults to 2; a
   device whose `recover()` returns non-`B_OK` is **latched dead** and left inert
   rather than reset in a loop. On a console-less host a reset loop is strictly
   worse than the wedge it was trying to fix — this is the #98/watchdog lesson in
   the type system.

The monitor thread runs at `B_URGENT_DISPLAY_PRIORITY` so that scheduler latency
is never misread as device death, sleeps on a semaphore (so a registration wakes
it immediately), and iterates with a marker/swap so the list survives the lock
being dropped around a callback. This is correct and matches the low-resource
manager's `call_handlers()` pattern.

### The gap that matters for adoption: binary probe vs bounded patience

The framework's `probe` is **binary**: return `true` and the miss count is
cleared outright (`check_watchdogs()` treats a positive probe as a fresh
heartbeat). ENA's watchdog does something deliberately different and harder-won.
`ena_watchdog_check_keep_alive()` grants a device that is *still moving frames*
**more misses, but a bounded number** — `ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC`
(8) instead of `ENA_KEEP_ALIVE_MISSES_BEFORE_RESET` (2) — and then resets it
anyway:

> "a device that moves frames while its management path is dead is exactly the
> partial wedge the watchdog exists to catch."

If ENA adopted the framework naively with `probe = "datapath moved this
interval"`, a NIC whose management path is dead but whose datapath is still
draining a queue would report a positive probe every tick and **never be
reset** — a behavioural regression versus what ships today. The binary probe
cannot express "still moving buys N extra misses, capped."

**Design consequence.** Two honest options; this document does not pick one
without a hardware A/B (see "What needs proof"):

- **Option A — keep ENA's specialised thread, share only the shape.** ENA's
  watchdog is five ordered checks (device-requested reset, wedged admin queue,
  keep-alive liveness, missing-TX-completion, RX stall), four of which can reset.
  Only the keep-alive check maps cleanly onto a single heartbeat+probe. The other
  three are datapath-progress checks the keep-alive test is blind to by
  construction. Collapsing them into one heartbeat loses fidelity. Under this
  option the framework serves *new, simpler* drivers (a single "is it alive"
  heartbeat) and ENA is left as-is — the framework's value is that the next
  driver does not re-derive the thread/priority/latch/IRQ-safety machinery.

- **Option B — extend the framework to express bounded patience**, then adopt in
  ENA. Add an optional `misses_before_recover_probing` (the cap that applies
  while `probe` is positive, distinct from the idle cap), so a positive probe
  extends patience without granting immunity. This is a small, backward-
  compatible addition (zero selects "probe clears outright", the current
  behaviour). ENA would still need its non-keep-alive checks expressed somehow —
  either as additional watchdog registrations (one per failure mode, each with
  its own `probe`) or by keeping those checks inline and using the framework only
  for keep-alive. A per-failure-mode registration is the cleaner shape but
  multiplies the monitor thread's per-tick callback cost; measure before
  committing.

**Recommendation (unverified until an A/B exists): Option A now, Option B only if
a second real driver needs bounded patience.** The framework's job today is to
stop the *next* driver from copy-pasting ENA's thread. ENA itself is measured,
tuned, and shipping; re-plumbing it through a less expressive API to prove a
point risks the exact false-reset regression the ENA work spent commits
eliminating (`58c02668ba`, `de4422feb5`). Adopting ENA is a behaviour change on
the flagship NIC and is gated on a hardware A/B with `ena_fault`, not on this
document.

### Deadline/slack guidance for new adopters (the "don't reset healthy" rule)

For a driver picking up the framework fresh, the parameter choices that encode
the lesson:

- `timeout` should be several times the device's *expected* healthy heartbeat
  period, not equal to it. ENA emits keep-alives on the order of a second and
  sets a **6 s** timeout — roughly 6x slack — precisely so that one late event
  under load is not death.
- `misses_before_recover` must be **≥ 2**. A value of 1 acts on a single sample;
  the header already discourages it in a comment. ENA measured that one missed
  deadline is not evidence of a dead device.
- Provide a `probe` when the device has an independent, cheap liveness signal
  (ENA: "did the datapath move frames this interval?"). A positive probe is
  direct evidence of progress and buys the merely-late heartbeat its patience.
- The effective silence before recovery is
  `timeout + (misses_before_recover - 1) * interval`. Write that number down for
  the device and sanity-check it against how long a *legitimate* stall (a long
  DMA, a slow admin command) can last. ENA's is 7 s idle.

---

## Part B — BFS forced-stop crash-safety

### How BFS keeps itself consistent today

BFS is a journalled filesystem with a **redo write-ahead log** (a ring in the
log area). The consistency machinery, as read from `Journal.cpp` / `Volume.cpp`:

- A transaction's changed blocks are written into the log area first
  (`_WriteTransactionToLog()`), via `writev_pos()` straight to the device.
- The **commit** is the superblock's `log_end` pointer: advancing it (and setting
  `SUPER_BLOCK_DISK_DIRTY`) via `Volume::WriteSuperBlock()` — a direct
  `write_pos()` at byte offset 512 — is what makes replay treat the entry as
  valid.
- Only after the log entry is durable does `cache_end_transaction()` allow the
  in-place ("home location") blocks to be written back. When those complete,
  `_TransactionWritten()` advances `log_start`, freeing the log space.
- On mount, if `log_start != log_end`, `Journal::ReplayLog()` replays every entry
  from `log_start` to `log_end`, then the volume is clean.

That ordering — *log durable → commit → in-place writeback → free log* — is the
correct redo-log shape. The gaps are in the **barriers between those steps** and
in **replay's ability to detect a torn commit**.

### Gap 1 — no barrier between the log body and the commit record (FIXED here)

In `_WriteTransactionToLog()` the log-body `writev_pos()` calls and the
superblock commit `write_pos()` both go to the device, and **historically there
was a single `B_FLUSH_DRIVE_CACHE` after both.** On a device with a volatile
write-back cache — every real disk, and the virtualised NVMe/virtio-blk paths a
Graviton guest sees — the two writes may reach the platter in **any order**. If
the drive persists the advanced `log_end` before the entry body it points at,
and power is lost in that window, replay walks a log entry whose body never
landed.

Replay's detection is only partial: `_CheckRunArray()` sanity-checks the
run-array *header* (`MaxRuns`, `CountRuns` bounds) and `ValidateBlockRun()`
checks each run's geometry, and a run that writes over the superblock is
`CheckSuperBlock()`-validated. But **there is no CRC or sequence number over the
log-entry body or its data blocks.** A stale-but-plausible run array left in that
log slot from a previous wrap passes every check and replays stale block runs
into live blocks — silent corruption.

**Fix implemented in this change:** insert a `B_FLUSH_DRIVE_CACHE` after the
log-body writes and **before** the superblock commit write, giving the standard
two-barrier WAL commit ordering:

1. write log body → **flush** (body now durable)
2. write superblock `log_end` (commit) → **flush** (commit now durable, and the
   flush also orders the log ahead of the subsequent in-place writeback)

After the first flush the body is on stable storage, so if power is lost during
or after the commit write, replay either sees the old `log_end` (entry ignored,
consistent) or the new one backed by a fully-durable body (entry replayed,
consistent). This is a strictly-additive durability barrier: worst case it costs
one extra cache flush per transaction commit; it cannot introduce corruption.

**Cost / caveat (UNVERIFIED):** it doubles the cache-flush count per commit,
which on a flush-bound workload is a real write-latency cost. The magnitude is
unmeasured. `B_FLUSH_DRIVE_CACHE` is a blunt full-cache drain; a lighter
mechanism (FUA/ordered writes) would be preferable but is not currently plumbed
through Haiku's block device ioctl surface. Whether the extra flush is worth its
cost, and whether the corruption window is reachable on the specific virtualised
block paths we run, **needs a crash-injection A/B on hardware before this can
merge** (see below). The change builds; it is not yet proven.

### Gap 2 — replay has no integrity check over the log body

Independent of Gap 1, the log format carries no per-entry checksum or monotonic
sequence number. This is why Gap 1 is *silent* rather than *detected*. A robust
fix (larger, out of scope for the first step) would add a CRC and a sequence/
generation number to the run-array header written at commit time, and have
`_CheckRunArray()` reject an entry whose CRC or sequence does not match — turning
a torn or stale commit into a detected "stop replay here" instead of applied
corruption. This is an **on-disk format change** (or a use of currently-reserved
header fields) and must be backward-compatible with existing volumes; it is
noted here as the natural follow-up, not attempted now.

### Gap 3 — abrupt device removal has no forced-flush hook

`Volume::Unmount()` flushes the log and blocks (`delete fJournal` →
`~Journal()` → `FlushLogAndBlocks()`) — that is the *clean* path. There is no
BFS path invoked on **abrupt device removal** (the block device disappears, e.g.
a hot-detached EBS volume or a stopped instance). On such an event the in-flight
transaction and any not-yet-written-back dirty blocks are simply lost. That is
"correct" in the sense that the journal makes the on-disk state consistent to the
last committed transaction — *provided Gap 1 is closed*. What is lost is the
uncommitted work, which is expected for power loss. The adjacent page-writer bug
(files never flushed until an explicit `sync`, see the EC2 stop/start memory)
compounds this: it widens the window of "acknowledged to userland but not yet
handed to the FS," which no journal can recover. That is a VFS/page-writer issue,
not a BFS-journal issue, and is tracked separately; it is called out here because
a crash-safety story that only fixes the journal will still lose data if the page
writer sits on it.

### Gap 4 — a failed log-body or commit write was ignored, then committed anyway (FIXED here)

Distinct from the *ordering* problem of Gap 1, `_WriteTransactionToLog()` did not
check whether its writes actually reached the device before committing. Both
log-body `writev_pos()` calls only logged `FATAL(...)` on failure and then **fell
through**, and the superblock commit write (`WriteSuperBlock()`) advanced the
in-memory `log_end` (`fVolume->LogEnd() = logPosition`) and ended the cache
transaction **regardless of its return value**.

The consequence on an abrupt device stop (a hot-detached EBS volume, a stopped
instance, a wedged NVMe/virtio-blk path) is a *guaranteed* torn commit rather than
a windowed one: the body write fails, yet `log_end` is advanced to cover it, so
replay walks an entry whose body never landed — and, per Gap 2, there is no
checksum to notice. Worse, if only the commit (superblock) write failed while the
body succeeded, the in-memory `log_end` still advanced and the cache transaction
was still ended, so the block cache was then free to write those dirty blocks back
to their home locations with **no durable log entry behind them** — a write-ahead
log violation that corrupts on the next crash.

**Fix implemented in this change** (`Journal.cpp`, all on the error path only, so
the healthy path is byte-for-byte unchanged):

- A failed log-body `writev_pos()` now returns `B_IO_ERROR` instead of falling
  through to the commit. The on-disk `log_end` is left at the last good
  transaction, which is consistent. (The mid-array wrap case matches the
  pre-existing `block_cache_get`-failure return above it; the end-of-array case
  releases the block-cache references it already took, since at that point every
  run of the array has been fetched.)
- A failed `WriteSuperBlock()` (the commit record) now returns the error without
  advancing `log_end` or ending the cache transaction. The transaction's blocks
  stay pinned in the block cache rather than being written back to their home
  locations un-journalled; the next flush retries, and a device that is truly
  gone keeps failing here without ever leaving the on-disk state inconsistent.

This is the *write-side* complement to Gap 1's *read/ordering-side* barrier: Gap 1
makes a body that we chose to commit durable-before-commit; Gap 4 makes sure we
only choose to commit a body (and a commit record) that the device actually
accepted. Both are strictly-additive on the failure path and cannot affect a
healthy commit. Like Gap 1, the corruption they prevent is **UNVERIFIED** as
reachable on our specific virtualised block paths and is owed a crash-injection
A/B before merge (see below).

### What a hard power loss loses today (summary)

- **Committed, replayed transactions:** safe — *once Gap 1 is closed*. Before
  this fix, a commit torn against its body is a silent-corruption risk
  (UNVERIFIED whether reachable on our block paths).
- **Uncommitted transactions:** lost. Expected and correct.
- **Dirty file data still in the page cache (not yet in a transaction):** lost,
  and the window is larger than it should be because of the page-writer
  flush-on-sync bug — separate issue.

---

## What was implemented in this change

- This design doc.
- **Gap 1 fix:** the pre-commit `B_FLUSH_DRIVE_CACHE` in
  `Journal::_WriteTransactionToLog()`. Builds clean:
  `jam -q bfs` → `...updated 872 target(s)...`, the `bfs` add-on links (only the
  pre-existing ld 2.41 `.comment` orphan-section warnings, unrelated).
- **Gap 4 fix:** the two failed-body `writev_pos()` sites and the failed
  `WriteSuperBlock()` commit in `Journal::_WriteTransactionToLog()` now abort the
  commit instead of proceeding. Error-path only; the healthy commit is unchanged.

Nothing in Part A is changed: the framework is already in-tree, and ENA adoption
is deliberately **not** attempted here because it is a behaviour change on the
flagship NIC.

## What needs hardware/emulator proof before merge

- **Gap 1 + Gap 4 fixes (crash-safety, load-bearing).** Boot the `bfs`-with-fix
  image on a reaped test instance (or QEMU), drive a write-heavy workload, and cut
  power
  abruptly (instance stop / `kill -9` QEMU / device-stop) at randomised offsets,
  repeated. Then remount + `fsck`/`bfs_shell`. **Before/after A/B on the same
  instance:** the "before" arm is the unpatched `bfs`; success = the patched arm
  survives commit-window cuts that the baseline can corrupt (or, if the baseline
  never corrupts on our block paths, that is itself the finding — the window may
  not be reachable, and the fix is then insurance with a measured flush cost).
  Also measure the write-latency cost of the extra flush (Gap 1 caveat) so the
  merge decision weighs correctness against throughput.
- **ENA framework adoption (Part A, if pursued).** Any move of ENA onto the
  framework must be exercised with `ena_fault` (reset/error-unwind, keep-alive
  suppression) and A/B'd for the false-reset regression the binary-probe gap
  predicts. Held until then.

Both are held per the #91 guardrails: kernel/FS PRs do not hotswap on the boot
path, and the "after" arm of the A/B is owed before merge.
