# Storage on Graviton: the first numbers

**Date:** 2026-08-24. **Hardware:** `c7g.4xlarge` (Graviton3 / Neoverse V1,
16 vCPU, 32 GiB RAM), `us-west-2`, canonical AMI `ami-0d61e3910062bb80a`,
DeBeOS `hrev59996`. **Reference:** an identical `c7g.4xlarge` running Amazon
Linux 2023 with `fio` 3.32, on an identically provisioned volume.

Storage had never been measured in this tree. There was not a single disk
throughput or IOPS figure anywhere in `graviton/docs/`, which is a conspicuous
gap given that the worst bug found in the whole project was on this path: the
page writer never flushed file data at all, and it was found by accident when an
EC2 stop/start lost an sshd host key that came back with the right size, mode and
mtime and 411 bytes of zeros in it.

This document is the baseline. It also records two measurement errors caught
before they became published numbers, and one claim from a code review that
turned out to be an artifact.

## Two generic Haiku defects found as prerequisites — read these first

Neither was introduced by this project, neither is storage-specific, and both were
found only because this investigation needed the kernel debugger. They are here at
the top rather than buried in the setup because a future contributor would
otherwise rediscover them the hard way, and in both cases the tool answers
*plausibly and wrongly* rather than failing.

### `bt <thread>` on arm64 traced the calling thread, not the named one

arm64's `stack_trace()` advertises `[ <thread id> ]` in its usage string, parses
`argv` only far enough to validate the argument count, and then does
`thread_get_current_thread()`. So `bt <blocked-thread>` printed the *debugger's own*
stack, correctly formatted and completely wrong. x86_64 has the same usage string
and does the work via `setup_for_thread()`.

Fixed in `arm64: make KDL 'bt <thread>' actually trace that thread`, which also
found that **`arch_debug_save_registers()` was an empty stub** — so the frame
pointer for a thread running on another CPU was whatever the struct happened to
contain. `bt` across CPUs has therefore been quietly wrong on arm64 for as long as
it has existed.

### KDL has never usefully worked on arm64 at all

Three compounding defects, fixed in `arm64: fix serial getchar, and give KDL a way
in on a headless machine`:

1. **No way to enter KDL on demand.** `debug_emergency_key_pressed()` has exactly
   three callers — x86's console interrupt handler, USB HID, PS/2 — and arm64 has
   none of them. A headless Graviton instance has no keyboard either. The debugger
   was reachable *only* by a panic.
2. **Typed commands would have been garbage.** `arch_debug_serial_try_getchar()`
   returned a `char`; **plain `char` is unsigned on AArch64**; so
   `DebugUART::GetChar(false)`'s -1 ("nothing waiting") became 255. `kgetc()` tests
   `c >= 0`, so KDL accepted an endless stream of `0xFF` instead of waiting for
   input.
3. `arch_debug_serial_getchar()`, the *blocking* half, passed `wait=false` and
   returned 255 immediately on an empty FIFO.

The second is a textbook signedness bug in a place nobody would look, and it is why
a whole debugging facility has been dead on this architecture.

**Consequence worth stating plainly:** until that fix ships, no hang, deadlock or
starvation on Graviton can be inspected interactively. That is a permanent
capability gap rather than an inconvenience, and it is why the page-writer
diagnosis below had to be done with counters instead of stack traces.


## The tool

`src/bin/disktput`, plus the harness `graviton/scripts/disktput-run`. Same split
as `nettput`/`nettput-run` and for the same reason: a stock image has no `fio`,
no `bonnie`, no reliable `dd` and no compiler, so before this there was no way to
ask whether a storage change helped.

It reports, per run: throughput, IOPS, mean/p50/p99/max latency, and **CPU
microseconds per mebibyte** summed from `cpu_info::active_time` across every CPU.
The last one is the number that separates a slow device from an expensive
software path, and it is the reason `nettput` was able to show that jumbo frames
cut per-byte cost to a quarter while barely moving the rate.

Four things in it exist because a storage number is easy to get wrong in a
specific way:

- **Concurrency is an explicit parameter, and threads are the queue depth.** See
  the finding below: the driver blocks per command, so one thread is one request
  outstanding no matter what the hardware allows.
- **Buffer alignment is an explicit parameter** (`-A`, `-U`). `malloc` gives no
  page-alignment guarantee, and on this driver an unaligned buffer silently
  diverts to a 16 KiB bounce path. A benchmark that just calls `malloc` measures
  whichever path it happened to land in.
- **Runs are timed, not sized** (`-T`). See "Error 2" below — this one produced a
  number above the instance's hard ceiling.
- **The cache state is printed with every result**, so a figure cannot be quoted
  without it. On a regular file `-D` opens with `O_NOCACHE`, which BFS honours by
  calling `file_cache_disable()` on the inode for as long as the descriptor is
  open; on a raw device the report says the file cache is not applicable, because
  devfs installs none.

## What the hardware actually allows — state this before any result

Both the volume's provisioned limits and the instance's own EBS limits matter,
and the ceiling is the lower of the two. Reporting a provisioned cap as an OS
limitation would be a straightforward mistake.

| | value | source |
|---|---|---|
| scratch volume | gp3, 100 GiB, **16,000 IOPS, 1,000 MiB/s** provisioned | `describe-volumes` |
| instance EBS baseline | **625 MB/s, 20,000 IOPS** | `describe-instance-types` |
| instance EBS maximum (burst) | **1,250 MB/s, 40,000 IOPS** | `describe-instance-types` |
| **effective sustained ceiling** | **~1008 MiB/s**, set by the volume | *measured*, see below |

The prediction here was that the instance's 625 MB/s baseline would bind first.
It did not: measured sustained throughput is ~1008 MiB/s in both directions, i.e.
the volume's provisioned 1,000 MiB/s. Recorded as a wrong prediction rather than
quietly corrected, because the ceiling is the thing every other number is read
against and it should be clear that it was measured and not assumed.

### Which volume — this determines whether any number here means anything

**The canonical AMI's root volume is 2 GiB gp3, which sits at the gp3 floor of
3,000 IOPS / 125 MiB/s.** Anything measured against the root disk measures that
floor and nothing about the OS: no result above ~125 MiB/s on that volume can
have come from the device, and a "gap" found below it would be fictional. That
volume also carries a 300 MiB BFS filesystem with ~232 MiB free — 135× smaller
than the machine's 31.5 GiB of RAM, so no working set that fits on it can defeat
the page cache, and buffered reads there have been measured at 6.4–8.5 GiB/s, a
50–70× inflation.

**None of the results in this document were taken on the root volume.** Every
number is from a separately attached scratch volume, and the reference was taken
on an identically provisioned one:

| | DeBeOS node | Linux reference node |
|---|---|---|
| instance | `i-001f6d2794826239f`, c7g.4xlarge, us-west-2a | `i-055f9686cf04f7f28`, c7g.4xlarge, us-west-2a |
| **measured volume** | `/dev/sdf` → `vol-01f1cf7ebabcd78b3` | `/dev/sdf` → `vol-0f14f1d7285e0f1a2` |
| provisioning | **gp3, 100 GiB, 16,000 IOPS, 1,000 MiB/s** | **gp3, 100 GiB, 16,000 IOPS, 1,000 MiB/s** |
| root volume (*not measured*) | `/dev/xvda`, 2 GiB, 3,000/125 | `/dev/xvda`, 8 GiB, 3,000/125 |

Both arms of every comparison are therefore the same volume type, the same
provisioning, the same instance type and the same availability zone. The 100 GiB
size also means the DeBeOS filesystem tests below have a 100 GiB BFS volume
available rather than a 232 MiB one, so a working set larger than RAM is possible.

What the driver reports on this hardware, from the boot log:

```
nvme_disk: attached to NVMe device "Amazon Elastic Block Store (vol...)"
nvme_disk:      maximum transfer size: 262144
nvme_disk:      qpair count: 2
nvme_disk:      block size: 512, stripe size: 0
nvme_disk: using MSI-X
```

`262144` is **not** a driver choice: it is the controller's MDTS. `libnvme`
starts from `NVME_MAX_PRP_LIST_ENTRIES * PAGE_SIZE` (2,072,576 bytes) and clamps
it to `min_page_size << mdts`; 262144 = 4096 × 2⁶, so the Nitro controller
reports MDTS = 6.

DeBeOS reports **both** the logical and the physical sector size as 512. Linux on
the same volume reports logical 512 but **physical 4096**, so DeBeOS is echoing the
logical size into `bytes_per_physical_sector` rather than reporting what the
controller says. Nothing measured here depends on it, but anything that later
tries to align to the physical sector will be misinformed. See the minor-gap note
in the reference section.

## Method, and the controls

- **The span is fully written before any read is measured.** A never-written
  block of a fresh gp3 volume reads back as zeros *without the backend being
  touched*, and a snapshot-backed volume is loaded lazily and reads slowly on
  first touch. Those two fake a read result in opposite directions. 64 GiB is
  written once, and the harness records that it happened.
- **The span is 64 GiB, larger than the 32 GiB of RAM**, so a random read cannot
  be answered from memory.
- **Interleaved A/B, never before-then-after**, and the median of repetitions
  rather than the mean.
- **Verified by artifact.** Every run prints its build stamp, and the harness
  checks the binary's SHA-256 on the node against the one on the builder. This
  was not ceremony: two deploys in this session silently produced a **zero-byte**
  binary, and one of them overwrote a working one. An empty file deploys
  perfectly happily and a shell reports success.

Deploying the binary to the node has its own failure mode that silently produces
an empty file; it is important enough to be stated as a rule rather than a note.
See "Rules this exercise established" below.

## The reference ceiling: Linux + fio on the same volume

An identical `c7g.4xlarge` running Amazon Linux 2023, with an identically
provisioned 100 GiB gp3 volume (16,000 IOPS / 1,000 MiB/s), same 64 GiB
initialised span, `fio` 3.32, `--direct=1` on the raw device.

Two engines were measured, and the distinction is what makes the DeBeOS numbers
interpretable:

- **`psync`** — one blocking request per thread. This is *structurally the same
  shape as `disktput`*, and as DeBeOS's driver, which blocks in `await_status()`
  per command. Comparing against this asks "is our code as good as Linux's at the
  same queue depth?"
- **`libaio` with `iodepth=32`** — a deep queue. This asks "what does the
  hardware allow at all?", which is the ceiling.

**Ceiling (libaio, deep queue):**

| workload | result |
|---|---|
| sequential read, 256 KiB, qd32 | **1015 MiB/s** |
| sequential read, 1 MiB, qd32 × 4 jobs | **1025 MiB/s** |
| sequential write, 256 KiB, qd32 | **1023 MiB/s** |
| sequential write, 1 MiB, qd32 × 4 jobs | **1024 MiB/s** |
| random read, 4 KiB, qd64 × 4 jobs | **16,534 IOPS** (64.6 MiB/s) |
| random write, 4 KiB, qd64 × 4 jobs | **16,262 IOPS** (63.5 MiB/s) |

So the binding limit is the **volume's provisioned 1,000 MiB/s and 16,000 IOPS**,
not the instance's 1,250 MB/s. ~1024 MiB/s and ~16.5k IOPS are the ceiling every
number below should be read against.

**`psync`, one blocking request per thread — the directly comparable arm:**

| threads | sequential read | sequential write |
|---|---|---|
| 1 | 174.4 MiB/s | 82.1 MiB/s |
| 2 | 348.9 MiB/s | 164.2 MiB/s |
| 4 | 698.0 MiB/s | 328.3 MiB/s |
| 8 | 1039.3 MiB/s | 656.5 MiB/s |
| 16 | 1015.4 MiB/s | 1039.3 MiB/s |

Linux scales linearly to the ceiling and then flattens, exactly as a token bucket
should. Note that **Linux is also latency-bound at one thread**: 174 MiB/s is
1/6th of what the same volume does at depth 32. One blocking thread is not a
measure of a disk; it is a measure of one round trip.

**Block size sweep, one blocking thread, read** — the row that matters most:

| block size | Linux MiB/s | mean latency |
|---|---|---|
| 4 KiB | 6.9 | 565 µs |
| 16 KiB | 25.7 | 608 µs |
| 64 KiB | 77.7 | 804 µs |
| 128 KiB | 127.0 | 984 µs |
| 256 KiB | 179.0 | 1397 µs |
| 512 KiB | 327.7 | 1526 µs |
| 1 MiB | **598.0** | 1672 µs |

`max_hw_sectors_kb` is **256 on Linux too**, so Linux splits a 1 MiB request into
four 256 KiB commands for the same reason DeBeOS does. The difference is that
Linux **issues the four concurrently**: latency rises only from 1397 µs to
1672 µs for four times the data, and throughput therefore keeps climbing well past
the 256 KiB chop. This is the one place where DeBeOS's per-command
`await_status()` should cost real throughput, and it is the hypothesis the
DeBeOS block sweep below was run to test.

**Random 4 KiB read, blocking:**

| threads | Linux IOPS |
|---|---|
| 1 | 1,772 |
| 4 | 7,075 |
| 16 | 16,534 |
| 32 | 16,153 |

### Disproven, and a correction to this project's stated premise: "only 2 qpairs" is not a DeBeOS limitation

This one contradicts a premise the storage work was handed: that the NVMe driver
was "already multi-queue by design" and merely needed verifying. That premise was
wrong in the direction described *and* irrelevant in the direction that matters.

DeBeOS logs `qpair count: 2` on a 16-vCPU instance, which looks like the
negotiation in `nvme_disk.cpp` giving up 14 queues. It is not: Linux on the same
instance and the same volume reports `nr_hw_queues: 2` as well. **Two IO queues
is what this EBS controller offers.** There is no missing multi-queue work here,
and the per-CPU selection the driver already does is the right design for it.

### Minor gap: the physical sector size is not picked up

Linux reports `logical_block_size 512` / `physical_block_size 4096` for this
volume. DeBeOS's `B_GET_GEOMETRY` reports `bytes_per_physical_sector` as **512**,
i.e. it echoes the logical size rather than the controller's reported physical
size. Nothing here depends on it, but anything that later tries to align to the
physical sector will be misinformed.

## Result 1: sequential storage is at Linux parity, and reaches the ceiling

Raw device `/dev/disk/nvme/1/raw`, 256 KiB blocks, 64 GiB initialised span,
**15 s timed cells**, 2 repetitions, median. The Linux column is `fio --ioengine=psync`
on the identically provisioned volume — the same one-blocking-request-per-thread
shape.

| threads | DeBeOS read | Linux read | DeBeOS write | Linux write |
|---|---|---|---|---|
| 1 | 173.5 MiB/s | 174.4 | 81.9 MiB/s | 82.1 |
| 2 | 343.9 | 348.9 | 162.8 | 164.2 |
| 4 | 685.5 | 698.0 | 326.2 | 328.3 |
| 8 | **1072.5** | 1039.3 | 651.1 | 656.5 |
| 16 | **1072.5** | 1015.4 | **1020.0** | 1039.3 |

**DeBeOS is within 1–3% of Linux at every point, and slightly ahead at depth 8.**
Both saturate at ~1010–1070 MiB/s, which is the volume's provisioned 1,000 MiB/s.

### The duration control: 15 s cells overstate saturated rows by 6%, not by 70%

Even after `-T` was added, a 15 s cell could in principle still be riding burst
credit. Tested directly, interleaved 15 s / 120 s / 15 s / 120 s at each depth on
one volume at one offset, so only duration varies:

| depth | 15 s | 120 s | 15 s | 120 s | 180 s |
|---|---|---|---|---|---|
| 1 | 173.50 | 173.60 | 173.48 | 173.48 | — |
| 8 | 1072.52 | **1007.72** | 1021.34 | **1007.72** | **1007.32** |

**Depth 1 is completely duration-independent** — four measurements inside 0.07%.
It is latency-bound, nowhere near any rate limit, and needs no correction.

**Depth 8 decays 6.4% from 15 s to 120 s and then holds** at 1007.7 MiB/s across
120 s and 180 s. A sustained 120 s write at depth 16 gives **1008.0 MiB/s**. So
the honest sustained ceiling is **~1008 MiB/s**, and the saturated rows in the
table above are 15 s figures that overstate it by about 6%.

This is worth stating precisely because a 23–70% overshoot has been measured
elsewhere in this project on a **2 GiB gp3** volume, where 125 MiB/s of provisioned
throughput sits under a large burst bucket. On a volume provisioned at 1,000 MiB/s
there is far less burst headroom relative to the provisioned rate, and the
overshoot is correspondingly small. The size of this artifact is a property of the
volume, not a constant — which is the argument for measuring it rather than
assuming a correction factor.

The Linux reference used 30 s runs, so both arms sit at comparable durations and
the parity conclusion is unaffected.

Corrected sustained figures, which are the ones to quote:

| | DeBeOS sustained | ceiling |
|---|---|---|
| sequential read, depth 8, 120–180 s | **1007.3–1007.7 MiB/s** | ~1008 (volume) |
| sequential write, depth 16, 120 s | **1008.0 MiB/s** | ~1008 (volume) |
| sequential read, depth 1, any duration | **173.5 MiB/s** | latency-bound |

One tail worth recording from the 120 s depth-16 write: p50 3980 µs, p99 4254 µs,
but **max 28,772 µs** — a single 28.7 ms outlier in 483,872 operations. Not
enough to move a median, and exactly what a p99-and-max report exists to surface.

Scaling is 99% of linear to 4 threads, 77% at 8, and then flat — flat because the
volume ceiling has been reached, not because anything in DeBeOS gave up. Random
4 KiB tells the same story against the same ceiling:

| threads | DeBeOS IOPS | Linux IOPS |
|---|---|---|
| 1 | 1,760 | 1,772 |
| 4 | 6,977 | 7,075 |
| 16 | **17,066** | 16,534 |
| 32 | 16,228 | 16,153 |

Both reach the volume's provisioned 16,000 IOPS; DeBeOS is marginally ahead.

**There is no sequential throughput gap and no IOPS gap.** That is the honest
headline, and the reason to say it plainly is that it would have been easy to
manufacture a project out of the single-threaded figure alone.

## Result 2: the one real throughput gap — large requests at low concurrency

Block size sweep, read, one thread:

| block size | DeBeOS | Linux | DeBeOS latency |
|---|---|---|---|
| 4 KiB | 6.9 MiB/s | 6.9 | 760 µs |
| 16 KiB | 25.5 | 25.7 | 831 µs |
| 64 KiB | 80.3 | 77.7 | 1087 µs |
| 128 KiB | — | 127.0 | — |
| 256 KiB | 173.5 | 179.0 | 1782 µs |
| 512 KiB | — | 327.7 | — |
| **1 MiB** | **158.3** | **598.0** | **7449 µs** |

Parity everywhere up to 256 KiB, then a **3.8× gap** at 1 MiB — and DeBeOS at
1 MiB is *slower than it is at 256 KiB*, while Linux at 1 MiB is 3.3× **faster**
than at 256 KiB.

The mechanism is visible in the latency column and is not a guess.
`max_hw_sectors_kb` is 256 on Linux too, so both split a 1 MiB request into four
256 KiB commands. DeBeOS's latency goes 1782 → 7449 µs, a factor of **4.18** —
four commands, strictly one after another. Linux's goes 1397 → 1672 µs, a factor
of 1.20 — the four overlap.

The cause is `nvme_disk.cpp`: the chopping loop calls `do_nvme_io_request` per
piece, and that function submits one command and then blocks in `await_status()`
before the loop can submit the next:

```c
status = do_nvme_io_request(handle->info, &nvme_request);   // submits, then waits
...
nvme_request.iovecs += nvme_request.iovec_count;            // only now the next one
```

**Scope, which matters for how much this is worth.** At depth 8 the same 1 MiB
block reaches 1071.7 MiB/s — the ceiling — so the gap only exists at low
concurrency. And it cannot affect buffered file I/O at all: the file cache chops
at `MAX_IO_VECS * B_PAGE_SIZE` = **128 KiB**, below the 256 KiB MDTS, so a
file-backed request never produces more than one NVMe command. The gap is
confined to large-block I/O on the **raw device** — imaging, `dd`-style bulk
copies, the boot path — not to ordinary file access.

## Result 3: the completion path costs 70× more CPU per operation at depth 16

Not a throughput finding — throughput is at the ceiling — but the most striking
number in the whole exercise. CPU microseconds per 4 KiB random read operation,
derived from the `us of cpu/MiB` column:

| threads | CPU µs per operation | IOPS |
|---|---|---|
| 1 | **4.1** | 1,760 |
| 4 | 13.4 | 6,977 |
| 16 | **287** | 17,066 |
| 32 | 133 | 16,228 |

At depth 16 the machine spends about **4.9 CPU-seconds per wall second**, roughly
30% of a 16-core c7g.4xlarge, to deliver 67 MiB/s. Per-operation cost rising with
the number of *waiters* rather than with the work is the signature of a
thundering herd, and the code has one: there is a single MSI-X vector
(`configure_msix(pcidev, 1, &msixVector)`) and a single per-device condition
variable that the interrupt handler `NotifyAll()`s, so every completion wakes
every waiting thread and each then polls its own qpair. On ARM64
`arch_int_assign_to_cpu()` is a no-op, so the vector cannot be steered either.

This costs no throughput against a 16,000 IOPS volume, but it is CPU an
application would rather have, and it would cost throughput on a faster volume.
**Unmeasured claim:** that the herd is the cause is inferred from the code plus
the shape of the curve; it has not been confirmed by instrumenting wakeups.

## Not reproduced: the 16 KiB bounce-path cliff

The prediction was that a buffer offset off a page boundary would divert to
`nvme_disk`'s bounce path, capped at `kMaxBounceBufferSize = 4 * B_PAGE_SIZE` =
16 KiB, turning a 256 KiB request into sixteen commands. Interleaved A/B with
`-U 512`:

| | aligned | misaligned by 512 B | ratio |
|---|---|---|---|
| depth 1 | 173.4 MiB/s | 173.4 MiB/s | **1.00×** |
| depth 8 | 1072.5 MiB/s | 1019.6 MiB/s | 1.05× |

**No cliff.** The honest reading is not "the cliff does not exist" but "this test
could not have found it": a single contiguous userland buffer produces a
*single-vec* request, and the single-vec check in `nvme_disk.cpp` is lenient —
it requires only 4-byte address alignment and an LBA-multiple size, not page
alignment. The page-alignment requirement applies to the *middle* vecs of a
multi-vec request. Reproducing the bounce path therefore needs a scattered
request (`readv`/`writev` with several unaligned vecs, or an unaligned file
offset), which `disktput` does not currently generate. The cliff remains
**untested**, not disproven, and `-A`/`-U` are retained because the alignment of
the buffer must still be stated for the numbers above to mean anything.

## Verified: `fsync()` on BFS never flushes the device cache — and on EBS that is free

Confirmed by reading the tree, not taken on trust:

- `bfs_fsync(volume, node, bool dataOnly)` → `return inode->Sync();` — **`dataOnly`
  is accepted and ignored**, so `fsync` and `fdatasync` are the same call.
- `Inode::Sync()` → `return file_cache_sync(FileCache());` and returns.
- `common_sync()` in `vfs.cpp`, which is what `fsync(2)` reaches, calls
  `FS_CALL(vnode, fsync, dataOnly)` **and nothing else**.
- The only `B_FLUSH_DRIVE_CACHE` on this path in `vfs.cpp` is at line 8203, inside
  the whole-mount `fs_sync` — i.e. `sync()`, not `fsync()`.

So `fsync()` pushes dirty pages to the driver but never asks the device to commit
its write cache. **On this hardware that costs nothing**, and the reason is
checkable rather than assumed: Linux on the same volume reports

```
/sys/block/nvme1n1/queue/write_cache : write through
/sys/block/nvme1n1/queue/fua         : 0
```

`write_cache = write through` is what Linux sets when the controller advertises
**no volatile write cache** (`VWC = 0`). An EBS write is durable once
acknowledged, so `B_FLUSH_DRIVE_CACHE` is a no-op here and its absence cannot
lose data on Graviton.

It is still a real portability gap — the same code on a device with a real
volatile cache would silently not be durable — and `dataOnly` being ignored means
`fdatasync` does more work than asked. Recorded as correctness, not as a
Graviton risk. The metadata half of the question (whether the inode that makes
the data reachable is committed) is tested separately below.

## Result 4: through BFS — reads are free, writes are where the problems are

The scratch volume was then formatted `mkfs -t bfs -o 'block_size 4096'` (BFS
defaults to **2048**, which would make every metadata write a sub-page write) and
a 40 GiB file created on it — larger than the machine's 31.5 GiB of RAM, so the
page cache cannot hold the working set. 20 s timed cells, 256 KiB blocks.

### Reads through the file system cost essentially nothing

| threads | BFS file, `-D` uncached | raw device | overhead |
|---|---|---|---|
| 1 | 173.24 MiB/s | 173.5 | 0.1% |
| 4 | 685.32 | 685.5 | 0.0% |
| 16 | 1055.89 | 1072.5 | 1.5% |

**BFS adds no measurable read cost.** Whatever else is true, the read path from
`pread` through the file cache, BFS, `file_map_translate`, devfs and the driver is
not where anything is being lost.

### The page cache makes large sequential reads *slower*

| | MiB/s |
|---|---|
| read, cache in use | 123.49, 123.32 |
| read, `-D` (cache bypassed) | 173.23, 173.16 |

**Going through the page cache costs 29%.** That is not a paradox: the file cache
has **no read-ahead** — `read_into_cache` fetches exactly the requested range and
nothing beyond, and the only prefetch facility in the tree is
`cache_prefetch_vnode`, used at boot and by mmap fault-around. So the cache adds an
allocation and a memcpy per block and contributes no lookahead, and on a working
set larger than RAM it cannot amortise that with hits either. Read-ahead in the
file cache is the most promising unexploited win found in this exercise, and it is
a kernel change rather than a module one.

### Buffered writes are irreproducible and get *slower* with concurrency

> **RETRACTED.** Both halves of this subsection are wrong, and the reason is worth
> more than the claim was: the write path degrades over the life of a boot, and this
> sweep ran its thread counts in ascending order, so degradation over *time* was
> measured and reported as an effect of *concurrency*. See
> "RETRACTED and replaced" below for the corrected numbers, and
> "The real finding" for what was actually going on -- which is worse.

Identical repetitions of the identical cell:

| cell | rep 1 | rep 2 | spread |
|---|---|---|---|
| buffered write, no fsync | **324.11 MiB/s** | **73.24 MiB/s** | **4.4×** |
| buffered write + fsync | 153.67 | 57.11 | 2.7× |
| uncached write + fsync | 81.76 | 81.81 | **1.001×** |

The uncached path is reproducible to one part in a thousand. The buffered path
varies by a factor of 4.4 between two runs of the same command on the same file.
And it anti-scales:

| threads | buffered write + fsync | uncached write + fsync |
|---|---|---|
| 1 | **114.77 MiB/s** | 81.81 |
| 4 | **80.01** | 109.65 |
| 16 | **67.23** | 192.44 |

**Adding writer threads makes buffered writes monotonically slower**, 115 → 67
MiB/s, while the uncached path speeds up. Buffered writes do not do their own
I/O: they dirty pages and the *page writer* writes them back, and there is one
page-writer thread per disk device. More writers therefore add contention for a
single flusher and nothing else. This is consistent with the writer's quota
machinery being involved — `fLastAveragePageWriteDuration` is the last sample
rather than an average, it is only updated on rounds of ≥8 pages with no
failures, and `WaitIfOverQuota` is entered with `flags = 0`, so a writer waits
indefinitely. Bimodal, irreproducible sustained-write numbers are exactly the
shape that predicts.

**Unmeasured:** that the single page-writer thread and the quota heuristic are
*the* cause is inferred from the code and from the anti-scaling, not confirmed by
instrumenting the writer. Someone should confirm it before changing the heuristic.

### Uncached writes through BFS reach only a fifth of the raw device

| threads | BFS file `-D` | raw device | gap |
|---|---|---|---|
| 1 | 81.81 MiB/s | 81.9 | — |
| 4 | 109.65 | 326.2 | 3.0× |
| 16 | 192.44 | 1020.0 | **5.3×** |

At one thread BFS matches the raw device exactly. By 16 threads it delivers
192 MiB/s where the device does 1020. Writes through a file system serialise on
something the raw path does not have — the obvious candidate being BFS's single
journal, which commits a transaction per allocation and calls `FlushDevice()`
(a whole `block_cache_sync`) plus `B_FLUSH_DRIVE_CACHE` per commit. **Not
attributed by measurement**; it is the next thing to instrument.

### Extending a file is far more expensive than rewriting it

Creating the 40 GiB file averaged **20.7 MiB/s** over 660 s, against **81.8 MiB/s**
for rewriting the same region afterwards at the same depth — a 4× difference
between allocating blocks and writing blocks. Every 1 MiB extension takes BFS
through a block-map update and a journal transaction. Worth knowing before
concluding anything from a benchmark that creates its own file.

### What fsync actually costs

Writing 64 MiB buffered and then calling `fsync` once: the write reports
**4840–4897 MiB/s** and the `fsync` takes **0.81–0.84 s**, which is **98.5% of the
run**. The 4.9 GiB/s figure is memory bandwidth and nothing else. Any buffered
write benchmark that does not fsync is measuring RAM, and this is what that error
looks like when quantified.

## Result 5: the write path is durable across a hard power loss

The question that matters more than any rate, given that this project's worst bug
was silent data loss on this exact path. `fsync()` never issues
`B_FLUSH_DRIVE_CACHE` (verified above), so the question is empirical.

Method: two 64 MiB files written to BFS with `-P`, so every block carries a magic
and its own absolute offset and can be classified afterwards as ok / zero / stale
/ corrupt.

- `nosync` — buffered, **no `fsync` at all**
- `fsynced` — buffered + `fsync`
- **no `sync()` was issued for either**, and then the instance was
  `stop-instances --force`'d **5 seconds** after the writes completed.

The force stop is a genuine power-loss test rather than a shutdown: DeBeOS does
not act on the inbound ACPI request, so nothing in the guest gets a chance to
flush. 5 seconds is also a deliberately tight margin against the page writer's
3-second flush interval.

Result after stop, start, and remount:

| | size | sha256 vs pre-boot | blocks bad |
|---|---|---|---|
| `nosync` | 67,108,864 ✓ | **identical** | **0 zero, 0 stale, 0 corrupt** |
| `fsynced` | 67,108,864 ✓ | **identical** | **0 zero, 0 stale, 0 corrupt** |

The 40 GiB `tf` file also came back at full size, and BFS remounted with no
journal replay error. The checksums are `sha256sum` on the node, so the verdict
does not rest on the same tool that wrote the data.

**Conclusion: the write path is durable on EBS — and that is a property of EBS,
not evidence that `fsync` is correct.**

Both halves of that sentence are load-bearing and must travel together.

What was demonstrated: the 3-second periodic flush fired and got un-`fsync`ed data
to the device inside a 5-second window. That is a direct regression test for the
page-writer bug — the failure that lost an sshd host key would have appeared here
as `zero` blocks, and there are none — and the metadata making the data reachable
was committed alongside it.

What was **not** demonstrated: that `fsync()` is a durability barrier. It is not.
`fsync()` never issues `B_FLUSH_DRIVE_CACHE` (traced above), so it returns once the
data has been handed to the driver, not once the device has committed it. The only
reason that is safe here is that this controller advertises **no volatile write
cache** — Linux on the same volume reports `write_cache: write through` and
`fua: 0`, so an EBS write is durable on acknowledgement and a flush would be a
no-op. **On any device with a volatile write cache the identical code would
acknowledge an `fsync` for data that a power loss then destroys.**

So this result licenses "DeBeOS does not lose data on Graviton/EBS". It does not
license "DeBeOS's `fsync` is correct", and it must not be quoted as the latter.
The gap is a genuine portability defect that happens to be free on the only
hardware this project targets. Also tested once, at one file size, and it says
nothing about a sub-3-second window.

## RETRACTED and replaced: the buffered-write "irreproducibility" and "anti-scaling"

Two claims in Result 4 above were investigated with progress tracing and **do not
survive**. They are left in place rather than deleted, per this document's
convention, because how they were wrong is more useful than the claims were.

### What was claimed

- Buffered writes vary **4.4×** between identical repetitions (324.11 vs 73.24 MiB/s).
- Buffered writes **anti-scale**: 114.77 → 80.01 → 67.23 MiB/s for t=1 → 4 → 16.

### What is actually true

**Neither is a concurrency or variance effect. Both are the same underlying thing:
the write path degrades monotonically over the life of a boot, and my sweep ran
its thread counts in ascending order.** The "anti-scaling" is the degradation
measured against time and mislabelled as concurrency — a direct violation of this
document's own interleaving rule, committed while writing the rule down.

Ten identical 20 s buffered writes at depth 1, on a 4 GiB file:

| rep | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| MiB/s | **430.2** | 202.6 | 201.6 | 202.3 | 195.5 | 197.3 | 187.9 | 205.0 | 193.8 | 198.5 |

Repetition 1 is a first-run effect — an empty cache absorbing writes at memory
speed. Repetitions 2–10 agree to **±5%**. There is no 4.4× variance.

And with 1 s progress tracing, buffered writes **scale up**, weakly:

| depth | throughput | p50 | p99 | max |
|---|---|---|---|---|
| 1 | 177.7 MiB/s | 658 µs | 4008 µs | 23,014 µs |
| 4 | 219.1 MiB/s | 4339 µs | 10,811 µs | 32,266 µs |
| 16 | **281.6 MiB/s** | 13,182 µs | **61,331 µs** | **174,510 µs** |

`NO PROGRESS` intervals across 45 s at each of the three depths, plus the control:
**zero**. No stall, no lost wakeup, no wedge at this load.

The buffered path is also *faster* than the uncached path at depth 1 — 177.7 vs
81.8 MiB/s — because the page writer batches `kNumPages = 256` pages (1 MiB) per
round instead of the caller's 256 KiB per operation.

### What the page writer is and is not responsible for

The negative control settles this. Uncached write, depth 16, same file, same
everything: **199.3 MiB/s, p99 34,367 µs, max 43,429 µs.**

- The **~200 MiB/s ceiling on BFS writes is present in the uncached path too**, and
  the uncached path never touches the page writer's quota. So the ceiling is
  **BFS**, not the page writer. The earlier attribution ("suspect the single
  journal") stands; the "page writer is the prime suspect" framing does not.
- What the page writer *does* add is **tail latency**: p99 61 ms against 34 ms, and
  max 175 ms against 43 ms. That is back-pressure, and it is consistent with
  `WaitIfOverQuota` being entered with `flags = 0` — see below, where it stops
  being a tail and becomes a liveness bug.

## The real finding: sustained write load degrades, and starves userland

> **PARTIALLY CORRECTED.** The degradation and the multi-second write latencies
> below are measured and stand. The word "wedge" and the claim of no recovery do
> not: reproducing it three more times showed the machine recovers once the write
> load stops. See "CORRECTED: it is unbounded starvation, not a permanent wedge".

This replaces the retracted claims and is more serious than either.

### Progressive degradation

After ~48 GiB had been written to the volume during one boot, the *same cells* that
had measured 178–282 MiB/s measured **18–80 MiB/s**, with p99 in the hundreds of
milliseconds and single `pwrite` calls taking up to **1.53 seconds**:

| cell | earlier this boot | after 48 GiB written | max latency |
|---|---|---|---|
| buffered write, depth 1 | 177.7 MiB/s | **18.7 MiB/s** | 376,875 µs |
| buffered write, depth 4 | 219.1 MiB/s | **20.2 MiB/s** | 356,439 µs |
| buffered write, depth 16 | 281.6 MiB/s | **79.6 MiB/s** | 1,004,289 µs |
| worst single write observed | — | — | **1,527,413 µs (1.53 s)** |

Interleaved between a 4 GiB file (fits in the 31.5 GiB of RAM) and a 45 GiB file
(cannot), the two arms were **indistinguishable** — 18.66 vs 18.72 MiB/s at depth 1.
So this is **not** a working-set-exceeds-RAM effect either; the whole write path had
degraded regardless of which file was touched. That was the hypothesis this
experiment was built to test, and it is disproven.

### Then it wedged

Attempting to measure the degraded state further, the node stopped responding
entirely. The state is unambiguous and was captured:

| probe | result |
|---|---|
| EC2 instance / system status | **ok / ok, running** |
| ICMP ping | **4/4, 0% loss, 0.22 ms** |
| TCP connect to :22 | **accepted** |
| SSH banner | **never arrives** |
| recovery over 5 retries / ~2 min | **none** |
| console: panic, KDL, low-resource message | **none — last line is ordinary boot chatter** |

So: **the kernel is alive, interrupts work, the network stack and the ENA driver
work, sshd's listening socket still accepts — and no userland process can make
progress.** Ping is answered in the interrupt/kernel path; anything that has to
touch a file does not return. That is a liveness bug, not a slow benchmark, and it
is the behaviour `WaitIfOverQuota(additionalPages, 0, B_CAN_INTERRUPT)` permits:

```c
// file_cache.cpp:840
status_t status = modifiedQueue->WaitIfOverQuota(toModified, 0, B_CAN_INTERRUPT);
```

`timeout` is 0 and **no timeout flag is set**, so `WaitIfOverQuota` skips its
relative-to-absolute conversion and calls `waitEntry.Wait(B_CAN_INTERRUPT, 0)` —
an indefinite wait. Any thread that dirties pages while the modified queue is over
quota blocks until the page writer says otherwise, forever if it never does. The
quota it is tested against is derived from `fLastAveragePageWriteDuration`, which
despite the name is **the most recent sample, not an average**:

```c
if (numPages >= 8 || fLastAveragePageWriteDuration == 0)
    fLastAveragePageWriteDuration = (system_time() - runStart) / numPages;
```

One slow round — and 1.5-second writes were being observed — raises the estimate,
which makes `IsOverQuota()` true at a much smaller `fCount`, which blocks more
writers, which is self-reinforcing. That is a plausible ratchet and it matches the
observed monotonic, non-recovering degradation.

**Confidence, stated honestly.** The wedge is **reproduced once**. The mechanism
above is the best fit to the evidence but is **not proven**: nothing was
instrumented inside the kernel, and the alternative that BFS's journal or block
allocator is the thing that stopped making progress is not excluded — the ~200
MiB/s ceiling is already known to be BFS's rather than the page writer's, so BFS is
a live suspect for the wedge too. Distinguishing them needs a KDL session on a
wedged node (`bt` on a blocked thread would settle it in one line) or kernel
counters, and both need a bake.

### Reproduction recipe

On a `c7g.4xlarge` from the canonical AMI, with a 100 GiB gp3 (16,000 IOPS,
1,000 MiB/s) scratch volume:

```bash
mkfs -q -t bfs -o 'block_size 4096' /dev/disk/nvme/1/raw Scratch
mount -t bfs /dev/disk/nvme/1/raw /pw
disktput -f /pw/tf  -m seqwrite -b 1M -t 16 -T 150 -D -s 4G      # 4 GiB file
disktput -f /pw/big -m seqwrite -b 1M -t 16 -T 400 -D -s 48G     # 48 GiB file
# then repeated buffered writes with -S; throughput falls from ~180 to ~19 MiB/s
# and the node stops answering ssh while still answering ping
disktput -f /pw/tf -m seqwrite -b 256K -t 1 -T 20 -s 4G -S -i 1
```

`-i 1` matters: without progress tracing this presents as "the benchmark is slow"
rather than as "the machine has stopped".

### Why this changes the priority

A path that goes 10× slower and then stops answering, with no panic and no log
line, on ordinary buffered file writes, is worse than any throughput number in this
document. It is also the same machinery that produced this project's worst bug. It
should be reproduced deterministically and then fixed — and the cheapest first fix
is bounding that wait: a timeout on `WaitIfOverQuota` would convert an indefinite
hang into a slow write, which is survivable, without needing the quota heuristic to
be right.


## CORRECTED: it is unbounded starvation, not a permanent wedge

The section above calls this a wedge and says "no recovery". **That overstates it,
and the correction came from reproducing it rather than from care.** Recorded here
in full because the difference matters for how alarming the defect is and for what
the fix has to do.

### What reproducing it three more times showed

The recipe was made cheaper (below), and run on two nodes differing only in volume
provisioning. Both stopped answering ssh, exactly as before — ping fine, `:22`
accepting, no banner. But when checked again after the harness had exited,
**both were responsive**:

| node | wedged at | later check |
|---|---|---|
| slow volume (125 MiB/s) | round 4, **12 GiB** written | **recovered** |
| fast volume (1000 MiB/s) | round 6, **20 GiB** written | **recovered** |

The harness classified them as wedged because its ssh call timed out at 400 s and
the node still answered ping. It never retried ssh later. So "no recovery" was an
artifact of not waiting long enough — the same class of error as the original
ascending-order sweep: **the instrument, not the system.**

The original occurrence was probed for only about two minutes before the instance
was terminated, so it was **never established as permanent either.**

### What is actually true

A deliberate sustained-load test — eight 2 GiB buffered writes queued back to back
on the node, so the load does not stop when the harness stops — left the machine
**unresponsive to ssh continuously for 1,663 seconds — 27.7 minutes — sampled every
23 seconds, and still unresponsive at 30 minutes when observation ended.** ICMP was
`ok` and TCP `:22` accepted at *every one of those samples*. The load was 16 GiB, in
eight 2 GiB buffered writes, on a 125 MiB/s volume.

```
      811s 05:17:43     ok     ok STALLED
      ...  (every 23 s, unbroken)
     1663s 05:31:55     ok     ok STALLED
```

So the correct statement, and no more than this: **under sustained buffered write
load userland stops making progress for at least tens of minutes.** A machine that
will not answer ssh for half an hour because something is writing files is broken
however it is labelled.

**Whether it always recovers is genuinely unresolved.** Two instances were
responsive when re-checked after their harness had exited. The third was starved
for 30 minutes and had not recovered when observation ended — and because its load
was eight cells queued on the node, "the load was still running" and "it does not
recover" cannot be told apart from outside. So:

- "permanent wedge" is **not** established (and was my original overstatement);
- "recovers when the load stops" is **not** established either (and was my
  correction overshooting in the other direction);
- what is established is **≥27.7 minutes of continuous, unbroken starvation**,
  measured, with the kernel demonstrably alive throughout.

Settling recovery needs the load to be bounded and instrumented so that "still
writing" is distinguishable from "stuck" — which the bounded-wait fix and its
timeout counters will do directly, since a machine that no longer starves but logs
thousands of quota timeouts has answered the question.

The directly measured single-operation latencies are not in doubt, and stand on
their own: individual buffered `pwrite` calls of 256 KiB taking **8.9 s, 13.9 s,
15.7 s and 26.9 s**, from a tool that timed each one.

### New evidence for the ratchet, and what it does not settle

Two things from these runs point at a global, non-recovering estimate rather than
at per-file or per-transaction filesystem cost:

1. **The slower volume degraded sooner** — 12 GiB against 20 GiB. A quota derived
   from a per-page write-duration estimate should trip earlier on a device where
   each page costs more, which is what happened.
2. **The non-allocating arm was healthy until the allocating arm ran once, and
   never recovered afterwards.** On the fast node, rewriting a fixed pre-existing
   2 GiB file measured **147.23 MiB/s with a 955 µs worst case** in round 1. After
   one round of writing a *new* file, that same rewrite cell never again exceeded
   **78 MiB/s**, and picked up multi-second worst cases:

   | round | rewrite (no allocation) | allocate a new file |
   |---|---|---|
   | 1 | **147.23 MiB/s**, max 955 µs | 84.28 MiB/s, max 13.99 s |
   | 2 | 78.24, max 0.12 s | 95.66, max 10.48 s |
   | 3 | 77.13, max 8.95 s | 94.75, max 10.82 s |
   | 4 | 77.89, max 8.95 s | 87.58, max 13.19 s |
   | 5 | 77.18, max 0.11 s | 93.48, max 11.88 s |

   Allocation is what first produces multi-second stalls, but the damage is not
   confined to the allocating path — it degrades a path that allocates nothing, by
   47%, permanently. `fLastAveragePageWriteDuration` is a single sample shared by
   every writer on the device, and `sGlobalEstimatedWriteDuration` is shared across
   devices, so one arm poisoning the other is exactly what that design permits.

**What it does not settle:** both arms write to the same volume and the same BFS
filesystem, so BFS-global state — free-space layout, journal behaviour as the
volume fills — also changed between round 1 and round 2. That is a live alternative
explanation for the rewrite arm's degradation and this experiment cannot exclude
it. `bt` on a stalled thread still decides it, which is why the arm64 `bt` fix is
in the bake.

### The cheap reproduction

Roughly 4× cheaper than the original 48 GiB, and it stalls on the first arm that
allocates:

```bash
# c7g.4xlarge, canonical AMI, plus a gp3 scratch volume.
# Use a SLOW volume -- 125 MiB/s / 3000 IOPS -- it reaches the stall in 12 GiB
# rather than 20, which is itself evidence for the estimate-driven quota.
mkfs -q -t bfs -o 'block_size 4096' /dev/disk/nvme/1/raw W
mount -t bfs /dev/disk/nvme/1/raw /w
disktput -f /w/fixed -m seqwrite -b 1M -t 8 -T 120 -D -s 2G     # rewrite target

# then alternate, 2 GiB per cell, and watch max latency in the -J line:
disktput -f /w/fixed  -m seqwrite -b 256K -t 4 -n 2G -s 2G -S -J   # no allocation
disktput -f /w/new-$n -m seqwrite -b 256K -t 4 -n 2G -s 2G -S -J   # allocates
```

Interleave the two arms — the ascending-order mistake above is easy to repeat here.
To hold the machine in the stalled state (for a KDL capture), queue several
allocating cells back to back on the node so the load does not stop when the
harness does; it recovers within a minute or two of the load ending.


## What the bake has to carry, and why none of it can be dropped in

Everything below is a kernel or boot-critical-module change. Per the hot-swap
finding above, **none of it can be installed on a running node** — and the
failure is silent, so a test that skipped the bake would produce numbers from the
old code.

| commit | what | why it must be compiled in |
|---|---|---|
| `nvme: submit a chopped request as a batch...` | the 3.8× fix for large requests | `nvme_disk` serves the root filesystem; the non-packaged override resolves under `/boot/home`, which needs `nvme_disk` to mount |
| `vm: bound the modified-page quota wait...` | the liveness fix + its instrumentation + the `page_writer_quota` KDL command | `vm_page_writer.cpp`, `file_cache.cpp` — kernel proper |
| `arm64: make KDL 'bt <thread>' actually trace that thread` | **prerequisite for the diagnosis** | `arch_debug.cpp` — kernel proper |
| `disktput: trace progress over time...` | `-i` interval tracing and `-m verify` | userland, so it *can* be pushed by hand, but the image's own `disktput` needs it to be in the tree |
| `arm64: fix serial getchar, and give KDL a way in...` | makes KDL usable on arm64 at all | **found after the bake started, so it is NOT in it** — see "The capture plan, revised" |

### The KDL prerequisites — the two checkable ones pass, the guest side did not

Interactive KDL on an EC2 instance needs serial **input**, not just the output
that `get-console-output` returns. The AWS side and the UART driver are fine:

- **EC2 Serial Console access is enabled** for account 668984504585
  (`get-serial-console-access-status` → `True`), so
  `send-serial-console-ssh-public-key` plus ssh to the serial-console endpoint
  gives a bidirectional console.
- **The UART itself can read.** `DebugUART::GetChar()` works in both blocking and
  non-blocking modes.

But the kernel above it could not, and the `try_getchar` TODO noted here in an
earlier revision turned out to be the whole problem rather than a curiosity: it
made the function incapable of returning -1, so KDL's `kgetc()` read 0xFF forever
instead of waiting. And nothing on arm64 could enter KDL on demand in the first
place. Both are fixed, neither is in the current bake. See "The capture plan,
revised".

### Why the `bt` fix is on the critical path and not a side quest

The plan for the wedge is "get `bt` on a wedged thread; several threads parked in
`WaitIfOverQuota` says one thing, several parked in a BFS transaction says
another". That plan could not have worked on this architecture. arm64's
`stack_trace()` advertises `[ <thread id> ]`, parses `argv` only far enough to
count arguments, and then unconditionally traces the calling thread — so
`bt <blocked-thread>` would have printed the debugger's own stack, plausibly and
wrongly, and it would have been read as the answer. It is fixed here, with the
frame-pointer index verified against the `stp` ordering in `arch_asm.S` rather
than inferred from the comment on the struct.


## ANSWERED: the page writer's quota is the mechanism, and it starves across devices

Run on `ami-02d5e711d25cc2d49` (tagged `feature=storage-quota-bound`,
`branch-head=e1e5a0f531`), `c7g.4xlarge`, 100 GiB gp3 scratch at **125 MiB/s**, the
identical 16 GiB load that previously starved a machine for 27.7+ minutes: eight
2 GiB buffered writes with `fsync`, queued on the node so the load outlives the
harness.

The counters settle the fork. **Not BFS. The quota.**

```
page writer: quota wait timed out after 5000006 us, proceeding over quota
  (waits 4102, timeouts 16, longest 5000007 us, per-page estimate 28 us, queue 216320 pages)
page writer: quota wait timed out after 5000004 us, proceeding over quota
  (waits 4224, timeouts 19, longest 5000007 us, per-page estimate 883 us, queue 0 pages)
```

19 timeouts against **4,224 waits** — so 99.5% of waits completed normally and the
bound is a safety valve rather than the common path. Every `longest` is
**5,000,00x µs**, i.e. exactly the 5 s bound, which is the direct evidence that
these waits were previously *unbounded*: the bound is the only thing terminating
them.

### The finding that explains sshd: it starves across devices

**15 of the 19 timeouts report `queue 0 pages`.** A thread was blocked over quota
while the queue it was writing to was *empty*. That cannot be a BFS journal or an
allocation cost — there was nothing queued on that device at all.

`IsOverQuota()` checks two things, and the second is global:

```c
if ((estimatedWriteDuration + additionalPagesDuration) > PAGES_FLUSH_DURATION_LOCAL_QUOTA)
    return true;
return ((atomic_get64(&sGlobalEstimatedWriteDuration) + additionalPagesDuration)
    > PAGES_FLUSH_DURATION_GLOBAL_QUOTA);
```

`sGlobalEstimatedWriteDuration` is summed across **every** `ModifiedPageQueue`, and
there is one per disk device. The numbers from the two devices in this machine:

| device | queue depth | per-page estimate | estimated drain |
|---|---|---|---|
| scratch (being hammered) | **221,186 pages** (864 MiB) | 31 µs | **6.86 s** |
| root (idle) | **0 pages** | 883 µs | 0 s |

6.86 s from the scratch device alone exceeds both the 3 s local quota *and* the 5 s
`PAGES_FLUSH_DURATION_GLOBAL_QUOTA`. So a writer to the **idle root disk** fails the
global check and blocks — and before the bound, blocked indefinitely.

**That is precisely why sshd starved.** sshd writes to the root filesystem: logs,
`utmp`, the session. Its disk was idle and its queue empty. It was stopped by a
backlog on a completely different device. Any process touching any file on any
disk is throttled by the busiest disk in the machine.

### The stale-sample defect, confirmed by measurement rather than by reading

`fLastAveragePageWriteDuration` is named like a mean and is the **most recent
sample**. The two devices above show what that costs: the busy device measures
**27–31 µs** per page; the idle one reports **883 µs**, a 30× higher figure that is
simply old. It never refreshes, because the writer only updates it on rounds of
≥8 pages and an idle device never has such a round.

The consequence is a threshold that is wrong in the dangerous direction. At
883 µs/page the local quota trips once that device holds
`3,000,000 / 883` ≈ **3,397 pages = 13 MiB** of dirty data. Thirteen mebibytes, on
a machine with 31.5 GiB of RAM, because of one stale sample.

### The fix works, and both halves were recorded

- **Liveness:** ssh answered at **0 of 34 samples stalled**, polled every 15 s
  across 8.4 minutes of the load, and all eight files reached exactly
  2,147,483,648 bytes — **16 GiB written and completed**. Against the pre-fix
  kernel's 27.7 minutes of *unbroken* starvation on the identical load, which never
  completed within observation. Same load, same volume provisioning, same instance
  type, same 125 MiB/s scratch: the kernel is the only variable.

  | | pre-fix kernel | this image |
  |---|---|---|
  | ssh samples stalled | **every one**, 0 s → 1663 s | **0 of 34** |
  | 16 GiB load | never completed under observation | **completed** |
  | worst single write | 26.9 s | quota waits capped at 5.0 s |

  The load was demonstrably live during that polling window and not merely
  finished early: the quota counters advanced from `waits 4129 / timeouts 18` to
  `waits 4567 / timeouts 20` across it.

  *Instrument note, in the spirit of the rest of this document:* the poller also
  printed a "GiB written" column which read 0 at every sample. That was a broken
  instrument, not a stalled write — `bc` does not exist on a DeBeOS image, so the
  `paste -sd+ | bc` pipeline fell through to its `|| echo 0`. The advancing quota
  counters and the final file sizes are what establish the load ran; the column
  established nothing. Third empty-result-mistaken-for-absence in this
  investigation, which is why the rule below is about asking what a positive row
  would look like.
- **The decay is still visible**, which is what the instrumentation was for:
  4,224 waits, 19 timeouts, `longest` pinned at the bound, and the per-device
  estimates and queue depths printed alongside. A machine that stops hanging but
  logs these is still reporting that write-back cannot keep up.

### What to fix next, now that the mechanism is known

The bound is a liveness guarantee, not a cure. In order:

1. ~~Make the global quota not couple idle devices to busy ones.~~ **Done and
   hardware-verified** — 0 of 2 timeouts on an idle queue, against 15 of 19 before.
2. ~~Make the estimate an actual average, and decay it.~~ **Done**; verified
   indirectly so far, and `page_writer_quota` now dumps every queue so the
   promotion image can measure it directly.
3. Still open: revisit the quota constants. They cannot be judged until the two
   fixes above are measured, because until now the estimate feeding them was
   unreliable and the aggregation was wrong.

### Method note: `get-console-output` needs `--latest`

Without it the API returns an **empty body** for a running Nitro instance, and every
`grep` against it counts zero. The first attempt at this measurement reported "the
quota never fires" for exactly that reason. What caught it was a **positive
control** — grepping for `nvme_disk`, a string known to be present — before
believing an empty result. With `--latest`: 48,943 bytes and 16 matches. The
procedure earlier in this document omitted `--latest` and has been corrected.


## The fixes, and how to tell whether they worked

Both are committed in `vm: stop the modified-page quota starving writers to idle
disks`, both are **unverified pending a bake**, and they are deliberately in this
order: the coupling is what made a shell unusable, and the estimate cannot be
judged while the thing it feeds is aggregating the wrong quantity.

### Fix 1 — the global bound counts pages, not summed time

`sGlobalEstimatedWriteDuration` added every device's estimated drain and compared
the total to one 5 s deadline. Durations do not sum across devices: each
`ModifiedPageQueue` has its own page writer and its own disk, and they drain in
**parallel**. 6.86 s on one disk plus 0 s on another means the system needs
6.86 s, not 6.86 s that a writer to the *second* disk must sit through.

The global limit is now **1/8th of RAM may be dirty across all devices** — pages
do genuinely sum, because they all occupy the same memory. The per-device time
quota is untouched and remains the flush-latency bound its name always described.
The summed duration is still maintained and still printed; it just no longer
decides anything.

Deliberately generous, and that is the point: the defect it replaces fired when
memory was in no danger at all — 32.6 GB free of 33.8. A bound that only speaks up
when memory really is at risk is the correct replacement for one that spoke up
about the wrong quantity.

### Fix 2 — the estimate is an average, and it decays

`fAveragePageWriteDuration` was the most recent sample despite its name. Now an
exponentially weighted moving average (α = 1/4), so one slow round cannot set the
threshold, and **halved on every idle round**, so a stale figure fades instead of
freezing. It stops at 1 rather than 0, because a zero estimate makes
`IsOverQuota()` always false and disarms back-pressure entirely — which is why the
original code special-cased zero.

**Stated trade-off:** after decaying low, the average climbs back over several
rounds rather than snapping to the first large sample, so back-pressure is weak at
the start of a burst. That is the safe direction — the defect was over-throttling
to the point of hanging the machine — and snapping upward would reintroduce
exactly the single-sample sensitivity being fixed. Memory remains protected by the
global page bound in the meantime.

### Pre-bake prediction, stated so the measurement can falsify it

Working the fix through the numbers actually observed, before booting it. RAM was
33,784,201,216 bytes = **8,248,096 pages**, so the new global limit is
`8,248,096 >> 3` = **1,031,012 pages (3.93 GiB)**. Peak observed global dirty was
**221,186 pages (864 MiB) — 21% of the new limit**, which is the quantitative form
of "it fired while 32.6 GB of 33.8 was free".

**Would the sshd writer still block?** Root disk, empty queue, `additionalPages ≤ 32`:

| check | arithmetic | result |
|---|---|---|
| local (time) | `0 × 883 + 883 × 32` = 28,256 µs vs 3,000,000 | under |
| global (pages) | `221,218` vs `1,031,012` pages | under |

**Not throttled.** Previously the global summed duration was 6.86 s against a 5 s
deadline, so it blocked — indefinitely, before the wait was bounded.

**Is back-pressure preserved on the busy disk?** Scratch, 221,186 pages at 31 µs:

| check | arithmetic | result |
|---|---|---|
| local (time) | `221,186 × 31 + 31 × 32` = 6,857,758 µs vs 3,000,000 | **over — still throttled** |

Both halves are what they should be: the writer that had no business waiting does
not wait, and the writer that caused the backlog still does. If the boot shows
`waits == 0`, this arithmetic is wrong somewhere and the throttle has been removed
rather than retargeted.


**Precondition checked on the quota change itself**, applying the rule above rather
than trusting that the same mistake was not made twice. `IsOverQuota()` now calls
`vm_page_num_pages()`, which returns `sNumPages - sNonExistingPages`; if those were
still zero the global limit would be zero and *every* writer would be permanently
over quota — a worse regression than the one being fixed, and silent. They are set
in `vm_page_init_num_pages()`, reached from `vm_init()` at `main.cpp:156`, whereas
the earliest `StartWriter()` is in `vm_page_init_post_thread()` at `main.cpp:222`,
and the per-disk queues are created later still by `KDiskDevice`. So the value is
always initialised before any page writer exists to read it.


### What success looks like, and what would mean I broke back-pressure

The failure mode of a fix like this is removing the throttle rather than fixing
it, so both directions have to be checked. Run the same 16 GiB load on a 125 MiB/s
volume:

| signal | fixed | broke back-pressure | not fixed |
|---|---|---|---|
| timeouts with `queue 0 pages` | **none** | none | present |
| `waits` | **still > 0** | **0** | > 0 |
| `timeouts` | **≈0** | 0 | > 0 |
| idle disk's per-page estimate | **~1–30 µs** | any | 883 µs |
| ssh during load | responsive | responsive | stalls |

`waits > 0` with `timeouts ≈ 0` is the target: writers to a genuinely backed-up
device still wait, and nobody waits the full 5 s. **`waits == 0` would mean the
quota never engages at all**, which is not a fix — it is the throttle deleted, and
it would show up later as unbounded dirty memory rather than as a hang.

Also worth recording: the global dirty count against its new 1/8-of-RAM limit,
from `page_writer_quota`, to confirm the replacement bound sits in a sane place
under real load rather than never engaging or engaging constantly.

### The boot panic: diagnosed, retracted, and un-retracted — the retraction was the error

I attributed the boot panic below to my own serial KDL listener —
`spawn_kernel_thread()` from `arch_debug_console_init_settings()` at
`main.cpp:168` against `thread_init()` at 212. A second bake appeared to disprove
it, so I retracted. **The retraction was wrong: the original diagnosis was right,
and the second bake had compiled the same source as the first.**

The resolved backtrace, from the kernel binary of that bake:

```
ffff0000000b2c4c -> spawn_kernel_thread
ffff00000017fb10 -> arch_debug_console_init_settings     <- the caller
ffff0000000da0cc -> debug_init_post_settings
ffff000000090b10 -> _start
```

And the faulting instruction pins the mechanism exactly: `bl team_get_kernel_team`
followed by `ldr w2, [x0, #48]`, with **`FAR=30`, and 0x30 = 48**. So
`team_get_kernel_team()` returned NULL — because the team structures do not exist
until `thread_init()` — and the load faulted at offset 48 of a null pointer. The
precondition was unmet in precisely the way predicted.

The reason the second bake looked identical is that it *was* identical: the branch
was baked from its pushed remote ref, which still pointed at the pre-rebase commit
without the fix. Same source in, same binary out.

| AMI | branch-head | boots? | contents |
|---|---|---|---|
| `ami-02d5e711d25cc2d49` | `e1e5a0f531` | **BOOTS** | my branch *before* the graviton merge |
| `ami-0fe2b76a049766507` | `8b674f3cae` | PANICS | graviton `d733822a15` + the quota fix |
| `ami-0a3c20e1085a75e16` | — | **PANICS identically** | graviton `5d97144c1a`, *including the boot fix* |

The panic is **byte-identical** in both failing images — same `FAR=30`, same
`ELR=ffff0000000b2c4c`, same frame addresses:

```
PANIC: unhandled pagefault! FAR=30 ELR=ffff0000000b2c4c ESR=96000004
... after arch_vm_translation_map_init_post_area
```

The two images have **different snapshots** (`snap-086021847de985b10` vs
`snap-0e9192e77c3744edf`), so the third really is a new build carrying the fix.
The fix changed nothing, therefore it was not the cause. That two different builds
produce identical addresses is itself consistent: my changes were in `debug.cpp`
and `arch_debug_console.cpp`, so anything linked before them keeps its address.

**Also checked and ruled out:** the alarming
`reserve_boot_loader_ranges(): Skipping range: 0xffffff0400000000, 32715571200`
(30.5 GiB) and `mark_page_range_in_use(0x0, 0x40000): start page is before free
list` appear **identically in the image that boots**. They are normal here and not
the cause. I had them as a hypothesis and they were wrong.

**What the evidence actually was.** Everything below in this subsection was
reasoned correctly from what I could see, and what I could see was a stale build.
The bisection table stands as a record of the reasoning, but its conclusion does
not: the cause is the serial listener's call site, not the graviton merge, and the
merge is exonerated.

**The fix is the fix**, and moving the listener into the generic debugger remains
the better design independently of that.

### The verification did not happen: the image did not boot

`ami-0fe2b76a049766507` panics during early VM setup:

```
PANIC: unhandled pagefault! FAR=30 ESR=96000004
... arch_vm_translation_map_init_post_area
```

Cause, and the resolved backtrace above confirms it. The serial KDL listener was
spawned from
`arch_debug_console_init_settings()`, which `main.cpp` reaches via
`debug_init_post_settings()` at line **168**. `thread_init()` is at line **212**.
So `spawn_kernel_thread()` ran 44 lines before the threading system existed and
faulted on an uninitialised structure. The previous image booted only because it
predated the listener; this was the first image to carry it.

Fixed by moving the listener to `debug_init_post_modules()` (line 369, after
`thread_init` at 212 and `vm_init_post_thread` at 222), and by moving it out of
arm64 into the **generic** debugger, where it belonged:
`arch_debug_serial_try_getchar()` is already architecture-neutral and nothing in
the polling loop is CPU-specific, so every architecture with a debug serial line
now gets on-demand KDL entry rather than only arm64.

**So no quota numbers yet.** `waits`, `timeouts`, the idle-disk per-page estimate
and the `queue 0 pages` check all require a booting image, and the pre-bake
prediction above stands untested.

Two things worth keeping from this:

- **"UNVERIFIED ON HARDWARE" was doing real work.** Every kernel commit on this
  branch carries that label, and this is why: the change compiled cleanly, was
  reviewed, was correct in its logic, and bricked the machine on a boot-ordering
  constraint that no build or reading of the diff would surface. A bake is not a
  formality between writing a kernel change and believing it.
- **The failure was in the same class as the one I keep finding in tooling** — a
  call that is only valid after some other subsystem exists, made before it does.
  It is worth noting that I found it in under ten minutes only because the console
  output was checked with `--latest` and read rather than assumed; the instinct that
  caught other people's empty results caught my own panic.


### The KDL cross-check is now available

With the serial/KDL-entry fix merged, the capture originally planned can finally
be done as a *cross-check* on the counters rather than as the primary evidence:

```
ssh -i k <instance-id>.port0@serial-console.ec2-instance-connect.us-west-2.aws
# type: kdl
  page_writer_quota          # queue depths, both bounds, wait statistics
  bt <thread-id>             # now traces that thread -- see the arm64 bt fix
  page_writer_quota_trace 0  # silence the log line if it is in the way
```

If any writer is still blocked, `bt` on it now says where. Under the fix the
expectation is that there is nothing to catch.

### Liveness evidence from the bounded wait, consolidated

Two independent pollers ran across the two 16 GiB loads on the bounded-wait image:
**93 ssh samples, 0 stalled** (59 in the first window at 20 s intervals, 34 in the
second at 15 s), against *every* sample stalled from 0 s to 1663 s on the pre-fix
kernel with the identical load. That is the bounded wait working as a liveness
backstop, and it stays in place after these two fixes precisely because it does not
depend on the diagnosis being right.


## VERIFIED on hardware: `ami-07c5e3b00b4dcee34`

Built from `9e3d6f942e` with a source precheck in the clone
(`arch_debug_console.cpp: 0`, `debug.cpp: 2`). `c7g.4xlarge`, 100 GiB gp3 scratch at
**125 MiB/s**, the identical 16 GiB load that previously starved a machine.

### Result 1 — it boots

| check | value |
|---|---|
| `PANIC` | **0** |
| `Kernel Debugging Land` | **0** |
| `spawn_kernel_thread` in a trace | **0** |
| `nvme_disk` *(positive control)* | 16 |
| `io batch size` | 2 |
| `bfs: mounted` | 1 |

First hardware evidence for the boot fix, and it retroactively validates the
cherry-pick sitting on `graviton`'s tip.

### Result 2 — liveness: the starvation is gone

14 consecutive samples of the ICMP → TCP → banner ladder, 15 s apart, **every one
`healthy`**: 0 STARVED, 0 no-ICMP. All eight files reached exactly 2,147,483,648
bytes, and the volume went to 16% used, so the 16 GiB landed on the scratch device
and not the 300 MiB root.

| | pre-fix kernel | this image |
|---|---|---|
| ssh samples starved | **every one**, 0 s → 1663 s | **0 of 14** |
| 16 GiB load | never completed under observation | **completed** |

### Result 3 — back-pressure survived, which was the falsifiable half

From KDL, `page_writer_quota`:

```
  global dirty limit      : 1031012 of 8248096 pages  (1/8 of RAM)
  quota waits             : 14684
  quota wait timeouts     : 2  (bound 5000000 us)
  total time waiting      : 447533488 us
  longest single wait     : 5000005 us
```

**`waits = 14684`.** The stated falsifier was `waits == 0` meaning the throttle had
been removed rather than retargeted; it did not fire. The quota engages ~14.7
thousand times and reaches the 5 s bound **twice**, with a mean wait of
**30.5 ms** (447.5 s over 14,684 waits). That is back-pressure working with a
safety valve, which is exactly the target shape.

### Result 4 — the cross-device coupling is gone

| | pre-fix | this image |
|---|---|---|
| timeouts reporting `queue 0 pages` | **15 of 19** | **0 of 2** |

Only two timeout lines exist in the whole run, and both are on a device with a real
backlog — 333,825 and 173,569 pages queued, at 30–31 µs per page:

```
(waits 1, timeouts 1, longest 5000005 us, per-page estimate 30 us, queue 333825 pages)
(waits 2, timeouts 2, longest 5000005 us, per-page estimate 31 us, queue 173569 pages)
```

Those are precisely the writers that *should* be throttled: 333,825 × 30 µs = 10.0 s
of estimated drain against a 3 s local quota. **No writer on an idle queue was
blocked at all**, which was the defect.

### Result 5 — the prediction was right to the page

Predicted before the boot: `8,248,096` pages of RAM, global limit
`8,248,096 >> 3` = **1,031,012 pages**. KDL reports
`global dirty limit : 1031012 of 8248096 pages`. Exact.

### What was *not* obtained, and why

**The idle-disk per-page estimate was not read directly.** `page_writer_quota` dumps
`vm_page_default_modified_queue()` only — the default queue, not the per-disk ones —
and that queue was unused this boot, so it reported `pages queued: 0` and
`per-page write estimate: 0 us`. The wait counters *are* static members and
therefore system-wide, which is why `waits`/`timeouts` are trustworthy; the
per-device estimates are not in that dump.

The evidence for fix 2 is therefore **indirect but strong**: zero timeouts on an
idle queue, where the pre-fix run had 15 of 19 driven by an idle disk stuck at
883 µs. The estimate no longer freezes high enough to throttle an idle device.
**Gap closed** in `vm: dump every modified page queue, not just the default one`:
the command now walks a registry of live queues, prints the per-device figures per
queue, and prints the shared counters once under a `system-wide:` heading so they
cannot be misread as per-device. The per-device estimate is therefore a direct
measurement on the promotion image rather than an inference.

### And KDL worked, for the first time on arm64

```
Thread 38 "serial debug listener" running on CPU 14
kdebug> page_writer_quota
```

The listener thread caught the `kdl` trigger, the typed commands were read
correctly rather than as a stream of `0xFF`, and `continue` resumed the machine
cleanly (ping and ssh both confirmed afterwards). The capability that was dead on
this architecture is the capability that produced the one number the syslog could
not — `waits` — because the log line only fires on a timeout and the fix reduced
timeouts to two.


## The capture plan, revised: the fork can be answered without KDL

Preparing the KDL capture turned up three more defects in the debugger path on
arm64 (see the `arm64: fix serial getchar...` commit). Their combined effect is
blunt:

**Interactive KDL does not work on arm64 today, and cannot work on the image
currently being baked.**

- There is **no way to enter KDL on demand at all**.
  `debug_emergency_key_pressed()` is called from exactly three places — x86's
  console interrupt handler, USB HID, and PS/2 — and arm64 has none of them. A
  headless Graviton instance has no keyboard: it logs a failed search for
  `bus_managers/ps2/v1` and has no USB HID. The debugger is reachable only by a
  panic, and the starvation does not panic.
- Even once inside, **typed commands would be garbage**.
  `arch_debug_serial_try_getchar()` could never return -1, because it returned a
  `char` and plain `char` is unsigned on AArch64, so `GetChar(false)`'s -1 became
  255. `kgetc()` tests `c >= 0`, so it accepted an endless stream of 0xFF instead
  of waiting for input.

Both are fixed, but the fixes were written **after** the bake started, so they
are not in it. Do not plan a KDL session against this image.

### What is in the bake, and why that is enough for the fork

The bounded quota wait and its counters **are** in the bake, and they answer the
question KDL was wanted for, without any interactivity — the output goes to the
serial console, which `get-console-output` returns.

| observation on the baked image | conclusion |
|---|---|
| starvation gone or greatly shortened, **and** `page writer: quota wait timed out ...` lines appear with `timeouts` climbing | **the page writer's quota is the mechanism.** Writers that used to block indefinitely now hit the 5 s bound and proceed |
| starvation persists essentially unchanged, **and no timeout lines appear at all** | **not the quota.** Threads are blocking somewhere else, and BFS's journal is the standing suspect |
| starvation persists **and** timeouts are logged | both are involved: the quota fires, but something else also blocks. The counters bound how much of it the quota owns |

That is the same fork — "several threads parked in `WaitIfOverQuota`" versus
"several parked in a BFS transaction" — decided by whether the wait is entered at
all, rather than by reading a stack. It is weaker evidence than a stack trace, in
that it says *whether* the quota path is implicated rather than showing the exact
call chain. It is also unavailable to misinterpretation in the way a silently
wrong `bt` would have been.

### Procedure for the baked image

```bash
# 1. Verify the image carries the changes BEFORE spending a boot on it.
#    The boot log stamps the driver; the quota lines only appear under load.
grep "io batch size" /var/log/syslog        # nvme batching present

# 2. Reproduce the starvation. Queue the cells ON THE NODE so the load
#    outlives the harness and the machine stays starved.
mkfs -q -t bfs -o 'block_size 4096' /dev/disk/nvme/1/raw W
mount -t bfs /dev/disk/nvme/1/raw /w
for i in $(seq 1 8); do
    disktput -f /w/new-$i -m seqwrite -b 256K -t 4 -n 2G -s 2G -S
done &

# 3. Watch from OUTSIDE, because ssh is what stops answering. The serial
#    console is readable without a working userland.
aws ec2 get-console-output --instance-id <id> --latest --output text \
  | grep -i "quota wait timed out"     # --latest is REQUIRED; see the method note
```

Use a **125 MiB/s** scratch volume: it reaches the stall in 12 GiB rather than 20.

Two things to record either way: whether ssh stays responsive throughout (the
liveness fix working), and the `timeouts` / `longest` figures from the dprintf
(the decay still being visible, which is what the instrumentation was for).

### Then, for the follow-up bake

With the serial fixes in, KDL becomes usable on arm64 for the first time:

```
# from the metal builder -- corp blocks :22 outbound from the workstation
aws ec2-instance-connect send-serial-console-ssh-public-key \
    --instance-id <id> --serial-port 0 --ssh-public-key file://k.pub
ssh -i k <id>.port0@serial-console.ec2-instance-connect.us-west-2.aws
# then type: kdl
#   bt <thread-id>        now traces THAT thread (see the arm64 bt fix)
#   page_writer_quota     queue depth, drain estimates, wait statistics
```

Account-level Serial Console access is already enabled, which was checked rather
than assumed.


## Before the promotion numbers: which of these results can move for reasons that are not regressions

`graviton`'s tip now carries eleven merges, and **each was verified on a different
base**: the scheduler work was measured on a deliberately frozen base without the
PCI and GIC changes, `memcpy` was measured before the checksum work existed, and
the checksum and offload numbers were each taken in the other's absence. The
promotion image is the first time any of it runs together. So a number that moves
there may be **composition rather than regression**, and it is worth saying which
of the storage figures are exposed *before* they are re-measured, so the
explanation is not reached for afterwards.

### Exposed, and expected to move

| result | why composition touches it | direction |
|---|---|---|
| concurrency sweeps (173.5 / 343.9 / 685.5 / 1072.5 MiB/s at depth 1/2/4/8) | the scheduler decides how many of the tool's threads actually run at once, and threads *are* the queue depth here | either; better scheduling should help the deeper rows |
| **the page cache costing 29%** (123.5 vs 173.2 MiB/s uncached) | that penalty is a copy plus no read-ahead, and arm64 `memcpy` was optimised since | the penalty should **shrink** — which would look like my finding was wrong |
| every `CPU µs per MiB` figure | all of them include copies | should fall |
| random 4 KiB at depth 16 (17,066 IOPS) | scheduler-sensitive at that thread count | either |
| `waits = 14684` | the scheduler changes how many writers contend for the quota | either, and neither direction is a defect |

The page-cache one deserves emphasis: **if the 29% penalty shrinks, that is
`memcpy` working, not a storage result being retracted.** The finding was "the
cache costs a copy and gives no read-ahead in return"; a cheaper copy narrows the
gap without changing the read-ahead conclusion, which is the part that matters for
finding 2.

### Not exposed

- The **volume ceiling** (~1008 MiB/s sustained) — that is EBS, not us.
- The **global dirty limit** (1,031,012 of 8,248,096 pages) — arithmetic from RAM
  size; it cannot move unless the instance changes.
- **nvme batching** — it has never produced a hardware number, only a boot and an
  `io batch size` line. There is no prior figure on that path, so its first
  measurement is a **result, not a comparison**.

### Why the 256 KiB negative control survives composition

256 KiB at depth 1 is one NVMe command, so batching cannot affect it — but
`memcpy` touches every read, so in principle the control is not perfectly inert.
It holds anyway because **the effect sizes are orders of magnitude apart**:
batching changes latency by a factor of about four (1441 → 6315 µs was four serial
commands), while a faster `memcpy` changes a 1441 µs device round trip by
microseconds. A control does not need to be perfectly isolated, only to be
insensitive to everything except the effect it is guarding against, by a margin
large enough that the two cannot be confused.


## Rules this exercise established

Two of the errors below were caught before they became published numbers. Both
are general, both would silently invalidate an entire matrix rather than produce
an obviously wrong cell, and both are stated here as rules rather than as
anecdotes.

### Rule: a rule in a document is not a habit

The retraction above is the case in point, and it is worth stating bluntly: the
thread sweep that produced the false "anti-scaling" was run in ascending order
**in the same change that wrote down "interleaved A/B, never before-then-after"**.
Knowing the rule, having just typed the rule, and writing the document the rule
lives in were all insufficient. What caught it was re-running the measurement
with a different instrument, not care.

So the rule is not "remember to interleave". It is: **any sweep is interleaved by
construction, or its result is not reportable** — and if a harness cannot
interleave a dimension, that dimension is measured one cell at a time against a
control, or not claimed.

### Rule: ask what a positive row looks like before believing an empty result

Four times in this work an empty or zero result was nearly read as a fact about
the system when it was a fact about the tooling:

- `get-console-output` without `--latest` returns an **empty body**, so every grep
  counted zero and "the quota never fires" looked true.
- `bc` does not exist on a DeBeOS image, so a `| bc` pipeline fell through to
  `|| echo 0` and a progress column read 0 while 16 GiB was being written.
- A `grep` filter that did not match bare numbers turned present counts into no
  output, which read as "the pieces are missing from the image".
- A deploy that silently produced a **zero-byte binary** ran and reported nothing,
  which reads as a tool with no output rather than a tool that is not there.

The defence is cheap and it worked every time it was applied: **before believing an
empty result, grep for something you know is present.** A positive control on the
console fetch (`nvme_disk`, 16 matches) is what exposed the `--latest` requirement
in one step. An empty result is a claim about the instrument until proven otherwise.

### Rule: an artifact check proves the change arrived, and only that

The sharper half of "an artifact must announce itself", and it cost a boot to
learn. The `serial debug listener` string **was** present in
`ami-0fe2b76a049766507`. The artifact check passed. The image was unbootable,
because the string was in the binary and the **call site was wrong**.

So a `strings`, `nm` or version-stamp check rules out exactly one hypothesis —
*"my change never arrived"* — and rules out nothing else. It does not establish
that the change is correct, that it runs, that it runs at the right time, or that
the machine survives it. Those need a boot.

Both halves are needed and neither substitutes for the other: without the stamp,
"the number did not move" and "the code did not load" are indistinguishable (which
is how the hot-swapped nvme driver was caught); with only the stamp, "the code
loaded" gets mistaken for "the code works" (which is how the boot panic got as far
as an AMI).

### Rule: a byte-identical failure across two builds means one build

The lesson I most wish I had had four hours earlier, and I had the evidence in hand
and drew the wrong conclusion from it.

A fix was applied, a second image was baked, and the panic came back **byte for
byte**: same `FAR=30`, same `ELR=ffff0000000b2c4c`, same frame addresses, same
ordering. I read that as "the fix was ineffective, so my diagnosis was wrong" and
retracted a correct finding. The right reading was the opposite and much simpler:
**identical addresses are near-proof that the same binary is running.** The fix had
moved a function between two translation units, which shifts link order; a genuinely
rebuilt kernel could hardly have reproduced the same addresses. What had actually
happened was that the bake pulled a stale remote ref, so the second build compiled
the first build's source.

I even reasoned *past* the evidence — noting that identical addresses were
"self-consistent" if the faulting code were linked before the files I changed — which
is true, but it is a weaker explanation than "it is the same build" and I should have
tested the stronger one first. **The cheap test I skipped was comparing the two AMIs'
snapshot IDs against what the sources should have produced, and confirming the ref
that was actually built.** I did compare snapshots, found them different, and treated
that as proof the source differed — but a different snapshot only proves a different
*build run*, not different *input*.

So: when a fix appears to change nothing and the failure is *identical* rather than
merely similar, the first hypothesis is **"the fix is not in what I ran"**, not "the
fix is wrong". Distinguish them by checking the input — the commit that was built —
rather than the output. Identical output is the signature of identical input, and
that is a much more common failure than an ineffective fix.

This is the same shape as the artifact rule two sections up, pointed the other way.
An artifact check proves a change *arrived*; a source precheck on the tree that is
about to be compiled proves *which* change is arriving. Neither substitutes for the
other, and the bake now does both.

### Rule: watch for the operation that is only valid once something else exists

Four defects in this work are the same shape — an operation performed before the
subsystem it depends on is ready — and the shape is worth recognising directly,
because none of the four looks like the others at the point of failure:

| operation | required first | how it presented |
|---|---|---|
| `spawn_kernel_thread()` from `arch_debug_console_init_settings()` (`main.cpp:168`) | `thread_init()` (`main.cpp:212`) | boot panic, `FAR=30`, in early VM setup |
| `bt <thread>` on a thread running elsewhere | `arch_debug_save_registers()` saving a frame pointer — it was an empty stub | a plausible, wrong stack |
| loading `nvme_disk` from `/boot/home/config/non-packaged` | the filesystem `nvme_disk` itself is needed to mount | silently ran the old driver |
| reading a `dprintf` from the console | the console buffer being fetched at all (`--latest`) | an empty result read as "never fired" |

Three of the four **fail silently or plausibly** rather than loudly, which is what
makes the shape worth naming: the failure mode of a missing precondition is usually
a confident wrong answer, not an error. The check that generalises is to ask, of
any call in an init path or a diagnostic path, *what has to be true already* — and
for init paths specifically, to read the actual ordering in `main.cpp` rather than
inferring it from the name of the hook.

### Rule: repeatability is not validity

The strongest illustration this project has. A fixed-size concurrency sweep
produced 2635 MiB/s at 16 threads — **2.2× the instance's hard maximum of
1250 MB/s, a limit enforced at the Nitro card** — and did so **repeatably to
under 1% across three repetitions**. Three reps agreed because all three were
wrong in the same way: each cell was short enough (0.39 s at the top of the
sweep) that the EBS token bucket never bound, so every cell measured burst
credit.

Repetition detects noise. It cannot detect a systematic artefact, because a
systematic artefact repeats. **The only thing that caught this was comparing the
result against the hardware's documented ceiling.** Therefore: every throughput
number gets checked against a published limit before it is believed, and a number
that exceeds one is treated as a broken measurement rather than a discovery.

### Rule: `malloc` and `posix_memalign` measure different code paths

`malloc` guarantees no page alignment. `nvme_disk` diverts a request whose vecs
are not page-aligned to a bounce path capped at `kMaxBounceBufferSize`
(`4 * B_PAGE_SIZE` = 16 KiB), so a misaligned 256 KiB request becomes sixteen
sequential commands, and a bounced write additionally takes `rounded_write_lock`
exclusively. A benchmark that simply `malloc`s its buffer therefore measures
whichever path that day's allocator happened to hand it, and the two differ by
more than an order of magnitude.

Therefore: an I/O buffer is `posix_memalign`ed, and its alignment is **reported
with the result**, so no figure can be quoted without the code path it came from.
`disktput` takes `-A`/`-U` so alignment is a variable rather than an accident.

### Rule: the boot-path carve-out is real; the "drivers can't be dropped in" version is NOT

**Note added 2026-08-24, and it records a wrong turn rather than a finding.** An
earlier revision of this section asserted that *no* driver can be hot-swapped —
that `ena` in particular could not be, because the device manager's support-score
competition would hand the tie to the packaged copy. **That is wrong. It was
reasoned from code, and the code was read in the wrong direction.**

What the code actually says, re-read in full:

- `device_node::_FindBestDriver()` (`device_manager.cpp:1794`) enumerates candidates
  through `open_module_list_etc()`, and keeps one only on
  `if (support > bestSupport)` — **strictly greater** (`:1803`). So a tie is decided
  by **whichever copy is enumerated first**.
- That iterator's push loop (`module.cpp:2015`) walks `kModulePaths` **forward** —
  `B_SYSTEM_ADDONS_DIRECTORY`, `B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY`,
  `B_USER_ADDONS_DIRECTORY`, `B_USER_NONPACKAGED_ADDONS_DIRECTORY` — pushing each
  onto a stack, and `iterator_pop_path_from_stack()` (`:831`) pops **LIFO**. So
  `B_USER_NONPACKAGED_ADDONS_DIRECTORY` is searched **first**.

**First-scanned wins a tie, and non-packaged is scanned first — so a non-packaged
driver returning the same support score as the packaged one WINS.** `ena` returning
a flat `0.8f` (`ena.cpp:3258`) is therefore droppable-in, and the successful
driver-only `ena` hot-swaps reported in `ena-tx-offload.md` are consistent with the
code rather than in conflict with it.

**Where the wrong version came from.** There *is* a reverse walk over the same array
— at `module.cpp:632` and `:1896` — but that is inside `search_module()`, a different
path with a different job. Reaching for it to explain the `nvme_disk` failure gave a
tidy mechanism for the wrong observation.

**So the carve-out is narrow, and it is about the filesystem, not about scoring:**
the **boot storage driver** cannot be overridden because
`B_USER_NONPACKAGED_ADDONS_DIRECTORY` resolves under **`/boot/home`**, which is on
the volume that driver is required to mount. At the moment the kernel needs it, the
directory that would override it does not exist. **That does not generalise to
drivers outside the boot path.** A network driver is not in the boot path and is
hot-swappable.

> ### The actual rule: settle this with a version stamp, not by reading code
>
> **This question has now been answered wrongly twice, in opposite directions, by
> people reading the same source.** The scoring rule, the array order, the LIFO pop
> and the boot-path exception all have to be composed correctly to get an answer, and
> a plausible wrong composition is available at every step.
>
> **Everything above is reasoning-from-code, not a measurement.** Do not treat it as
> settled. The cheap, decisive alternative is the subject of the next section:
> **compile a distinctive version string into the module and look for it.** That is
> what caught the `nvme_disk` case, and it is the only evidence in this whole episode
> that never pointed the wrong way.

### Rule: stamp a version into anything you hot-swap

Generalised from the `nvme_disk` finding, and it applies to every module and
every agent. The module was installed, made executable, hash-verified against the
builder and cold-booted, and the **stock driver ran anyway**. The only reason that
read as "not loaded" rather than "loaded, no effect" is a deliberate
`TRACE_ALWAYS("io batch size: %d")` in the modified copy.

Without the stamp the reading would have been "my change does nothing" — a wrong
conclusion about the code rather than a wasted cycle, and one that would have sent
the next person looking in the wrong place. **A distinctive `dprintf` or version
string in anything hot-swapped is not optional**, because "the number did not
move" and "the code did not load" are indistinguishable otherwise.

### Rule: deploying to a DeBeOS node requires chunking and a hash check

Not an optimisation — a correctness requirement, because this failure mode
**fails closed in the worst possible way: you get a file, it is just empty.**

`scp` to a DeBeOS node does not work, and the documented workaround
`base64 -w 200 <file> | ssh <node> 'base64 -d > /path'` **truncates at exactly
65536 bytes and then drops the connection.** Any binary over 64 KiB of base64
arrives empty or corrupt. It happened twice while this was being written, and
once it overwrote a working binary with a zero-byte one; the shell reported
success both times, and a subsequent run reported nothing at all rather than
failing loudly.

Therefore the standard is: **split the base64 into 32 KiB pieces, append them,
and compare SHA-256 on the node against the builder before measuring anything.**
`disktput-run` prints the build stamp and the hash for exactly this reason.

## Disproven, and a correction to a reported regression: the page writer bug is not still present

A code review of this tree reported that the page-writer flush bug was
unfixed — that `vm_page_writer.cpp` still reads
`if (!fPageWriterCondition.Wait(PAGES_FLUSH_DURATION_LOCAL_QUOTA, true)) continue;`,
so the periodic flush never fires. That would have invalidated every buffered
write measurement here.

It is an artifact of reading the wrong branch. Commit `8331882470`
*"kernel/vm: don't skip the page writer's periodic flush on timeout"* is present
in `graviton` and removes the `continue`; the branch that was read had forked
before it. Recorded here because the reasoning about `BinarySemaphore::Wait()`
returning `false` on timeout is correct and worth keeping — it is exactly why the
bug existed — and because "which branch is this" is a live hazard in a tree with
a dozen concurrent worktrees.

## Summary

The headline is that **there is no storage throughput crisis**. Sequential and
random I/O on the raw device are at Linux parity to within 1–3% and reach the
volume's provisioned ceiling, and reads through BFS cost nothing measurable.
Saying that plainly matters, because the single-threaded figure of 173 MiB/s
looks alarming next to a 1,000 MiB/s volume and would have supported a
manufactured optimisation project. It is not a defect: Linux measures 174 MiB/s
under the same one-request-per-thread condition.

| | DeBeOS | Linux / ceiling | verdict |
|---|---|---|---|
| seq read, depth 1 | 173.5 MiB/s | 174.4 | **parity** |
| seq read, depth 8, sustained | 1007.7 MiB/s | ~1008 volume ceiling | **at ceiling** |
| seq write, depth 16, sustained | 1008.0 MiB/s | ~1008 volume ceiling | **at ceiling** |
| random 4 KiB read, depth 16 | 17,066 IOPS | 16,534 / 16,000 provisioned | **at ceiling** |
| BFS read vs raw read | 0–1.5% overhead | — | **free** |
| durability across hard power loss | 0 bad blocks | — | **passes on EBS** ¹ |
| seq read, 1 MiB, depth 1 | 158.3 MiB/s | 598.0 | **3.8× gap** |
| BFS write, depth 16 | ~200 MiB/s buffered *and* uncached | 1020 raw | **5.1× gap, in BFS** |
| sustained write load | 180 → 19 MiB/s, userland starved **25+ min** | — | **liveness bug** ² |
| read through the page cache | 123.5 MiB/s | 173.2 uncached | **cache costs 29%** |

² Starvation, not deadlock: it recovers when the write load stops. Reproduced
3/3 times; cheapest recipe is 12 GiB on a 125 MiB/s volume.

¹ Passes *because EBS has no volatile write cache*, not because `fsync` is a
barrier — it is not. See Result 5; the caveat must travel with the claim.

Four things are worth someone's time, in this order:

1. **Sustained buffered write load degrades ~10× and starves userland for as
   long as the load lasts** — over 25 minutes observed — with the kernel still
   answering ping and sshd still accepting connections, and no panic or log line.
   **Reproduced 3/3**; the cheap recipe is 12 GiB on a 125 MiB/s volume.
   It recovers when the load stops, so it is starvation rather than deadlock. The cheapest first fix is to bound the indefinite wait in
   `WaitIfOverQuota` so a hang becomes a slow write. Kernel change.
   *(This replaces what was listed here as "buffered writes are irreproducible and
   anti-scale", which was an artifact of my own un-interleaved sweep.)*
2. **No read-ahead in the file cache**, which is why going through the page cache
   is 29% *slower* than bypassing it on a large sequential read. Probably the
   largest available win for real workloads. Kernel change, so it needs a bake.
3. **Writes through BFS cap at about a fifth of the device** at depth 16 —
   ~200 MiB/s against 1020. Now attributed to BFS rather than the page writer,
   because the uncached path, which never touches the page writer's quota, caps at
   the same place. Suspect the single journal and its per-commit
   `block_cache_sync`.
4. **Requests larger than 256 KiB are issued serially** by `nvme_disk`
   (`await_status()` per chopped command). Worth up to 3.8× on raw-device bulk I/O
   at low concurrency, nothing on file I/O — the file cache never emits a request
   larger than 128 KiB. **A fix is written and committed but unverified:** the boot
   disk driver turns out *not* to be hot-swappable, because the non-packaged
   directory that would override it lives on the filesystem the driver is needed to
   mount. It needs a bake. See "Finding 4" below.

Deliberately not pursued: multi-queue. `qpair count: 2` is what the EBS
controller offers, and Linux reports the same, so the per-CPU queue selection
already in the driver is correct and complete for this hardware.

## Finding 4: the serialisation quantified, a fix written, and why it needs a bake

### The serialisation, measured exactly

Stock driver, raw device, one thread, 15 s cells, three repetitions agreeing to
**0.02%**:

| block size | commands | throughput | mean latency | latency ÷ 256K latency |
|---|---|---|---|---|
| 256 KiB | 1 | 173.50 MiB/s | 1441 µs | 1.00 |
| 512 KiB | 2 | 145.58 MiB/s | 3434 µs | **2.38** |
| 1 MiB | 4 | 158.35 MiB/s | 6315 µs | **4.38** |
| 2 MiB | 8 | 165.72 MiB/s | 12,068 µs | **8.37** |

Latency scales with the **number of chopped commands**, essentially one-for-one.
That is the serialisation, measured rather than inferred: a request is split at
`max_xfer_size` and each piece waits for the previous one. Linux on the same
volume gets 327.7 MiB/s at 512 KiB and 598.0 at 1 MiB, because it issues the same
pieces concurrently.

Note also that 512 KiB is *slower than* 256 KiB (145.6 vs 173.5) — two serial
commands cost slightly more than twice one, so the chop is worse than neutral.

### The fix

`nvme_disk.cpp`: `submit_nvme_io_request()` is split out of
`do_nvme_io_request()`, and `do_io()` now submits up to `NVME_IO_BATCH_SIZE` = 8
chopped commands before reaping any of them. Reaping in submission order costs the
slowest command rather than the sum. The qpair is now chosen **once per request**
rather than per command, which also fixes a latent bug: `await_status()` polls a
specific qpair and `get_qpair()` picks by current CPU, so a thread that migrated
between submitting and waiting could previously poll a queue its command was not
on.

Only the contiguous prefix of successful commands is counted as transferred, so a
later command completing cannot make an earlier failed one's bytes look valid.

**Predicted, not measured:** 512 KiB → ~300 MiB/s, 1 MiB → ~550 MiB/s, i.e. close
to the Linux figures, with 2 MiB (8 commands, exactly the batch size) benefiting
most.

### Why it is not verified: the boot disk driver cannot be hot-swapped

The module was built, installed at
`/boot/home/config/non-packaged/add-ons/kernel/drivers/disk/nvme_disk`, made
executable, hash-verified against the builder, and the node cold-booted. The
driver logs a deliberate version stamp (`io batch size: 8`) so the loaded copy can
be identified rather than inferred from a number moving. **The stamp did not
appear: the kernel loaded the stock module from `/boot/system`.**

The reason is structural, not a wrong path. `kModulePaths` is walked backwards, so
the user non-packaged directory really is searched first — but
`B_USER_NONPACKAGED_ADDONS_DIRECTORY` resolves under **`/boot/home`**, which is on
the filesystem that `nvme_disk` itself is required to mount. At the moment the
kernel needs the disk driver, the directory that would override it does not exist
yet. `/boot/system/add-ons` cannot be used instead because it is packagefs and
read-only.

**This corrects a piece of this project's operating knowledge.** "Drop a kernel
module in `non-packaged/add-ons/kernel` and reboot, 4 minutes instead of 25" is
true for modules loaded *after* the boot volume is mounted — a network driver, a
non-root file system. It is **structurally impossible for the boot storage
driver**, and the failure is silent: the module sits there, the machine boots
happily, and it is the old code that runs. Anyone testing a storage driver this
way and reading a number would conclude the change did nothing.

So the change is committed but **unverified**, and it needs an image bake — which
is the operator's job. The version stamp is deliberately left in so that the first
boot of a baked image proves which driver is running before any number is taken
from it.

### What to run once it is baked

```bash
# expect ~300 MiB/s at 512K and ~550 at 1M, against the 145.6/158.4 baseline
disktput -f /dev/disk/nvme/1/raw -m seqread -b 512K -t 1 -T 15 -s 8G -J
disktput -f /dev/disk/nvme/1/raw -m seqread -b 1M   -t 1 -T 15 -s 8G -J
disktput -f /dev/disk/nvme/1/raw -m seqread -b 2M   -t 1 -T 15 -s 8G -J
# and prove it did not corrupt anything -- this is a data path change
disktput -f /dev/disk/nvme/1/raw -m seqwrite -b 1M -t 4 -n 4G -s 8G -P -y
disktput -f /dev/disk/nvme/1/raw -m verify   -b 1M -t 4 -n 4G -s 8G
# negative control: 256K is one command and must NOT move
disktput -f /dev/disk/nvme/1/raw -m seqread -b 256K -t 1 -T 15 -s 8G -J
```

The 256 KiB row is the negative control: it is a single command, so the batching
cannot touch it, and if it moves then something other than batching changed.


## Scoping finding 2: read-ahead in the file cache

The measured cost: reading a file through the page cache runs at **123.5 MiB/s
against 173.2 MiB/s with `O_NOCACHE`** — the cache makes a large sequential read
**29% slower**. Every ordinary program reads through the cache, so this is the
largest real-workload win in this document.

### Why it is smaller work than it looks

**The sequential-access detector already exists and already works.**
`file_cache.cpp` keeps a ring of the last accesses per `file_cache_ref`
(`last_access[LAST_ACCESSES]`, `push_access()`) and exposes
`access_is_sequential(ref)`. It is currently consulted in exactly one place —
`reserve_pages()`, to decide *what to evict* when memory is low — and never to
decide what to fetch. So the detection half of read-ahead is done; what is
missing is issuing the fetch.

**There is also already a prefetch path.** `cache_prefetch_vnode()` takes a vnode,
offset and size, resolves the `file_cache_ref`, clamps to the file size, rounds to
pages and checks resources before reading. It is used at boot and by mmap
fault-around. A read-ahead would reuse this rather than inventing a mechanism.

### What actually has to be written

1. **A per-`file_cache_ref` read-ahead window** — current size and the offset the
   last readahead reached, so the window can grow on continued sequential access
   and reset on a seek. This is new state on `file_cache_ref`, which is the only
   structural change.
2. **A hook in the cached read path** that, when `access_is_sequential()` is true,
   issues an asynchronous fetch for the next window beyond the current read.
3. **Asynchrony.** This is the one genuinely hard part. The win comes from the
   next range being fetched *while the caller consumes the current one*; a
   synchronous prefetch just moves the same wait earlier and buys nothing. The
   read path currently blocks in `read_into_cache()`. Either the prefetch is
   handed to a worker, or it is issued as a non-waiting `IORequest` whose
   completion unbusies the pages.
4. **A cap and a back-off.** Read-ahead that guesses wrong evicts useful pages and
   wastes device bandwidth. Needs a maximum window, and it must not run when
   `low_resource_state(B_KERNEL_RESOURCE_PAGES)` is set — the same condition
   `reserve_pages()` already tests.

### Sizing, and why 128 KiB is the number to beat

The cached read path chops at `MAX_IO_VECS * B_PAGE_SIZE` = **128 KiB**, and the
driver blocks per command, so a cached sequential read is one 128 KiB round trip
at a time. At the measured ~1.4 ms per round trip that is ~90–125 MiB/s, which is
what 123.5 MiB/s is. A read-ahead window of 8 × 128 KiB would put ~1 MiB in
flight and should approach the depth-8 figure of ~1008 MiB/s. **Predicted, not
measured** — and worth stating as a prediction so it can be falsified.

### Cost and risk

- **Kernel proper, so it needs a bake**, and the bake is the operator's job.
- Risk is moderate and mostly in the asynchronous fetch: a prefetch that leaves
  pages busy or double-frees on error is a corruption bug, not a slow path. It
  wants the `-P`/`-m verify` content check from `disktput` run against it, not
  just a throughput number.
- The interaction with `reserve_pages()`'s low-memory eviction needs care: that
  code already treats sequential access as a reason to *drop* pages, and
  read-ahead would be adding them. Those two must not fight, which is a design
  question to settle before writing code.

Recommendation: worth doing, after finding 1 has a fix, and it should ship with a
durability/content assertion rather than a rate.

## Reproducing this

```bash
# on the metal builder, against a DeBeOS node with a scratch volume attached
graviton/scripts/disktput-run <node-ip> -d /dev/disk/nvme/1/raw -m threads -T 15 -r 3
graviton/scripts/disktput-run <node-ip> -m blocks -T 15 -r 2
graviton/scripts/disktput-run <node-ip> -m random -T 15 -r 2
```

Do not measure the root volume: it is a 2 GiB gp3 at the 125 MiB/s floor with a
300 MiB filesystem on it, and nothing measured there describes the OS.

If storage acquires a regression-worthy number, the candidate for
`graviton/scripts/haiku-perf-gate` is **sequential read at depth 8** (stable to
0.1% across runs, and sensitive to both the driver and the cache) together with
the **durability check** — write with `-P`, hard stop, `-m verify`, assert zero
bad blocks. That last one is the assertion that would have caught the bug this
project actually shipped. Proposed, not deployed.
