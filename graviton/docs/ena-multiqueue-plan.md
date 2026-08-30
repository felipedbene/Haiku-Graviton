# ENA multi-queue + RSS: ~~what is blocked~~ CANCELLED on evidence — do not implement this

> ## STOP. This project is CANCELLED as of 2026-08-24. This document is a design that
> ## was never built and must not be built.
>
> **Confirmed again 2026-08-30.** A later Stage 3/4 attempt did build the enabling
> infrastructure (interrupts now land on any CPU; the driver can request more io
> queues) and measured it end to end: correct, safe, no regression — and a conclusive
> **no-win**. Receive still caps around 10 Gbit/s with the box ~86% idle, because the
> ceiling is a single-reader serialization in the network stack's receive path, not the
> queue count. The fix is a receive-path rework, not more queues — exactly as the
> analysis below concluded. Queue-count work stays cancelled.
>
> **The deciding control, which post-dates everything below:** **Linux, forced down to
> ONE ENA queue, does 29826 Mbit/s — against 29823 on eight** (`c7g.16xlarge`,
> interleaved A/B, four runs; `ena-multiqueue-headroom.md` §5). One queue already
> carries ~30 Gbps. DeBeOS's plateau on that class is **9.0–10.2 Gbit/s**. More queues
> cannot lift a ceiling that a single queue clears three times over, so **queue count
> is not the limiter and cannot be**.
>
> **Two further facts that void specific parts of the design below:**
>
> - The ENA IO-queue grant is a **fixed 8 for the whole C7g family** — it is **not** a
>   function of vCPU count. Any "one queue per vCPU" scaling story here is wrong about
>   the device.
> - **The real limiter (corrected 2026-08-25) is bufferbloat in the device-interface
>   receive FIFO** — a standing ~13.7 ms, ~16 MiB queue that is ~99.93% of a frame's
>   transit time — **plus `TCPEndpoint::fLock`, ~47% of the ceiling**
>   (`ena-receive-latency-account.md`). It is **not** interrupt cadence, which was
>   falsified and stays falsified. The `XXX STRUCTURAL FIX STILL OWED` at `ena.cpp:221`
>   (§7 step 2) is a **correctness** fix worth doing on its own terms, not the
>   throughput lever.
>
> ### The specific error in this document, because it is the transferable one
>
> §6.1 extrapolates a **"~8 Gbit/s ceiling"** from 4936 Mbit/s consuming **62 % of a
> 2-vCPU `c7g.large`**. **The arithmetic is fine. The label is not.** That figure is
> "the ceiling **on `c7g.large`**" — a 2-vCPU saturation point — and it was written,
> and then read, as *the* ceiling. The instance class was in the working and absent
> from the conclusion.
>
> **Rule: never let a saturation or ceiling figure stand without its instance class.**
> A number that means "this 2-vCPU box is out of CPU" reads, one paragraph later, as
> "the driver cannot go faster than this" — and a 16xlarge then measured 10.2 Gbit/s
> against Linux's 29.8, which the "8 Gbit/s ceiling" would have declared impossible.
>
> Everything below is retained as the record of the analysis. Its `file:line`
> groundwork and its "what is blocked in the stack" survey are accurate and useful;
> its **recommendations and rankings are void**.

**Date:** 2026-08-23. **Status: CANCELLED 2026-08-24.** **Scope:** analysis and design
only — nothing in this document
has been implemented, and nothing in it should be. **Method:** static reading of this tree at `graviton`; every
claim below carries a `file:line` so it can be checked or refuted without a boot.
Where a claim needs hardware to settle, it is marked **[needs hardware]** and the
exact command that settles it is given.

---

## 0. Verdict, up front

**Receive multi-queue is blocked, and it is blocked in two independent places —
neither of them the driver.** Transmit multi-queue is *not* blocked. And the honest
headline is uncomfortable:

> **Multi-queue is not the next lever. It is not even the second lever.** *(This
> judgement was right, and a later control made it final — see the banner at the top.)*
> At the
> measured cost of ~18 µs of CPU per 9 KB frame, receive at 4.9 Gbit/s already burns
> **62 % of the entire 2-vCPU c7g.large** (arithmetic in §6.1) — **a saturation figure
> for that class only; it is not a fleet-wide ceiling, and §6.1 went on to read it as
> one.** The bottleneck is
> per-frame cost, not per-core parallelism, and the largest single contributor found
> is a **one-line mismatch between `ENA_PACKET_BUFFER_SIZE` and what a `net_buffer`
> data node can actually hold** (§6.2). That is shippable now, needs no stack change,
> and is measurable on the instance we already have.

Multi-queue, when it is finally worth doing, is a **kernel project plus a stack
project plus a driver refactor**, in that order of difficulty, and it cannot be
measured at all on a 2-vCPU instance.

### One correction to the framing

The brief quotes "transmit 3852 Mbit/s". That number is a **CPU cost, not a rate** —
`throughput-measurement.md` records transmit at MTU 9001 as *1419 Mbit/s and
3852 µs/MiB* with the old 64 KiB socket buffer. With `send.buffer_size` now at
256 KiB, transmit measures **4375–4451 Mbit/s at ~2221–2228 µs/MiB**. So both
directions sit at **4.4–4.9 Gbit/s and ~2.1–2.2 ms of CPU per MiB**. The two
directions cost almost the same per byte, which matters: receive does strictly more
work per frame than transmit (validate, allocate, copy in, enqueue, hand off), so
transmit being no cheaper says the cost is not concentrated where it looks.

---

## 1. What the hardware and the common layer already support

### 1.1 `ena-com` is fully multi-queue capable, untouched

- `ena-com/ena_com.h:39` — `#define ENA_MAX_NUM_IO_QUEUES 128U`
- `ena-com/ena_com.h:41` — `#define ENA_TOTAL_NUM_QUEUES (2 * (ENA_MAX_NUM_IO_QUEUES))`
- `ena-com/ena_com.h:395-396` — `io_cq_queues[ENA_TOTAL_NUM_QUEUES]` and
  `io_sq_queues[ENA_TOTAL_NUM_QUEUES]` are already arrays. The HAL indexes them by
  `qid` throughout (`ena_com.h:1149,1159`).

**No change to `ena-com/` is required for multi-queue.** The vendored layer stays
byte-identical to `ena_freebsd_2.8.4`, as `ena-com/README.md` requires. Queue-pair
*i* is `(qid 2i, qid 2i+1)` — which is exactly the convention the driver's
`ENA_TX_QUEUE_ID 0` / `ENA_RX_QUEUE_ID 1` (`ena.h:74-75`) already follows for pair 0.

### 1.2 RSS is already fully wired, and already pointed at one queue

`ena_prepare_rss()` (`ena.cpp:820-859`) already does everything except spread:

- `ena_com_rss_init(comDev, ENA_RSS_TABLE_LOG_SIZE)` — a 128-entry indirection table
  (`ena.h:211-212`).
- Toeplitz hash with a device-generated 40-byte key:
  `ena_com_fill_hash_function(comDev, ENA_ADMIN_TOEPLITZ, NULL, ENA_RSS_HASH_KEY_SIZE, 0x0)`
  (`ena.cpp:841-842`).
- `ena_com_set_default_hash_ctrl()` (`ena.cpp:848`).
- `ena_flush_rss()` pushes table + function + control to the device after the queues
  exist (`ena.cpp:868-883`).

The *only* thing making this a no-op is `ena.cpp:830-838`, which fills every one of
the 128 buckets with `ENA_RX_QUEUE_ID`:

```
/* One queue pair, so everything lands on our single receive queue. */
result = ena_com_indirect_table_fill_entry(comDev, i, ENA_RX_QUEUE_ID);
```

**Spreading RSS across N receive queues is therefore a change to one loop body.**
That is category (c) work — already possible, simply not done — and it is worthless
on its own, because nothing above the driver can consume more than one receive queue
(§3).

### 1.3 What the driver hard-codes to 1

| Thing | Where | Note |
|---|---|---|
| Queue pairs | `ena.h:73` `ENA_IO_QUEUE_PAIRS 1` | Defined but, notably, **never referenced anywhere in `ena.cpp`** — the "1" is structural, not parameterised. |
| MSI-X vectors | `ena.cpp:53-55` `ENA_MGMNT_VECTOR_IDX 0`, `ENA_IO_VECTOR_IDX 1`, `ENA_MSIX_VECTOR_COUNT 2` | `ena_enable_msix()` refuses a device offering fewer than 2 (`ena.cpp:287-291`) and requests exactly 2 (`ena.cpp:294-295`). |
| Queue creation | `ena_create_queue_pair()` `ena.cpp:972-1042`, called once from `ena_setup_io_queues()` `ena.cpp:1057` | Carries the standing `TODO` at `ena.cpp:978-979`. |
| Queue handles | `ena.h:285-288` | Four scalar pointers, not arrays. |
| Locks | `ena.h:300` `txLock`, `ena.h:314` `rxLock` | One of each, device-wide. |
| Buffer pools | `ena.h:296-312` | One TX pool + free-id stack, one RX pool + fill/refill cursors. |
| Wake-ups | `ena.h:301` `txCompleted`, `ena.h:318` `rxReady` | One semaphore per direction, released by the single ISR (`ena.cpp:201-204`). |
| ISR | `ena_io_interrupt()` `ena.cpp:191-237` | One handler, installed once (`ena.cpp:1751`), re-arms via `txCompletionQueue` (`ena.cpp:229-234`). |

The device's own advertised limits **are already read and logged** but never used:
`ena.cpp:462-477` prints `max_tx_sq_num / max_tx_cq_num / max_rx_sq_num /
max_rx_cq_num` from `max_queue_ext` (or `max_sq_num / max_cq_num` on the legacy
path). See §7 step 0 — **the answer for our test instance is already in every boot
log we have taken.**

---

## 2. Interrupt affinity: blocker #1, and it is in the kernel, not the stack

This is the one that makes "N vectors, N cores" impossible today, and it is the
blocker that the earlier note in `graviton-optimization-plan.md` §5 flagged as an
"open question". It is not open. It is a stub.

### 2.1 There is an affinity API. It is a no-op on arm64.

- `headers/private/kernel/interrupts.h:86` — `void assign_io_interrupt_to_cpu(int32 vector, int32 cpu);`
  Private kernel header, **not** in `KernelExport.h`, and no add-on in the tree calls
  it — only the scheduler does (`scheduler/low_latency.cpp:178`,
  `scheduler/power_saving.cpp:217,267`, `scheduler/scheduler_cpu.cpp:119`).
- `headers/private/kernel/arch/int.h:37` — the arch hook
  `int32 arch_int_assign_to_cpu(int32 irq, int32 cpu);`
- **`src/system/kernel/arch/arm64/arch_int.cpp:62-67`:**

```cpp
int32
arch_int_assign_to_cpu(int32 irq, int32 cpu)
{
	// Not yet supported.
	return 0;
}
```

It ignores `cpu` and returns 0 unconditionally. Only x86 implements the real thing
(`arch/x86/arch_int.cpp:450-467`). Since the generic round-robin placement at
`src/system/kernel/interrupts.cpp:462-474` and the rebalancer at
`interrupts.cpp:726-752` both *store the return value*, `assigned_cpu->cpu` is
permanently **0** on arm64. The generic load tracking (`interrupts.cpp:245-257`) and
the scheduler rebalancing (`scheduler/low_latency.cpp:136-179`) all run, and all
accomplish nothing.

### 2.2 The GICv3/ITS hardware routing is fixed to the boot CPU at init

- **SPIs:** `arch/arm64/arch_int_gicv3.cpp:133-137` writes `GICD_IROUTER` for every
  SPI once, to the boot CPU's affinity, and that is the **only** write site of
  `GICD_IROUTER` (`gicv3_regs.h:19`) in the tree. `GICD_IROUTER_IRM` (1-of-N
  delivery, `gicv3_regs.h:29`) is defined and never used.
- **LPIs — which is what MSI-X is on this port:**
  `arch/arm64/gicv3_its.cpp:129-131` creates exactly **one** ITS collection,
  targeting one redistributor:

```cpp
// A single collection, targeting the redistributor of the boot CPU, is
// enough: every LPI is delivered there.
status = _MapCollection(0, fCollectionTarget, true);
```

  Every MSI is mapped to collection `0`, hardcoded at `gicv3_its.cpp:626`. The
  comment at `gicv3_its.cpp:350-352` says it outright: *"LPIs have to be enabled on
  all of them even though only the boot CPU is targeted today."*
- **Retargeting is not merely unwired, the opcodes are absent.** `gicv3_regs.h:157-163`
  defines MAPD / MAPC / MAPTI / INV / INVALL / DISCARD / SYNC. `GITS_CMD_MOVI` (0x01)
  and `MOVALL` (0x0e) — the only ways to move an LPI to another
  collection/redistributor — are **not implemented**.

### 2.3 Even on x86 you could not pin individual MSI-X vectors of one device

`src/system/kernel/interrupts.cpp:673`, inside `allocate_io_interrupt_vectors()`:

```
sVectors[vector + i].assigned_cpu = &sVectorCPUAssignments[vector];
```

Note the base index, not `vector + i`. **The whole allocated MSI-X block shares a
single `irq_assignment`, so it is one affinity unit.** `install_io_interrupt_handler`
correspondingly only performs the assignment for the first handler in the block
(`interrupts.cpp:462-474`, guarded on `assigned_cpu->cpu == -1`). This is a design
limitation of the interrupt layer, independent of arm64, and it has to be fixed for
per-queue affinity to mean anything.

### 2.4 Consequence

**Requesting N MSI-X vectors on this port delivers all N to CPU 0.** The AWS guidance
quoted in `graviton-optimization-plan.md` §5 — "assign eth0 ENA interrupts to the
first N-1 cores", which it calls *the point* of the exercise — is not merely
unimplemented here; the mechanism it needs does not exist at three separate layers.

### 2.5 Vector budget, for when this is fixed

| Limit | Value | Where |
|---|---|---|
| PCI bus manager, per device | 32 | `bus_managers/pci/pci.cpp:2347` |
| ITS events per device | 32 | `gicv3_its.h:21` `GIC_ITS_EVENTS_PER_DEVICE`, enforced `gicv3_its.cpp:582` |
| ITS total MSI vectors, all devices | 256 | `gicv3_its.h:15` `GIC_ITS_MAX_VECTORS` |
| ITS devices tracked | 32 | `gicv3_its.h:18` |

Allocation requires a **contiguous** LPI run (`gicv3_its.cpp:597-611`, returns
`B_BUSY` otherwise), so 32 vectors is a comfortable ceiling but fragmentation is a
real failure mode once several devices allocate and free. 32 vectors ⇒ up to 31 IO
queue pairs, which is above any Graviton instance's vCPU count that we would target
in this decade of work.

---

## 3. The receive path: blocker #2, in the stack

### 3.1 The device interface has no concept of a queue

`headers/private/net/net_device.h:22-38` — `struct net_device` has `mtu`, `media`,
`link_speed`, `flags`, `stats`. **There is no queue count, and no per-queue
anything.**

`headers/private/net/net_device.h:54`:

```c
status_t	(*receive_data)(net_device* device, net_buffer** _buffer);
```

**One buffer per call. No queue index. No count. No batch.** This one line is the
whole receive-side API. Nothing in `headers/private/net/` mentions RSS, queues, or a
packet hash — verified by grep across all of `headers/private/net/*.h`.

`headers/private/net/net_buffer.h:19-21` — `net_buffer_flags` has exactly
`NET_BUFFER_L3_CHECKSUM_VALID` and `NET_BUFFER_L4_CHECKSUM_VALID`. There is nowhere
to record which hardware queue a frame arrived on even if we wanted to.

### 3.2 Exactly one reader thread, and it is structurally one

`src/add-ons/kernel/network/stack/device_interfaces.h:31` — `thread_id reader_thread;`
A scalar. Spawned once in `up_device_interface()`,
`device_interfaces.cpp:538-541`:

```cpp
interface->reader_thread = spawn_kernel_thread(device_reader_thread,
	name, B_REAL_TIME_DISPLAY_PRIORITY - 10, interface);
```

and joined by `thread_id` in `down_device_interface()`, `device_interfaces.cpp:580-587`.

`device_reader_thread` itself (`device_interfaces.cpp:48-93`) is a serial loop:
`receive_data` (:57) → monitors (:60-61) → `deframe_func` inline (:65) →
`fifo_enqueue_buffer` (:72). Every received frame on the machine passes through this
one thread, one at a time.

### 3.3 One FIFO, one consumer thread, and a lock across the whole protocol stack

- One FIFO per interface: `device_interfaces.h:49` `net_fifo receive_queue;`,
  initialised once at `device_interfaces.cpp:192` with a 16 MiB cap.
- Its enqueue/dequeue take a plain mutex (`stack/utility.cpp:211`, `:232`), so reader
  and consumer already contend on it once per frame.
- One consumer thread: `device_interfaces.h:47` `thread_id consumer_thread;`, spawned
  once at `device_interfaces.cpp:206-210`.
- **`device_consumer_thread` holds `interface->receive_lock` across the entire
  protocol dispatch** — `device_interfaces.cpp:128`:

```cpp
RecursiveLocker locker(interface->receive_lock);
...
&& handler->func(handler->cookie, device, buffer) == B_OK)
```

  `handler->func` is `domain_receive_adapter` → IPv4 → TCP → socket wake-up. So the
  critical section is *the whole receive-side network stack*. Even if you spawned N
  consumer threads tomorrow, they would serialise here.

### 3.4 Verdict on receive

Receive multi-queue is **category (a): genuinely impossible without stack changes.**
Not "awkward", not "capped" — there is no expressible way for a driver to say "I have
4 receive queues", no second thread to drain them, and one lock that would serialise
them if there were.

---

## 4. The transmit path: **not** blocked

This is the finding that changes the shape of the plan, and it is the opposite of
what §5 of the optimization plan assumed.

**There is no lock on the stack's transmit path, and `send_data` runs on the
caller's own thread.** Traced end to end:

- `datalink.cpp:414-416` — `datalink_send_routed_data()` calls
  `datalink->first_info->send_data(...)` with **no interface lock held**. (`fLock` is
  taken all over `interfaces.cpp` for address management; it is *not* taken here.)
- `datalink_protocols/ethernet_frame/ethernet_frame.cpp:141-165` —
  `ethernet_frame_send_data()` prepends the header and calls straight through. No lock.
- `datalink.cpp:697-713` — `interface_protocol_send_data()` calls
  `protocol->device_module->send_data(protocol->device, buffer)` directly. No lock.
- `devices/ethernet/ethernet.cpp:286-298` — `ethernet_send_data()` issues
  `ioctl(fd, ETHER_SEND_NET_BUFFER, ...)`.
- `device_manager/devfs.cpp:1450` — `devfs_ioctl()` takes **no lock**; it forwards to
  `device->Control()`.

So N userland threads on N cores calling `write()` on N sockets arrive **concurrently**
inside `ena_send()`. The one and only serialisation point is the driver's own:

- `ena.cpp:2295` — `MutexLocker locker(device->txLock);`

**Transmit multi-queue is category (c): already possible, simply not done.** A driver
that keeps N TX rings and picks one by `smp_get_current_cpu()` needs no stack change
at all. Whether that is *worth* anything is §6.3 — and the answer is "less than you'd
hope".

### 4.1 What is still blocked on transmit

Two things, both previously established and both re-confirmed:

- **Doorbell coalescing.** `ethernet.cpp:295` is one ioctl per `net_buffer` with no
  "more coming" signal, so `ena.cpp:2483`'s per-frame
  `ena_com_write_sq_doorbell()` cannot be deferred — a deferred doorbell with no
  successor is a frame never sent. Category (b): needs a batched transmit entry point.
  Note the driver is already *positioned* for it — the
  `ena_com_is_doorbell_needed()` check sits in the correct place at `ena.cpp:2454`,
  per the comment at `:2442-2453`.
- **TX checksum offload.** `net_buffer.h:19-21` has RX-validity bits only. Category (b),
  and small: one `NET_BUFFER_*_CHECKSUM_NEEDED` pair plus a capability
  advertisement — but it requires the stack to *stop* computing the checksum, which
  means a per-device capability negotiation, not just a flag.

---

## 5. Classification summary

| Item | Category | Evidence |
|---|---|---|
| N RX hardware queues drained concurrently | **(a) impossible without stack change** | `net_device.h:54`, `device_interfaces.h:31`, `device_interfaces.cpp:538`, `:128` |
| MSI-X vectors on different CPUs | **(a) impossible without kernel change** | `arch/arm64/arch_int.cpp:62-67`, `gicv3_its.cpp:129-131,626`, `interrupts.cpp:673` |
| N TX queues fed from N CPUs | **(c) already possible** | `datalink.cpp:710`, `devfs.cpp:1450`, `ena.cpp:2295` |
| RSS indirection spread across N RX queues | **(c) already possible** (and useless alone) | `ena.cpp:830-838` |
| Per-queue driver refactor (arrays, per-queue locks) | **(c) already possible** | `ena.h:285-318` |
| TX doorbell coalescing | (b) batched TX entry point, small-to-medium | `ethernet.cpp:295`, `ena.cpp:2473-2483` |
| TX checksum offload | (b) flags + capability negotiation, medium | `net_buffer.h:19-21` |
| RX zero-copy | (b) `net_buffer` foreign-data API, medium | `net_buffer.cpp:65-72`, `create_data_header()` `:801`, `release_data_header()` `:824` |
| RX buffer sized to a `net_buffer` node | **(c) already possible, one line** | `ena.h:98` vs `net_buffer.cpp:149,1700` |

---

## 6. The honest partial-win assessment

### 6.1 First, the arithmetic that reorders everything

From `throughput-measurement.md`: receive 4936 Mbit/s at 2115 µs/MiB, on a
**2-vCPU** c7g.large.

```
4936 Mbit/s              = 588.4 MiB/s
588.4 MiB/s × 2115 µs/MiB = 1,244,466 µs of CPU per second
2 vCPU                    = 2,000,000 µs of CPU per second
                          → 62 % of the whole machine
```

Per frame, at MTU 9001:

```
588.4 MiB/s ÷ 9001 B      ≈ 68,500 frames/s
1,244,466 µs ÷ 68,500     ≈ 18.2 µs of CPU per frame
× 2.6 GHz (Neoverse V1)   ≈ 47,000 cycles per 9 KB frame
```

> **CORRECTION (2026-08-24): the per-frame framing below is wrong, and it undercuts
> §6.** A later 15-point MTU sweep fits the cost as **2.34 µs/frame + 1.85 ns/byte**
> (R² 0.94), so at MTU 9001 the split is **12% per-frame / 88% per-byte**. Dividing
> total CPU by frame count, as done here, silently attributes the per-byte 88% to the
> frame and makes per-frame work look like the lever. It is not. Anything in this
> document that argues from "cycles per frame" — including the §6 rankings — is
> targeting the smaller half of the cost. The per-byte path is where the remaining
> ~10× of *cost* sits, and most of it is still not itemised (the two bounce copies
> account for at most 0.78 of the 1.85 ns/B, and it is not memory bandwidth). See
> `net-receive-profile.md`. The arithmetic below is retained because the *totals* are
> correct and the thread occupancy conclusion still holds. **But note (2026-08-25):
> per-byte cost is not what caps receive throughput — the machine is 97.3% idle at the
> ceiling. The limiter is bufferbloat in the receive FIFO + `TCPEndpoint::fLock`
> (`ena-receive-latency-account.md`).**

**47,000 cycles to receive one 9 KB frame is roughly an order of magnitude more than
it should be.** Receive is spread across exactly two threads (reader and consumer),
so on a 2-vCPU box those two threads are each running at ~62 % occupancy and the
ceiling ~~is around 8 Gbit/s of pure CPU~~ **on `c7g.large` is around 8 Gbit/s of pure
CPU** — *with both cores fully consumed and nothing left for the application*.

> ### The "~8 Gbit/s ceiling" — corrected 2026-08-24, and this is the lesson to keep
>
> **The arithmetic is right; calling it "the ceiling" was the error.** It is the
> ceiling **on `c7g.large`**, a 2-vCPU instance: the number says *this box runs out of
> CPU at about 8 Gbit/s*. It says nothing about the device or the driver on any other
> class. Written without its instance class in the conclusion, it was then read as a
> driver ceiling — and a `c7g.16xlarge` subsequently measured **10.2 Gbit/s** against
> Linux's **29.8**, which "8 Gbit/s" would have ruled out.
>
> **Rule: a saturation or ceiling figure without its instance class is not a finding,
> it is a trap.** The class was present in the working two paragraphs above and absent
> from the sentence people quote.

Two conclusions follow:

1. **On c7g.large, multi-queue is provably worthless.** *(Correct — and the reason
   generalised further than this argument could see. Multi-queue is worthless on
   `c7g.16xlarge` too, where there are plenty of spare cores: Linux on **one** queue
   does 29826 Mbit/s. So the conclusion holds fleet-wide, but **not** for the
   "no third core" reason given here.)* There is no third core to put
   a third queue's work on, and the device on a 2-vCPU instance is unlikely to grant
   more than 2 pairs anyway ~~(**[needs hardware]**, §7 step 0)~~ — **answered: the
   grant is a fixed 8 across the whole C7g family, independent of vCPU count.**
2. **Per-frame cost is the lever with roughly 10× of headroom in it.** Parallelism has
   at most `ncpus` in it, and only on an instance we are not currently testing on.

### 6.2 The one that actually pays, is one line, and is not blocked

**`ENA_PACKET_BUFFER_SIZE` is 2048 (`ena.h:98`). The largest contiguous run a fresh
`net_buffer` data node can hold is 1952.** Therefore *every single* receive segment
append fragments across two nodes and takes the slow path.

Derivation, from two independent sites that agree:

- `net_buffer.cpp:42` `BUFFER_SIZE 2048`; `:147-149`
  `DATA_HEADER_SIZE = _ALIGN(sizeof(data_header))` = 40,
  `DATA_NODE_SIZE = _ALIGN(sizeof(data_node))` = 56,
  `MAX_FREE_BUFFER_SIZE = 2048 - 40` = 2008.
- `create_data_header()` sets `tail_space = BUFFER_SIZE - DATA_HEADER_SIZE - headerSpace`;
  with `headerSpace = DATA_NODE_SIZE` that is **2048 − 40 − 56 = 1952**.
- `append_size()` `net_buffer.cpp:1700` independently computes
  `sizeUsed = MAX_FREE_BUFFER_SIZE - headerSpace` = 2008 − 56 = **1952**.
- `create_buffer()` `net_buffer.cpp:1100-1105` clamps `create(0)`'s header space *up*
  to `DATA_NODE_SIZE`, so the **first** node has 1952 of tail space too.

Consequence, per 9001-byte frame in `ena_receive()` (`ena.cpp:2684`, `:2702-2704`):
the device fills 5 descriptors (4 × 2048 + 809). Each 2048-byte `append()` exceeds
1952, so `append_size()` takes the `node->TailSpace() < size` branch
(`net_buffer.cpp:1696-1754`), allocates an **extra** `data_header`, and returns
`*_contiguousBuffer = NULL` (`:1747`) — which makes `append_data()`
(`net_buffer.cpp:1785-1792`) fall through to `write_data()`, walking the node list
instead of a single `memcpy`.

So we pay, per jumbo frame: **~9 `data_header` slab allocations + ~9 `data_node`
allocations** where 5 of each would do, **plus** five node-walking `write_data()`
calls instead of five flat `memcpy`s.

**Fix: set `ENA_PACKET_BUFFER_SIZE` to 1920.** (1920 ≤ 1952, and 1920 = 30 × 64 so
each DMA slot stays 64-byte/cache-line aligned inside the contiguous area carved by
`ena_allocate_buffer_area()`.) Then every segment lands contiguously in exactly one
fresh node and takes the `memcpy` fast path. Frame descriptor count is unchanged:
`ceil(9001 / 1920) = 5`, same as `ceil(9001 / 2048) = 5`.

**Why I believe this is worth real percent, stated as a hypothesis not a result:**
Haiku's slab allocator is not free, `write_data()`'s node walk is pointer-chasing
across freshly-allocated cache-cold memory, and this happens 68,500 times a second on
the single busiest thread in the system. I have **not** measured it. It is a one-line
change and `nettput` measures exactly the number it should move (RX µs/MiB), so the
cost of finding out is one bake.

**Risks, all checkable statically:**
- `ena_calculate_frame_limits()` (`ena.cpp:691`) caps `frameSize` so a max frame fits
  in `rxMaxDescriptors × ENA_PACKET_BUFFER_SIZE`. With 1920 that needs
  `rxMaxDescriptors ≥ 5` (5 × 1920 = 9600 ≥ 9015). `ENA_MAX_PACKET_DESCRIPTORS` is 8
  (`ena.h:117`), so the binding constraint is the device's own
  `max_per_packet_rx_descs`, which is **already logged** at `ena.cpp:470`. Confirm it
  is ≥ 5 before shipping, or the MTU silently drops below 9001. **[needs hardware —
  or an existing boot log]**
- Nothing requires `ENA_PACKET_BUFFER_SIZE` to be a power of two: it is used as a
  `memcpy`/descriptor length and a stride (`ena.h:98` comment, `ena.cpp:2323`,
  `:2387-2397`, `:2664-2669`). The *ring* depths must be powers of two; the buffer
  size need not.
- The same constant sizes the TX bounce slots. `ena_send()`'s segment count
  (`ena.cpp:2323`) rises for a jumbo frame from 5 to 5 — unchanged — but
  `ENA_MAX_PACKET_DESCRIPTORS` headroom shrinks slightly. Still 8 ≥ 5.

### 6.3 Multiple TX queues fed from multiple CPUs — real, but buys nothing *yet*

Not blocked (§4). But be honest about when it pays:

- The contended resource is `device->txLock` (`ena.cpp:2295`). A **single** TCP stream
  has exactly one thread in `ena_send()` at a time, so contention is **zero** and N TX
  queues save **nothing** on the benchmark as currently run.
- It pays only with ≥ 2 concurrently-sending threads on ≥ 2 cores. On c7g.large those
  two cores are already ~62 % consumed by receive-side work during a bidirectional
  test, so even the multi-stream case has little room.
- It also does *not* fix the per-frame doorbell (§4.1), which is the actual per-frame
  transmit overhead.

**Verdict: build it as part of the refactor because it is free once the per-queue
struct exists, but do not expect it to move a number, and do not sequence it ahead of
§6.2.**

### 6.4 RSS with N hardware RX queues drained by one consumer — buys nothing

Considered carefully, and the answer is no, for three reasons:

1. **It cannot distribute interrupts**, because nothing can (§2). N vectors → CPU 0.
2. **Interrupt cost is not the bottleneck anyway.** `ena_io_interrupt()`
   (`ena.cpp:191-237`) is two `release_sem_etc()` calls and one MMIO write. With
   `ENA_RX_IRQ_INTERVAL` at 20 µs (`ena.h:150`) the rate is bounded at 50 k/s, and the
   reader already collapses the backlog with `get_sem_count()` +
   `acquire_sem_etc(..., count, ...)` at `ena.cpp:2604-2607`. There is no measurable
   ISR cost to redistribute.
3. **It is actively negative for the single consumer.** Draining N completion queues
   from one thread means scanning N rings per call, N × 1024 descriptors and N × 2 MiB
   of buffer area in the working set, for identical per-frame work. More cache
   pressure, same throughput.

**Verdict: this buys nothing. Do not build it as a stepping stone.** It is only worth
doing at the same time as the stack work that consumes it.

### 6.5 Per-CPU interrupt affinity for the *existing single* queue — buys nothing

Even if §2 were fixed, moving the one IO vector from CPU 0 to CPU 1 does not reduce
work; it relocates an ISR that costs ~microseconds per 20 µs interval. It might
marginally reduce interference with the reader thread if the scheduler happens to put
the reader on CPU 0 — an effect of the same order as scheduling noise, on a 2-vCPU
box where there is nowhere else for the reader to go. **Not worth doing for its own
sake.**

### 6.6 RX zero-copy — the other big one, but it is stack surgery

`ena_receive()`'s comment at `ena.cpp:2504-2510` is correct that
`net_buffer_module_info` has no way to take ownership of driver memory. But the
groundwork is closer than the comment suggests: `data_header` already carries
`ref_count` **and** `physical_address` (`net_buffer.cpp:65-72`, the latter with a
`// TODO: initialize this correctly` in `create_data_header()`, `net_buffer.cpp:801`).

What is missing is a `data_header` variant whose backing store is foreign and whose
release calls back into the owner, plus an `append_foreign_data()` module entry. Size:
~150–250 lines in `net_buffer.cpp`, one new slot in
`headers/private/net/net_buffer.h`, and a driver rework to post a *replacement* DMA
buffer instead of recycling the same slot (which trades a 9 KB copy for an allocation,
so the driver needs a small free-list to stay ahead).

**Risk: high.** `net_buffer` is used by every protocol and every driver; a refcount
error there is a kernel-wide use-after-free, and unlike a driver bug it is not
contained to the network interface. Sequence this **after** §6.2 has told us how much
of the 18 µs was allocator/copy cost in the first place — §6.2 is the cheap
experiment that sizes this expensive one.

### 6.7 Things I considered and rejected outright

- **Publish N `/dev/net/ena/*` nodes, one per queue pair, so the stack makes N
  interfaces (N reader + N consumer threads for free).** Rejected: each becomes a
  separate `net_device` with its own index, IP configuration and ARP state
  (`device_interfaces.cpp:213`, `get_device_interface_address()` `:315-332`), all
  sharing one MAC. It is not one logical link, RSS cannot steer to it correctly, and
  it would break routing and neighbour discovery. This is a trap, not a shortcut.
- **Have the driver call `device_enqueue_buffer()` directly from N threads to push
  frames in.** It *is* exported in the stack module (`stack.cpp:962`, declared
  `net_stack.h:135`, defined `device_interfaces.cpp:829-845`). Rejected on three
  counts: the driver has no `net_device*` (the ethernet device module owns it); it
  takes the global `sLock` and walks a linked list **per packet**
  (`get_device_interface()` `:421-438`); and it still funnels into the one FIFO and
  the one consumer thread. It moves the boundary without removing it.
- **Software receive steering (RPS-style).** Explicitly contraindicated by the AWS
  guidance already captured in `graviton-optimization-plan.md` §5 item 2 — do the
  steering in hardware, RPS "is not needed on Graviton2 and newer".

---

## 7. Sequenced plan — VOID except for step 2 (see banner at top of file)

> **Do not work this plan (2026-08-24).** Status of each step:
>
> | Step | State |
> |---|---|
> | **0** — settle three facts | **ANSWERED.** The queue grant is a **fixed 8 across the whole C7g family**, not a function of vCPU. And SMP *is* live — the "if only CPU 0 is online" worry below is resolved: `c7g.metal` brings up **64 of 64 CPUs**, and the scheduler-placement work measured a **9.9×** speed-up at 32 threads on 16 CPUs, which is not possible on one core. |
> | **1** — the `ENA_PACKET_BUFFER_SIZE` / `net_buffer` data-node mismatch | **SHIPPED** — `017f72cecd` "ena: post 1920 byte receive buffers so segments fit a net_buffer node". This was the best call in this document. |
> | **2** — move the interrupt unmask after the ring drain | **STILL OPEN as a correctness fix — NOT the throughput lever (corrected 2026-08-25).** `ena.cpp:221`. The receive ceiling is bufferbloat in the receive FIFO + `TCPEndpoint::fLock` (`ena-receive-latency-account.md`), not the unmask order. Driver-only and hot-swappable — but **verify the swap took effect with a compiled-in version stamp**, not by watching a number move (`ena-multiqueue-headroom.md` §6). |
> | **3** — per-queue struct refactor + N TX queues | **VOID.** Multi-queue is cancelled on evidence. |
> | anything else premised on parallelism | **VOID.** |
>
> Ordered so that everything shippable and low-risk precedes anything requiring deep
> surgery, and so that each step's *measurement* informs whether the next is worth it.
> That ordering principle was sound, and it is why step 1 shipped and step 3 never did.

### Step 0 — Settle three facts from data we may already have. **Zero code.**

| Question | How | Why it gates everything |
|---|---|---|
| How many IO queue pairs does the test instance grant? | Already printed at `ena.cpp:465-471`; grep an existing `aws ec2 get-console-output` capture for `queue counts (ext`. | If it is 2 on c7g.large, multi-queue's ceiling there is 2. |
| What is `max_per_packet_rx_descs`? | Same log line, `ena.cpp:470`. | Gates step 1 (needs ≥ 5). |
| **How many CPUs does Haiku actually bring online on Graviton?** | `sysinfo`, or `nettput`'s `systemInfo.cpu_count` (`src/bin/nettput/nettput.cpp:104`), or the kernel's boot CPU count. | **The whole premise.** SMP is wired (`boot/platform/efi/arch/arm64/arch_smp.cpp:182-206` PSCI `CPU_ON`; MADT GICC enumeration at `arch_acpi.cpp:180-184`) but I found **no verification of it anywhere in `graviton/docs/`**. If only CPU 0 is online, every parallelism item in this document is worth exactly zero and the *real* next project is arm64 SMP bring-up. |

**Risk:** none. **Size:** none. **Recommendation:** also plan to move the benchmark to
a **c7g.4xlarge or larger** before any multi-queue work, because on 2 vCPUs the
result is unmeasurable by construction.

### Step 1 — Size the RX buffer to a `net_buffer` data node. **Ship this first.**

- **Changes:** `ENA_PACKET_BUFFER_SIZE` 2048 → 1920, with a comment recording *why*
  (the 1952-byte node payload derived in §6.2) so the next person does not "tidy" it
  back to a power of two.
- **Files:** `src/add-ons/kernel/drivers/network/ether/ena/ena.h:98`.
- **Size:** 1 line + ~8 lines of comment.
- **Risk:** **low**, one caveat: confirm `max_per_packet_rx_descs ≥ 5` (step 0) or the
  negotiated MTU silently drops below 9001. `ena_calculate_frame_limits()`
  (`ena.cpp:691`) will do the right thing either way — it will just do it quietly.
- **Verify:** `nettput` receive, 512 MiB × 3, MTU 9001, on one boot with the existing
  interleaved-control methodology. **Watch `µs/MiB`, not Mbit/s** — the rate is
  window-limited, the CPU cost is what should move. Confirm MTU is still 9001 via
  `ifconfig` and `ETHER_GETFRAMESIZE` in the boot log before trusting the number.

### Step 2 — Move the interrupt unmask after the ring drain.

This is the `XXX STRUCTURAL FIX STILL OWED` already recorded at `ena.cpp:219-228`.
Today the vector is re-armed while every completion is still unconsumed, so
moderation is the only thing preventing an interrupt per completion.

- **Changes:** remove the unmask from `ena_io_interrupt()` (`ena.cpp:229-234`); re-arm
  at the end of the drain in `ena_receive()` and `ena_reclaim_transmitted()`. The
  vector is shared, so both directions need a shared "who re-arms" rule — simplest
  correct answer is an atomic in-flight counter, re-arm when it reaches zero.
- **Files:** `ena.cpp` (`ena_io_interrupt`, `ena_receive`, `ena_reclaim_transmitted`,
  and the two other unmask sites — bring-up at `:1846-1850` and `ena_open()` at
  `:2161-2165`), `ena.h` (one counter field). All three sites are found by
  `grep -n ena_com_unmask_intr ena.cpp`.
- **Size:** ~40–60 lines.
- **Risk:** **medium.** Getting it wrong means a vector that is never re-armed — the
  interface silently goes dead. Test with `ena_fault` reset injection and a long soak.
  The existing `ioInterrupts` counter (`ena.h:274`) makes the rate observable.
- **Verify:** interrupt count per MiB should fall; `nettput` µs/MiB should improve or
  hold. Soak ≥ 30 min plus a fault-injected reset to prove re-arming survives it.

### Step 3 — ~~Per-queue struct refactor + N TX queues~~ **VOID — do not do this**

> **CANCELLED 2026-08-24.** Its own guard condition ("only if the instance has ≥ 4
> vCPUs and ≥ 4 queue pairs") is *satisfied* on the larger classes — which is exactly
> why the guard was not enough. A `c7g.16xlarge` has plenty of both, and multi-queue
> still buys nothing there: **Linux on one queue does 29826 Mbit/s.** The guard tested
> for *capacity to parallelise*, never for *whether parallelism was the constraint*.

~~Do the refactor *now* only if step 0 says the instance has ≥ 4 vCPUs and ≥ 4 queue
pairs; otherwise defer, because it is unmeasurable and it multiplies the reset and
descriptor-reclaim paths.~~

- **Changes:** promote `ena.h:285-318` into a `struct ena_io_queue` (SQ/CQ pointers,
  lock, buffer pool, free-id stack, semaphore, MSI-X vector), **cache-line aligned**
  to avoid false sharing. Array it. Loop `ena_create_queue_pair()` in
  `ena_setup_io_queues()` (`ena.cpp:1055`) over N pairs at `(2i, 2i+1)`. Request
  `min(device_max, ncpus) + 1` vectors instead of `ENA_MSIX_VECTOR_COUNT`
  (`ena.cpp:53-55, 287-295`). Select the TX queue in `ena_send()` by
  `smp_get_current_cpu()`. **Keep receive on queue 0 only** and keep the RSS table
  pointed entirely at it (`ena.cpp:830-838` unchanged) — see §6.4.
- **Files:** `ena.h`, `ena.cpp` (bring-up, teardown, reset unwind, `ena_send`,
  `ena_receive`, `ena_open`/`ena_close`, `ena_release_buffers`).
- **Size:** **large**, ~600–900 lines touched. This is where most of the mechanical
  risk in the whole programme lives.
- **Risk:** **high**, concentrated in the reset and teardown paths, which currently
  hold `resetLock → txLock → rxLock` (`ena.cpp:2079-2130` documents the ordering) and
  would become `resetLock → N locks`. Lock-order discipline across N queues is the
  thing that will bite.
- **Verify:** correctness first — traffic still works, `ena_fault` reset injection
  survives N queues, no leak across repeated resets. Then multi-stream `nettput`
  (several concurrent connections) to see whether `txLock` contention was ever real.
  Expect **no single-stream change at all**; if you see one, be suspicious.

### Step 4 — arm64 IRQ affinity. **Kernel work, and valuable beyond ENA.**

A prerequisite for any interrupt-side benefit, and it fixes a whole-machine gap: today
*every* interrupt on this port lands on CPU 0.

- **Changes, in order:**
  1. `GICv3ITS`: one collection per CPU (replace the single `_MapCollection(0, …)` at
     `gicv3_its.cpp:129-131`); add `GITS_CMD_MOVI` (0x01) to `gicv3_regs.h:157-163`;
     add `_MoveInterrupt()`. The vector → (deviceID, eventID) reverse map already
     exists as `fVectorDevice` / `fVectorEvent` (`gicv3_its.h:101-102`). Also fix
     `fCollectionTarget` selection, which currently takes the *first* redistributor
     frame found (`gicv3_its.cpp:376-382`) and merely assumes it is the boot CPU's,
     rather than matching `MPIDR_EL1` the way `_CurrentRedistributor()` does
     (`arch_int_gicv3.cpp:169-189`).
  2. `GICv3InterruptController::SetInterruptTarget(irq, cpu)` writing
     `GICD_IROUTER + irq*8` for SPIs, with an LPI/SPI discriminator
     (`irqnr >= GIC_LPI_BASE`, as at `arch_int_gicv3.cpp:290`).
  3. Replace the stub at `arch/arm64/arch_int.cpp:62-67` and return the **actual**
     CPU, not 0.
  4. Fix the shared-`irq_assignment` limitation at `interrupts.cpp:673` so individual
     MSI-X vectors of one device are separately assignable.
- **Files:** `src/system/kernel/arch/arm64/gicv3_its.{cpp,h}`, `gicv3_regs.h`,
  `arch_int_gicv3.{cpp,h}`, `arch_int.cpp`, `src/system/kernel/interrupts.cpp`.
- **Size:** ~400–600 lines.
- **Risk:** **high, and the blast radius is the whole machine** — a mis-routed
  interrupt is an unbootable instance on a console-less host. Note two latent hazards
  the stub is currently masking, both worth fixing here: `assign_io_interrupt_to_cpu`
  early-returns when `newCPU == oldCPU` **before** unlinking the assignment
  (`interrupts.cpp:735-736`), which means `CPUEntry::Stop()`
  (`scheduler/scheduler_cpu.cpp:111-124`) would spin forever; and
  `interrupts.cpp:713-717` panics when freeing a vector still assigned to a CPU.
- **Verify:** the scheduler's own `int_load` debugger command
  (`interrupts.cpp:131-158`) shows per-CPU distribution. Then confirm ENA's
  `ioInterrupts` still advances and a fault-injected reset still re-arms. Do this on a
  disposable instance with serial console capture, never on the canonical AMI path.
- **Prerequisite:** step 0's SMP answer. This is meaningless with one CPU online.

### Step 5 — Multi-queue receive in the network stack. **The actual blocker.**

- **Changes (minimal correct design):**
  - `headers/private/net/net_device.h`: add `uint32 rx_queue_count` to `net_device`
    (default 1) and `receive_data_queue(net_device*, uint32 queue, net_buffer**)` to
    `net_device_module_info` — a **new slot, not a change to `receive_data`**, so
    every other device in the tree keeps working untouched.
  - `headers/private/net/ether_driver.h`: a new `ETHER_RECEIVE_NET_BUFFER_QUEUE`
    ioctl beside `ETHER_RECEIVE_NET_BUFFER` (`:30`), carrying a queue index.
  - `devices/ethernet/ethernet.cpp`: probe for it in `ethernet_up()` the same way
    `supports_net_buffer` is probed (`:191-195`), and publish
    `rx_queue_count`.
  - `stack/device_interfaces.h`: `reader_thread` (:31) → an array; `receive_queue`
    (:49) and `consumer_thread` (:47) → arrays.
  - `stack/device_interfaces.cpp`: spawn N readers in `up_device_interface()`
    (:538) and join N in `down_device_interface()` (:580); N FIFOs in
    `allocate_device_interface()` (:192); N consumers (:206).
  - **The lock.** `receive_lock` (`device_interfaces.cpp:128`) must stop serialising
    dispatch. `receive_funcs` is mutated only by register/unregister
    (`:659-721`), so an `rw_lock` held for *reading* during dispatch is the right
    shape. This is the subtle part: `monitor_lock` and the `deframe_func` refcount
    have the same read-mostly shape and the same recursive-locking assumptions, and
    ARP / IPv6-datagram / the device monitors all sit under them.
- **Files:** the six above, plus a sweep of every `net_device_module_info` initialiser
  in the tree (loopback, `devices/dialup`, `devices/tunnel`, the FreeBSD compat layer)
  to add the NULL slot.
- **Size:** ~300–500 lines, spread thin across the whole networking subsystem.
- **Risk:** **high and broad.** This changes a module interface every network device
  implements and a lock every receive path holds. It is also the item most likely to
  be rejected as gratuitous by anyone reading it without §6.1's arithmetic in hand.
- **Verify:** first that a **single**-queue device is bit-for-bit unaffected (loopback,
  then ENA with `rx_queue_count == 1`). Only then N queues with RSS spread — at which
  point step 3's RSS loop (`ena.cpp:830-838`) finally changes and `ena_receive()`
  becomes per-queue.
- **Prerequisites:** steps 0, 3 and 4. Without step 4, N reader threads all wake from
  interrupts delivered to CPU 0.

### Step 6 — Deferred, independently scoped

- **Batched transmit entry point** → unlocks doorbell coalescing (§4.1). Independent
  of everything above; the driver is already positioned for it (`ena.cpp:2454`).
- **RX zero-copy via a `net_buffer` foreign-data API** (§6.6). Sequence *after* step 1
  has quantified how much of the 18 µs/frame is allocator and copy cost.
- **TX checksum offload** (§4.1). Smallest of the three, needs capability negotiation.

---

## 8. What this analysis did not verify

Stated plainly so nothing here is mistaken for a measured result.

- **Nothing was built and nothing was booted.** Another agent holds the build tree.
- **The step 1 hypothesis is unmeasured.** The *arithmetic* (1952 < 2048, hence
  fragmentation on every append) is solid and independently confirmed at
  `net_buffer.cpp:1700` and in `create_data_header()`. The *magnitude* of the win is a
  hypothesis.
- **Whether Haiku brings more than one CPU online on Graviton is unknown to me.** The
  code path exists; I found no evidence in `graviton/docs/` that it has been observed
  working. This is step 0 and it gates roughly everything.
- **The device's actual queue-pair grant on our instances is unknown to me**, though
  the driver already logs it.
- **`ena-com/` was not modified and needs no modification** for any step here. If a
  future step appears to need one, that is a decision to escalate, not to make.
