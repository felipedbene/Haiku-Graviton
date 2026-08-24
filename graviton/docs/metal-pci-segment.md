# c7g.metal: no PCI, because its one ECAM region is numbered 1

Status: **FIXED, merged and hardware-verified 2026-08-24** — `f5367b3602`
"pci/ecam: give each root bridge the ECAM region for its own buses", merged via
`e270548f33`. On `c7g.metal` this enumerates **53 PCI devices across buses 0–4**
(matching Linux `lspci` on the same host), publishes NVMe, mounts its root volume
and reaches userland. On the 96-vCPU `c8g` class it is what makes the ENA visible
at all.

**`pci_segment != 0` is no longer "the next blocker" anywhere.** If you were sent
here by a doc that says metal does not boot, that doc is stale — say so.

This was the blocker that followed the GICv3 fix (`metal-gicv3-panic.md`).

> **Tense corrected 2026-08-24.** The next sentence used to read "… `c7g.metal`
> **still does not** reach userland", directly contradicting the status line four
> lines above it. It is kept in the past tense because the failure description that
> follows is the valuable part.

With the interrupt controller working, all 64 CPUs up and the ITS ready,
`c7g.metal` **still did not** reach userland.

## The failure (CONFIRMED, observed on two hosts)

```
PCI: pci_module_init
ACPI: MCFG 0x0000000015E1D898 00003C (v01 AMAZON GRVTN003 00000000 AMZN 20200601)
+ECAMPCIController::InitDriver()
initialize PCI controller from ACPI
PCI: range from ACPI [0(1),ff(1)] with length 100
PCI: range from ACPI [e0000000(1),ffffffff(1)] with length 20000000
PCI: range from ACPI [1e000000000(1),1ffffffffff(1)] with length 2000000000
PCI: mechanism addr: e010000000, seg: 1, start: 0, end: ff
PCI: multiple segments not supported!driver busses/pci/ecam/driver_v1 init failed: General system error
...
publish device: node ..., path acpi/namespace, module bus_managers/acpi/namespace/device_v1
publish device: node ..., path acpi/call, module bus_managers/acpi/call/device_v1
PANIC: did not find any boot partitions!
```

No PCI controller means no PCI bus, so no `disk/nvme/*` is ever published — a
working guest publishes `disk/nvme/0/raw` and friends at this point — so there is
no boot device and the kernel panics. The exact failing check is
`ECAMPCIControllerACPI::ReadResourceInfo()` in
`src/add-ons/kernel/busses/pci/ecam/ECAMPCIControllerACPI.cpp`:

```c
if (alloc->pci_segment != 0) {
        dprintf("PCI: multiple segments not supported!");
        continue;
}
```

The loop then falls out and returns `B_ERROR`.

## The diagnosis, and why the message is misleading

**There is exactly one ECAM region on this machine. It is merely numbered 1.**
Two independent artefacts say so:

* The MCFG table is `0x3c` = 60 bytes. An MCFG is a 44-byte header (36-byte
  standard ACPI header plus 8 reserved bytes) followed by 16-byte allocation
  entries. 60 − 44 = 16, so **one** entry.
* `ReadResourceInfo()` has a second warning immediately above, `"multiple host
  bridges not supported!"`, which fires when `alloc + 1 != end`. It appears
  **zero** times on either metal host — grep-confirmed on both consoles. So the
  code itself agrees there is one entry.

So the emitted diagnostic is wrong about what happened: nothing here has multiple
segments. A machine with a single ECAM region whose segment group number is 1
rather than 0 was rejected outright.

Why a non-zero number is harmless: an ECAM config address is

```
(bus << 20) | (device << 15) | (function << 12) | offset
```

relative to that region's own base, and contains no segment field. A segment
group number only says *which base* an address belongs to, so it matters only
when there is more than one base to choose between. One region therefore
describes the machine completely, whatever it is numbered.

## Verified before writing the fix, not assumed

* **Does anything downstream assume segment 0?** No. Outside ACPICA's own table
  headers, `pci_segment` occurs in exactly two places in the entire tree — the
  `dprintf` and the rejected comparison, both in this one function. No segment
  number is carried into the PCI bus manager, the device manager or the ACPI IRQ
  routing code. There is nothing to thread a segment through, which is also why
  a genuinely multi-region machine cannot be supported by simply accepting them
  all.
* **Is metal's absent PCI I/O window tolerated?** Yes, by construction. Metal's
  `_CRS` yields a bus-number range, a 32-bit MMIO window and a 64-bit MMIO
  window, where a guest also yields an I/O port range. Every consumer filters on
  range type: `PCI::InitDomainData()` and `PCI::_AddDomain()` both skip
  non-`B_IO_PORT` entries and simply never map an I/O area, and
  `PCI::LookupRange(B_IO_PORT, ...)` returns `B_ENTRY_NOT_FOUND`, which
  `pci_io.cpp` already checks for. A device requesting an I/O BAR would fail to
  get one, which is correct on a machine with no I/O space; NVMe and ENA are
  MMIO-only. Verified by inspection — the boot is what proves it.

## The fix

Enumerate and log every MCFG region; **prefer segment 0 where one exists**, so
that no machine which works today changes behaviour at all; otherwise use the
first region. Warn only when there is genuinely more than one region, which is
the thing that is actually unsupported, and say so accurately.

The segment-0 preference is deliberate: the previous loop would skip a leading
non-zero segment and settle on a later segment-0 entry, and this code also runs
on x86 via `X86PCIControllerMethPcie`. Preferring segment 0 preserves that
outcome exactly, so the only behavioural change is on machines that have no
segment 0 — which is precisely the broken case.

A partial trailing entry is also no longer read: the loop bound is
`alloc + 1 <= end` rather than `alloc < end`.

## What this does not do

It does not add multi-segment support. A machine with two or more ECAM regions
still uses one of them and now says clearly that it is ignoring the rest, rather
than claiming multiple segments are unsupported while rejecting a single one.
Real support would mean carrying a segment from the controller through the bus
manager to every config-space access, and nothing in the tree carries one today.

## Verified on hardware

`ami-0200e8f97df35c16c`, `c7g.metal`:

```
PCI: [dom 0, bus  0] bus 0, device 0, function 0: vendor 1d0f, device 0200
PCI: [dom 0, bus  1] bus 1, device 0, function 0: vendor 1d0f, device cec2
...  53 devices in total, across buses 0-4
publish device: ... path disk/nvme/0/raw, module drivers/disk/nvme_disk/device_v1
Identified boot partition by partition offset.
bfs: mounted "Haiku" (root node at 131072, device = /dev/disk/nvme/0/1)
Mounted boot partition: /dev/disk/nvme/0/1
ena: found an ENA device
... Doing first boot processing #15 for package gcc_syslibs-...
```

The guests are unaffected: `c7g.large` and `c7g.4xlarge` log
`PCI: ecam region: addr 20000000, segment: 0, buses: 0-ff`, choose it, and
enumerate the same four devices as before.

One thing the boots corrected in this change's own diagnostic: a 96-vCPU guest
lists **three** MCFG regions that are all segment 0 and differ only by bus range
(0-0, 1-43, 44-56). Saying "ignoring all but segment 0" there is useless when all
three are segment 0, so the message now names the region taken — segment *and*
bus range. The region chosen is the same one the old code chose, so no behaviour
changed.

## The 96-vCPU class: one MCFG allocation per root bridge

A second, distinct failure in the same function, found on `c8g.24xlarge` and
affecting the whole 96-vCPU class. It boots from disk and looks healthy while
having **no network at all**.

That machine declares **three PCI root bridges**, each with its own `_CRS` bus
range and its own disjoint MMIO windows, and the MCFG holds one allocation per
bridge covering exactly those ranges:

| root bridge | `_CRS` buses | MCFG allocation | MMIO windows |
|---|---|---|---|
| 1 | `0-0` | `0-0` @ `20000000` | `80000000-81ffffff` |
| 2 | `1-43` | `1-43` @ `20000000` | `84000000-8c2fffff`, `400002000000-400041ffffff` |
| 3 | `44-56` | `44-56` @ `20000000` | `8c400000-8e6fffff`, `400042000000-400051ffffff` |

`ReadResourceInfo()` runs once per bridge, and every bridge read the whole MCFG
and took the first entry it liked. So all three mapped bus 0, all three
enumerated the *same physical devices*, and one NVMe controller was published
three times:

```
3 × [dom 0] bus 0 dev 0/1/4      host bridge, 16550, NVMe
2 × [dom 1] ...the same three
1 × [dom 2] ...the same three
disk/nvme/0/raw   disk/nvme/1/raw   disk/nvme/2/raw
```

`"multiple host bridges not supported!"` fires **three times** on the pre-fix
image, which is the code saying so out loud.

### This also explains a symptom that had been filed as unrelated

`c8g.24xlarge` was recorded earlier in this document's sibling as stalling after
publishing three NVMe devices, and dismissed as *"identical on both images, so
not a regression, a separate previously unverified Graviton4 class"*. That
observation was true and the inference from it was wrong: three bridges each
publishing the same physical controller **is** the stall's cause, so the "extra"
disks and the missing NIC are one bug, not two. "Pre-existing" and "unrelated"
are different claims, and treating the first as the second is how a symptom gets
orphaned from its cause.

### The fix, and the reading it depends on

The bus range each bridge owns was already being decoded and discarded:
`AcpiCrsScanCallbackInt()` built it and then dropped it, because
`ACPI_BUS_NUMBER_RANGE` is `resource_type == 2` and fell into
`default: return B_OK`. `walk_resources()` already runs before the MCFG is
examined, so it is in hand at the moment of the decision. It is now kept, and
each bridge takes the allocation covering its own buses — tightest fit where
several cover them, with the old prefer-segment-0 behaviour as the fallback when
firmware supplies no bus range.

**The base is the address of bus 0, not of the entry's start bus**, and the data
forces that reading rather than the plain ACPI one: three entries report the
*same* base with different start buses, which cannot each mean "the address of my
start bus" without putting three apertures at one physical address, but is
consistent as one aperture based at bus 0 whose buses firmware listed in pieces.
So the mapping begins at the start bus's offset into that aperture and
`ConfigAddress()` rebases absolute bus numbers onto it, as Linux does with
`PCI_MMCFG_BUS_OFFSET()`. This was the one thing here settled by argument rather
than by measurement, and the next boot settled it: bridge 2's devices appeared at
buses `1-0x43`, so **the reading is right** (confirmed 2026-08-24 — see "Hardware
result" below).

> ### Do NOT reach for the GICv3 "one array in pieces" analogy here
>
> **This is the trap this item is most likely to be re-broken by, so it is stated
> as a rule.** The arm64 GICv3 redistributor work dealt with *one* array that
> firmware had described in several pieces, and the right move there was to
> **coalesce** the pieces into the span they cover. **That move is wrong for
> ECAM, and applying it would make the 96-vCPU class worse than it was.**
>
> The difference is what the pieces belong to. On the 96-vCPU machine there are
> **three separate root bridges**, each with its own `_CRS` bus range and its own
> disjoint MMIO windows. They are not three descriptions of one thing. Coalescing
> their allocations into a single `0x00`–`0x56` span and handing that to every
> bridge reproduces the original bug exactly: all three bridges enumerate all the
> same physical devices, and one NVMe controller gets published three times as
> `disk/nvme/0`, `/1` and `/2`.
>
> The correct operation is **per-bridge selection, not union**: each bridge takes
> the allocation covering *its own* buses, which is what `pci_ecam_map_bus()` does
> on Linux's arm64 ACPI path and what `f5367b3602` does here. The single shared
> base address is a property of the *aperture*; the *ownership* is per bridge.

### Why bus numbers nobody claimed are never probed

A bridge's buses may arrive in more than one piece, and the pieces are not
required to tile the range they span. A config read to a bus that nothing decodes
is not guaranteed to return all-ones — on some fabrics it can abort — so each
piece's buses are recorded in a bitmap that `ConfigAddress()` checks, and gaps
are never touched. On the machine that prompted this the three pieces tile
`0x00`–`0x56` exactly, which was verified rather than assumed and was **luck
rather than design**: the next machine may leave holes, and the bitmap makes that
safe by construction instead.

### Containment

Simulated against every measured shape, and metal and the guests come out
unchanged — single allocation, bus offset 0, same base, same 256 MiB, every bus
valid, one piece:

```
c7g.metal        crs=0x0-0xff  -> buses 0x0-0xff   offset 0x0   base 0xe010000000  256 MiB
c7g.large/4xl    crs=0x0-0x0   -> buses 0x0-0xff   offset 0x0   base 0x20000000    256 MiB
c8g bridge 1     crs=0x0-0x0   -> buses 0x0-0x0    offset 0x0   base 0x20000000      1 MiB
c8g bridge 2     crs=0x1-0x43  -> buses 0x1-0x43   offset 0x1   base 0x20100000     67 MiB
c8g bridge 3     crs=0x44-0x56 -> buses 0x44-0x56  offset 0x44  base 0x24400000     19 MiB
```

### Hardware result (CONFIRMED)

`ami-02e994c907fc6484b`. Per-bridge selection works: each bridge mapped its own
allocation, exactly as simulated.

```
PCI: ECAM at 20000000 (bus 0  base 20000000), buses 0-0,   1 decoded,  1 MiB
PCI: ECAM at 20000000 (bus 1  base 20100000), buses 1-43, 67 decoded, 67 MiB
PCI: ECAM at 20000000 (bus 44 base 24400000), buses 44-56,19 decoded, 19 MiB
```

Three of the five pass conditions met outright, and one better than hoped:

* **Exactly one NVMe disk.** `disk/nvme/0/raw` alone, where the pre-fix image
  published `/0`, `/1` and `/2` for one controller. The triplication is gone.
* **No abort while probing**, on any bridge.
* **The stall is fixed.** The pre-fix image stopped after publishing three NVMe
  devices; this one mounts its root volume, reaches `first boot processing #0`
  and goes on into `input_server` add-on loading. That confirms the stall and the
  duplication were one bug, as §above argued.
* `"multiple host bridges not supported!"` fires zero times — but that is *not*
  evidence, because this branch deleted the message. Recorded so nobody counts
  it as a pass later.

**The ENA is still missing**, and the reason was a defect in this branch rather
than anything about the machine: bridges 2 and 3 enumerated *nothing at all*.
The device count stayed at bus 0's three devices across all three domains.

**Enumeration was starting below those bridges' windows.** `PCI::AddController()`
created every domain's root `PCIBus` with its number left at zero:

```c
data.bus = new(std::nothrow) PCIBus {
        .domain = domain,          // .bus omitted, so 0
```

so a bridge whose window begins at bus 1 was probed at bus 0. Its controller
correctly refused — bus 0 is outside its window — `ReadConfig()` returned
`ERANGE`, every device on the probe was skipped, and because the only route to a
non-zero bus is a PCI-to-PCI bridge discovered on bus 0, buses `1-0x43` were
never reached at all.

### A wrong turn worth recording

The first diagnosis blamed the bus-number rebasing and removed it, reasoning that
Haiku's controller interface is window-relative where Linux's is absolute. The
*observation* was right — the root bus really is created at 0 — but the
*conclusion* was wrong twice over:

* The rebasing is what Linux's **arm64 ACPI** ECAM path does.
  `pci_ecam_map_bus()` subtracts `cfg->busr.start` before shifting, and
  `pci_mcfg_lookup()` takes the window base as `e->addr + (bus_res->start << 20)`.
  That is byte-for-byte the `fBusOffset` plus `address + (startBus << 20)` this
  branch already had. The earlier citation of `PCI_MMCFG_BUS_OFFSET` pointed at
  Linux's **x86** `pci_mmconfig` path, which does the opposite — the mechanism was
  right and the reference was for the wrong architecture.
* Removing it would not have restored the ENA anyway. Bridge 2 would still have
  begun enumerating at bus 0, and the valid-bus bitmap would still — correctly —
  have refused it. It would have traded a general mechanism for one that only
  works while every window shares a base, and still missed the goal.

So the rebasing is restored and the root bus is created at the window's first
bus, via an optional `get_bus_range()` on the controller interface. Controllers
that do not implement it get bus 0 exactly as before, which is every other
implementation in the tree. This is the second time on this branch that a Linux
mechanism was reached for without first checking that the surrounding interface
matched; the first was the redistributor stride, where the citation was real but
answered a different question.

~~Whether buses `1-0x56` actually hold an ENA is still unknown, and is the next
boot's question.~~ **ANSWERED 2026-08-24: they do.** The 96-vCPU `c8g` finds its
ENA once each bridge maps its own buses.

### Sibling drivers: two pre-existing arm64 breakages

Checked because this change touches a public struct in
`headers/os/drivers/bus/PCI.h` that three controllers implement. Both failures
reproduce **at the clean base**, so neither is caused by this work:

* `<pci>x86` — `X86PCIController.cpp: fatal error: ioapic.h: No such file or
  directory`. An x86-only driver that cannot cross-build for arm64. Its *shared*
  sources (`ECAMPCIController.cpp`, `ECAMPCIControllerACPI.cpp`,
  `kernel_interface.cpp`) do compile in that directory's context, so the changes
  here are exercised by it.
* `<pci>designware` — `msi.h: 'MSIInterface::AllocateVectors(uint32, uint32,
  uint32&, uint64&, uint32&)' was hidden [-Werror=overloaded-virtual=]`. The
  second `AllocateVectors` overload — the requester-ID one the GICv3 ITS needs —
  hides the base in designware's MSI class. **That driver has been unbuildable on
  arm64 since that overload was added**, which nothing had noticed because nothing
  builds it. Worth a separate fix; adding `using` declarations or overriding both
  overloads would do it.

### Result: row 4 — done (CONFIRMED)

`ami-0f8d04437d5bdf50d`, built from the merge candidate. Every pre-registered
condition met, and the geometry matched the prediction line for line:

```
PCI: ECAM buses 1-43  at 20000000 belong to another bridge; not mapped here
PCI: ECAM buses 44-56 at 20000000 belong to another bridge; not mapped here
PCI: ECAM at 20000000 (bus 0  base 20000000), segment 0, buses 0-0,   1 decoded,  1 MiB
PCI: of 3 ECAM region(s): 1 mapped here, 2 other bridges' buses, 0 separate windows
PCI: ECAM at 20000000 (bus 1  base 20100000), segment 0, buses 1-43, 67 decoded, 67 MiB
PCI: ECAM at 20000000 (bus 44 base 24400000), segment 0, buses 44-56,19 decoded, 19 MiB
```

**The ENA is at `2:47:00.0`** — domain 2, bus `0x47`, inside bridge 3's window.
It was never a missing device; it was a device nobody looked for, because bridge 3
had mapped bus 0 only.

```
PCI: 2:47:00.0 vendor 1d0f device ec20 class 02.00.00 rev 00
ena: found an ENA device   ->   ena: link is up   ->   ena: attached
```

Device sets are disjoint and each domain enumerates its own buses, recursing
through the bridges it finds:

| domain | buses | devices |
|---|---|---|
| 0 | `00` | 3 — host bridge, 16550, NVMe |
| 1 | `01`, `02`, `03` | 66 — bridges |
| 2 | `44`, `45`, `46`, `47` | 19 — bridges plus **the ENA** |

88 in total, counted `3 -> 69 -> 88` as each domain was added. Exactly one
`disk/nvme/0/raw`. No abort. 64 CPUs, ITS up with 32 redistributors skipped as
cpu-less, root mounted, first-boot processing running.

**Row 2 is ruled out by measurement, which settles the base reading.** Domains 1
and 2 enumerated *different* devices from domain 0, not the same ones, so
`0x20100000` does not alias bus 0 and the flat-aperture-based-at-bus-0 reading is
correct — the interpretation that had flipped twice on this branch and had until
now only an argument behind it. Pre-committing to what a duplicate set would have
meant is what makes this a measurement rather than a rationalisation.

### Guests unchanged

`c7g.large` and `c7g.4xlarge`, byte-identical geometry:

```
PCI: ECAM at 20000000 (bus 0 base 20000000), segment 0, buses 0-ff, 256 decoded, 256 MiB
PCI: 4 device(s)      one disk/nvme/0/raw      ena: attached
```

Single domain at bus `00`, no sibling-bus lines (there is only one region), and no
bare `0 device(s)`. The GIC lines are unchanged too.

## Open

*Re-checked 2026-08-24. Two of the three bullets below are closed; they are kept
with their resolutions because the second one is the whole point of this document.*

* **Metal ENA attach — still not demonstrated end to end, as of 2026-08-24.**
  `ena: found an ENA device` and the driver banner are the last ENA lines before
  the console ring ended. Metal *networking* is still not shown working either way.
  Distinguish this from two things it is not: the 96-vCPU `c8g` guest **does** now
  find its ENA (that is closed, below), and there is a separate, pre-existing
  **intermittent NIC attach** failure — roughly 1 warm reboot in 6 comes up with no
  network — which another pair is investigating and which is not a regression from
  this change.
* ~~Whether the multiple-region case should eventually pick by bus range rather
  than taking the first. Nothing needs it today — every device we care about on
  every class tested is on the chosen region — but a device behind buses 44-56 on
  that guest would be invisible.~~
  **CLOSED — this is exactly what was needed, and it was needed immediately.**
  "Nothing needs it today" was wrong within one class: the 96-vCPU `c8g` has a
  device behind buses `0x44`–`0x56`, it *was* invisible, and that is why the machine
  booted with no network at all. Picking by bus range is what `f5367b3602`
  implements. **The lesson worth keeping: "nothing needs it today" was a statement
  about the classes that had been booted, not about the fleet** — and it read as
  the latter.
* `<pci>designware` is unbuildable on arm64 and has been since the ITS's
  requester-ID `AllocateVectors` overload landed. Its own small branch; the fix
  is a `using` declaration or overriding both overloads. The wider issue it
  exposes is that the arm64 build does not compile every driver, so a shared
  interface change can break an unbuilt one silently.
