# c7g.metal: the GICv3 panic, and what bare metal presents that a guest does not

Status: **panic reproduced and diagnosed; fix written and compiling; awaiting a
bake for hardware verification.**

`c7g.metal` was the one Graviton instance class DeBeOS would not boot on. It is
also the class of our own build machine, so the project could not dogfood its
own hardware. This is what was wrong.

## 1. The panic (CONFIRMED, observed)

Booted the canonical AMI `ami-0d61e3910062bb80a` on a fresh `c7g.metal`
(`i-0ac875282beb9fd6a`, us-west-2a) and read the serial console with
`aws ec2 get-console-output --latest`. The console works fine on metal; output
became available a few minutes after launch.

```
reserve_io_interrupt_vectors: reserved 1020 vectors starting from 0
PANIC: gicv3: no redistributor for affinity 0x200000

Welcome to Kernel Debugging Land...
revision: hrev59996
Thread 0 "" running on CPU 12
 3 ffff0000024f1e30 (+ 176) ffff0000000dbbcc   <kernel_arm64> panic() + 0x8c
 4 ffff0000024f1f50 (+ 288) ffff000000182cec   <kernel_arm64> GICv3InterruptController::_CurrentRedistributor() + 0x8c
 5 ffff0000024f1f60 (+  16) ffff000000182d90   <kernel_arm64> GICv3InterruptController::_PerCpuInit() + 0x10
 6 ffff0000024f1f80 (+  32) ffff0000000a2548   <kernel_arm64> smp_trap_non_boot_cpus() + 0xc4
 7 ffff0000024f1fd0 (+  80) ffff000000090244   <kernel_arm64> _start() + 0xa4
```

The exact failing check is the fall-through at the end of
`GICv3InterruptController::_CurrentRedistributor()`
(`src/system/kernel/arch/arm64/arch_int_gicv3.cpp`): a secondary CPU walked the
mapped redistributor window, found no `GICR_TYPER` whose affinity matched its
own `MPIDR_EL1`, and panicked. Boot never reaches `scheduler_init`, so the
metal console never prints a CPU count.

## 2. What the loader itself already said (CONFIRMED, observed)

Two lines further up the same console:

```
discovered gic from acpi: version=4, gicd=b800000000, gicr=b800140000 (size 0), its=bc00100000
discovered psci from acpi: conduit=smc
...
Chosen interrupt controller:
  kind: gicv3
  regs: 0xb800000000, 0x10000
        0xb800140000, 0x0
```

Compare a working virtualised boot (`c7g.4xlarge`, and `c7g.16xlarge` verified
again in this investigation):

```
discovered gic from acpi: version=3, gicd=10000000, gicr=10200000 (size fdf0000), its=10080000
discovered psci from acpi: conduit=hvc
```

Three differences, and the first two are the bug:

* **`size 0`** — the redistributor range length is unknown.
* **`version=4`** — this is a GICv4 implementation, not GICv3.
* `smc` rather than `hvc` for PSCI, and 40-bit register addresses. Both already
  handled; neither is implicated.

## 3. Ground truth from the hardware (CONFIRMED, observed)

Rather than guess, read the same ACPI tables from Linux on the metal build host
`i-0f7f6f3e8922acffd` (also `c7g.metal`), via SSM, read-only. Parsing
`/sys/firmware/acpi/tables/APIC` directly:

```
GICC entries: 64   GICR structures: 0   ITS structures: 1
GICR structures: []
ITS structures: [(1, '0xbc00100000')]
   cpu 0  gicr=0xb800140000 mpidr=0x0
   cpu 1  gicr=0xb800180000 mpidr=0x10000
   cpu 31 gicr=0xb800900000 mpidr=0x1f0000
   cpu 32 gicr=0xbc00140000 mpidr=0x100000000
   cpu 63 gicr=0xbc00900000 mpidr=0x1001f0000
distinct deltas between sorted gicr_bases: [262144, 17171742720]
```

and from `/proc/iomem`:

```
total GICR regions: 64
block 0xb800140000 .. 0xb800900000  (32 frames of 0x40000)   next gap 0x3ff840000
block 0xbc00140000 .. 0xbc00900000  (32 frames of 0x40000)
```

So, on Graviton3 bare metal:

* **There is no MADT GICR structure at all.** The redistributors are described
  per-CPU, by each GICC entry's GICR Base Address field. ACPI defines that form
  for precisely one situation: when the redistributors are *not* one contiguous
  range.
* **They are not contiguous.** 64 redistributors in two runs of 32, at
  `0xb800140000` and `0xbc00140000` — a gap of `0x3ff840000`, about 16 GiB.
* **The stride is `0x40000`, not `0x20000`.** A GICv4 PE owns four 64 KB frames
  (RD, SGI, VLPI, reserved) where a GICv3 PE owns two. Linux sizes each region
  from `GICD_PIDR2.ArchRev`, and the `0x40000`-sized GICR ranges in
  `/proc/iomem` are that calculation's output — independent confirmation of the
  MADT's `version=4`.
* Linux on the same machine also reports `GICv3: GICD_CTLR.DS=0, SCR_EL3.FIQ=1`,
  `960 SPIs implemented`, `48 PPIs, DirectLPI`, `GICv4 features: DirectLPI
  RVPEID Valid+Dirty`, and the ITS in `GICv4.1 mode`.

Everything above is invisible under virtualisation: KVM's emulated distributor
reports GICv3, presents a single contiguous redistributor range with a real
length in a GICR structure, and has one security state.

## 4. Why the code could not handle it (CONFIRMED, arithmetic matches)

`arch_int_gicv3.cpp` held two assumptions:

```c
fGicrStride(GICR_STRIDE_V3),          // 0x20000, hardcoded; GICR_STRIDE_V4 defined and never used
...
if (fGicrSize == 0)
    fGicrSize = fGicrStride * smp_get_num_cpus();
```

and the loader kept only the lowest per-CPU base ("The frames are contiguous,
so the lowest base wins").

With 64 CPUs that fallback is `0x20000 * 64` = **8 MB**, so the mapped window
was `0xb800140000 .. 0xb800940000`. The real spacing is `0x40000`, so 8 MB is
exactly 32 redistributors — the whole first block and not one byte more. The
window ends at `0xb800940000`; the second block starts 16 GiB away. Every one
of CPUs 32–63 was therefore unfindable, and the first of them to run
`_PerCpuInit()` panicked.

The panic value corroborates this precisely. `0x200000` is a packed affinity
with Aff2 = 32 — core 32, i.e. the first CPU of the second block, the first one
past the end of the window.

`GICR_TYPER.Last` made it worse rather than better: it marks the last
redistributor of *the region it appears in*, so even a window large enough to
span both blocks would have stopped the walk at core 31.

Note one honest discrepancy. The test instance's panic reports affinity
`0x200000` (Aff2 = 32, Aff3 = 0), whereas the build host's MADT gives core 32
`mpidr=0x100000000` (Aff3 = 1, Aff2 = 0), which packs to `0x1000000`. Both are
"core 32, first CPU of the second redistributor block", so the diagnosis does
not depend on which encoding a given machine uses — but the two `c7g.metal`
instances do appear to differ in MPIDR layout, and that is unexplained. The fix
is affinity-encoding agnostic: it matches `GICR_TYPER[63:32]` against whatever
this PE's `MPIDR_EL1` actually packs to, and searches every region.

## 5. The fix

Three commits on `fix/arm64-metal-gicv3`. Both `kernel_arm64` and
`haiku_loader.efi` compile clean; **not yet hardware-verified.**

1. **`arm64: find every GIC redistributor, not just the first contiguous run`**
   * `intc_info` carries a list of redistributor *regions* (up to
     `INTC_MAX_GICR_REGIONS`, 16) instead of one range.
   * The loader collects every GICC's GICR base, sorts them, and coalesces them
     into contiguous runs. The spacing is taken from the addresses themselves
     (a neighbour exactly one v3 or v4 stride away continues the run), falling
     back on the MADT's GIC version only for a lone redistributor — the
     firmware's own numbers rather than an assumption.
   * The kernel maps each region separately and derives the stride from
     `GICD_PIDR2.ArchRev`, the same source Linux uses.
   * All three redistributor walks — `_PrefaultRedistributors()`,
     `_CurrentRedistributor()`, and the ITS's `_InitLpis()` — iterate regions,
     treating `GICR_TYPER.Last` as the end of a region rather than the end of
     the walk.
   * Generous `dprintf`: distributor geometry and chosen stride, every region,
     and every redistributor's affinity, processor number, VLPIS and Last bits,
     plus found-vs-CPU-count. One boot on this hardware costs a bake, so it
     should yield many facts.

2. **`arm64: enable group 1 on a distributor with two security states`**
   (PLAUSIBLE — spec-derived, *not* observed.) `GICD_CTLR_ENABLE_G1NS` was
   `1u << 1`, which is the enable bit only when `GICD_CTLR.DS` is set. Linux
   reports `DS=0` on this hardware, and in the non-secure view of a
   two-security-state distributor with affinity routing on, non-secure group 1
   is enabled by **bit 0**, bit 1 being reserved. With only bit 1 written, no
   SPI would ever be delivered — a machine that finishes interrupt setup and
   then goes quiet. `DS` is not readable outside the secure view, so both bits
   are written, as Linux does. This is the next thing in the way after the
   redistributor fix, not the current blocker. `GICD_CTLR` is now logged on
   readback so the next boot says which bits stuck.

3. **`arm64/efi: don't spin forever when the MADT lists more CPUs than we
   support`** — see §7.

## 6. Hypotheses considered and disproven

Kept here deliberately.

* **"The ITS sizing ceilings are too small for metal."** `GIC_ITS_MAX_VECTORS`
  256, 32 devices, 32 events each, and the contiguous-LPI-run requirement were
  all suspects. Not reached: the panic is in the redistributor walk, long
  before the ITS is initialised. Untested on metal, still plausible *later*.
* **`GITS_TYPER.PTA` being 1 on real hardware.** A good hypothesis — a
  hypervisor hides it, and `_MapCollection` would have to write a redistributor
  physical address rather than a processor number. But the existing code already
  handles both cases, and the ITS is never reached. Unresolved, not a bug so
  far.
* **`GITS_CMD_MOVI`/`MOVALL` being unimplemented, and the single hardcoded
  collection 0.** Real limitations (every LPI lands on the boot CPU) but not
  this failure; nothing here is reached.
* **`arch_int_assign_to_cpu()` being a stub returning 0.** Unrelated to boot.
* **`SMP_MAX_CPUS` = 64 being the metal problem.** Disproven directly:
  `c7g.16xlarge` also has 64 vCPUs and boots fine (§7). The failure is
  bare-metal topology, not CPU count. This is why the 64-vCPU virtualised
  instance was launched alongside the metal one — it isolates the two variables.
* **The redistributor `LAST`/stride handling being merely *inefficient*.** It is
  not; the wrong stride is load-bearing, because it is what makes the fallback
  size land exactly on the block boundary.

## 7. Secondary: instance classes above 16 vCPU (CONFIRMED, observed)

* **`c7g.16xlarge`, 64 vCPU — boots and works.** `scheduler_init: found 64
  logical cpus`, ITS ready with 256 vectors, ENA up with MSI-X at MTU 9001,
  first-boot package processing and the desktop starting. 64 is exactly
  `SMP_MAX_CPUS`, and sitting on the ceiling is fine.

* **`c8g.24xlarge`, 96 vCPU — hangs in the boot loader, silently.** Console
  stops at

  ```
  acpi: Found 'APIC' @ 0x00000000786d0114
  ```

  and never reaches `discovered gic from acpi`. Polled twice; the second sample
  showed the entire firmware banner and ACPI sequence again, i.e. the watchdog
  reset the machine into the same hang.

  Cause, found by reading `arch_acpi.cpp`: `arch_smp_register_cpu()` returns
  NULL past `SMP_MAX_CPUS`, and the MADT walk responded with `continue` — but
  `desc` is advanced at the *bottom* of that loop, so `continue` re-reads the
  same GICC entry for ever. The 65th CPU entry wedges the loader. Fixed by
  stepping over the entry and reporting once; a machine larger than we support
  should run on the CPUs we can use, not wedge before printing anything.

  This is why the ceiling looked untested: every class at or below 64 vCPU is
  unaffected.

## 8. What remains

* **A bake and a metal boot.** The code compiles but nothing here is
  hardware-verified. One boot of `c7g.metal` will say whether the redistributor
  walk now finds 64 of 64, whether `GICD_CTLR` took both group-1 bits, what
  `GITS_TYPER.PTA` really is, and what the MPIDR layout on that machine is.
* If the group-1 fix is right and the boot continues, the ITS is the next
  unexplored surface: PTA, `GITS_BASER.Indirect` (Linux chooses an indirect
  device table here, we request a flat one and do not check the readback), and
  the 256-vector / 32-device ceilings against what metal presents.
* `c8g.24xlarge` should be re-tested after the bake to confirm it boots on 64 of
  its 96 CPUs rather than hanging.
* QEMU was deliberately not used. It can model GICv3 with configurable
  redistributor counts, but it cannot easily reproduce the thing that actually
  broke — two redistributor blocks 16 GiB apart described per-GICC with no GICR
  structure — and this project has already been burned by treating QEMU
  agreement as evidence about EC2.
