# c7g.metal: no PCI, because its one ECAM region is numbered 1

Status: **fixed and hardware-verified.** With this change `c7g.metal` enumerates
53 PCI devices across buses 0–4, publishes NVMe, mounts its root volume and
reaches userland.

This is the blocker that follows the GICv3 fix (`metal-gicv3-panic.md`). With the
interrupt controller working, all 64 CPUs up and the ITS ready, `c7g.metal` still
does not reach userland.

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
`PCI_MMCFG_BUS_OFFSET()`. This is the one thing here settled by argument rather
than by measurement, and the next boot is expected to settle it: if bridge 2's
devices appear at buses `1-0x43`, the reading is right.

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

### Pass condition for the next boot

One domain per bridge with **disjoint** device sets; exactly **one** NVMe disk;
an ENA present; `"multiple host bridges not supported!"` firing **zero** times;
and no abort while probing. If the stall clears too, that is a bonus and will
only be claimed if the boot goes past where it stalled.

## Open

* **Metal ENA attach is unverified.** `ena: found an ENA device` and the driver
  banner are the last ENA lines before the console ring ended. Networking on
  metal is not yet demonstrated either way.
* Whether the multiple-region case should eventually pick by bus range rather
  than taking the first. Nothing needs it today — every device we care about on
  every class tested is on the chosen region — but a device behind buses 44-56 on
  that guest would be invisible.
* Whether the ITS then hands out MSIs correctly on metal — untested, because no
  MSI-capable device has attached there yet. `GITS_TYPER.PTA` is 0 and the ITS
  reports 18 DeviceID bits against our `min(fDeviceIDBits, 16)` device-table cap,
  which is slack for ENA at requester ID `0x2400` but is now load-bearing where
  it was not on the guest.
