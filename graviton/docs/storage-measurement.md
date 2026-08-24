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

## The real finding: sustained write load degrades without recovering, and then wedges

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

### The KDL prerequisites are satisfied — checked, not assumed

Interactive KDL on an EC2 instance needs serial **input**, not just the output
that `get-console-output` returns. Both halves work:

- **EC2 Serial Console access is enabled** for account 668984504585
  (`get-serial-console-access-status` → `True`), so
  `send-serial-console-ssh-public-key` plus ssh to the serial-console endpoint
  gives a bidirectional console.
- **arm64 can read it.** `arch_debug_serial_getchar()` is implemented
  (`sArchDebugUART->GetChar(false)`), so KDL can accept typed commands. Note that
  `arch_debug_serial_try_getchar()` is still a TODO that falls through to the
  blocking version; that has not caused a problem here but is worth knowing if
  KDL behaves oddly.

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
| sustained write load | 180 → 19 MiB/s, then **wedges** | — | **liveness bug** |
| read through the page cache | 123.5 MiB/s | 173.2 uncached | **cache costs 29%** |

¹ Passes *because EBS has no volatile write cache*, not because `fsync` is a
barrier — it is not. See Result 5; the caveat must travel with the claim.

Four things are worth someone's time, in this order:

1. **Sustained buffered write load degrades ~10× and then wedges userland
   indefinitely**, with the kernel still answering ping and sshd still accepting
   connections, and with no panic or log line. Reproduced once; recipe in "The
   real finding" below. The cheapest first fix is to bound the indefinite wait in
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
