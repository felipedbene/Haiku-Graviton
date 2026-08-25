# c7g.metal: the GICv3 panic, and what bare metal presents that a guest does not

Status: **FIXED, merged and hardware-verified as of 2026-08-24.** The GIC brings
up all 64 CPUs on bare metal across three hosts of two different redistributor
shapes, and with the PCI ECAM fix (`metal-pci-segment.md`, `f5367b3602` merged via
`e270548f33`) `c7g.metal` **now boots DeBeOS to userland** — 53 PCI devices, NVMe
root mounted, package daemon running first-boot processing.

**Both blockers in this document are closed.** Neither the GIC nor
`pci_segment != 0` is "the next blocker" any more. §10 below still carries its
original title and is corrected in place; read the banner there, not the heading.

`c7g.metal` was the one Graviton instance class DeBeOS would not boot on. It is
also the class of our own build machine, so the project could not dogfood its
own hardware. This is what was wrong.

## 1. The panic (CONFIRMED, observed)

Booted the canonical AMI `ami-0d61e3910062bb80a` on a fresh `c7g.metal` node
under test (us-west-2a) and read the serial console with
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

Rather than guess, read the same ACPI tables from Linux on the metal build host —
a *different* machine from the node under test above, also a `c7g.metal` — via
SSM, read-only. Parsing
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
* **The spacing is `0x40000`, not `0x20000`.** A GICv4 PE with virtual LPIs owns
  four 64 KB frames (RD, SGI, VLPI, reserved) where a GICv3 PE owns two. Two
  independent confirmations: the `0x40000`-sized GICR ranges in `/proc/iomem`
  are the output of Linux's `GICD_PIDR2.ArchRev` region sizing, and the
  `GICv4 features:` dmesg line below is printed only when
  `GICR_TYPER.VLPIS` is set on **every** redistributor (Linux AND-reduces it).
  The second is the one that matters — see §5.2 for why.
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

**Solved.** The two `c7g.metal` instances really do differ, and the reason is
core harvesting. Boot `metalB` logged this:

```
gicv3: redistributor 13 (region 0) affinity 0xd0000,   processor 13, vlpis 1, last 0
gicv3: redistributor 14 (region 1) affinity 0xf0000,   processor 15, vlpis 1, last 0
gicv3: redistributor 31 (region 1) affinity 0x200000,  processor 32, vlpis 1, last 1
gicv3: redistributor 32 (region 2) affinity 0x1000000, processor 64, vlpis 1, last 0
```

Aff2 = 14 is absent — a fused-off or harvested core — so the 32 live cores of
the low cluster occupy 33 Aff2 slots, 0 through 32. MADT index 31 therefore has
Aff2 = 32, which packs to **`0x200000`**: the exact value the original panic
reported. On a host with no hole (`metalA`) index 32 is the first core of the
*high* cluster and packs to `0x1000000` instead, which is what the build host's
MADT showed.

Same MPIDR layout on every host; different harvest patterns. `_CurrentRedistributor()`
compares packed affinity against `GICR_TYPER[63:32]`, so it is layout-agnostic
and this has no bearing on the fix — but it is why one panic value looked
impossible against another host's tables.

## 5. The fix

Four code commits on `fix/arm64-metal-gicv3`. Both `kernel_arm64` and
`haiku_loader.efi` compile clean.

> **Corrected 2026-08-24.** This line used to end "**not yet hardware-verified**",
> which contradicted this document's own status line and §8 "Hardware results
> (CONFIRMED, observed)" below it. It **is** hardware-verified: 64 of 64 CPUs on
> multiple metal hosts of differing redistributor shapes. The "not yet" was true
> for about as long as it took to boot the image, and then sat here.

1. **`arm64: find every GIC redistributor, not just the first contiguous run`**
   * `intc_info` carries a list of redistributor *regions* (up to
     `INTC_MAX_GICR_REGIONS`, 16) instead of one range.
   * The loader collects every GICC's GICR base, sorts them, and coalesces them
     into contiguous runs. The spacing is taken from the addresses themselves
     (a neighbour exactly one v3 or v4 stride away continues the run) rather
     than from any claim about how big a redistributor is.
   * The kernel maps each region separately.
   * All three redistributor walks — `_PrefaultRedistributors()`,
     `_CurrentRedistributor()`, and the ITS's `_InitLpis()` — iterate regions,
     treating `GICR_TYPER.Last` as the end of a region rather than the end of
     the walk.
   * Generous `dprintf`: distributor geometry and chosen stride, every region,
     and every redistributor's affinity, processor number, VLPIS and Last bits,
     plus found-vs-CPU-count. One boot on this hardware costs a bake, so it
     should yield many facts.

2. **`arm64: step the redistributor walk by what each redistributor reports`**
   The first version of this work derived one global stride from
   `GICD_PIDR2.ArchRev`, citing Linux. That citation was wrong, and the claim
   was load-bearing, so it is recorded here rather than quietly amended.

   Linux uses `ArchRev` in `gic_acpi_parse_madt_gicc()` to choose how many bytes
   to `ioremap` for a region — and it never *walks* those regions, marking them
   `single_redist` and stopping after the first redistributor. Where it does
   walk, `gic_iterate_rdists()` advances by asking the frame it is standing on:

   ```c
   ptr += SZ_64K * 2;                  /* Skip RD_base + SGI_base */
   if (typer & GICR_TYPER_VLPIS)
           ptr += SZ_64K * 2;          /* Skip VLPI_base + reserved page */
   ```

   That is also what the architecture guarantees. The extra pair of frames
   belongs to redistributors that implement virtual LPIs, reported *per
   redistributor* by `GICR_TYPER.VLPIS`, and that field is RES0 where virtual
   LPIs are unsupported. A GICv4 implementation whose redistributors lack VLPIs
   is legal, and a global 0x40000 stride would then step through 0x20000 frames:
   every other redistributor read, the rest silently missed, no panic. Worse
   than the bug in §4, and correctly identified as such before any of it
   shipped.

   All three walks now step by `gicr_frame_stride(typer)`. `ArchRev` survives
   only where Linux uses it — sizing a window when firmware gives neither a
   length nor a region list — and the member is named `fGicrFallbackStride` to
   keep that scope visible. The ITS no longer takes a stride at all.

   `GICR_TYPER.VLPIS` on this hardware is confirmed from an artifact, not
   inferred: Linux computes `has_vlpis &= !!(typer & GICR_TYPER_VLPIS)` across
   every redistributor and prints `GICv4 features:` only if that survives. The
   build host's dmesg has the line, so VLPIS is set on all 64. KVM sets
   `GICR_TYPER.PLPIS` and not VLPIS, so the guest gets 0x20000 — identical to
   the pre-existing behaviour. **Both derivations agree on both platforms under
   test; only the counterexample separates them**, which is exactly why this
   needed fixing before a boot could be taken as evidence either way.

3. **`arm64: enable group 1 on a distributor with two security states`**
   (Conclusion CONFIRMED by measurement, original reasoning DISPROVEN — see §8.
   Note that it writes `GICD_CTLR`,
   a distributor-wide control register, on **every** arm64 platform, not just
   metal; the added bit is reserved rather than meaningful where `DS=1`, which
   is the case on every GIC we have booted so far, but this is not a
   metal-only change and the virtualised regression test covers it.)
   `GICD_CTLR_ENABLE_G1NS` was
   `1u << 1`, which is the enable bit only when `GICD_CTLR.DS` is set. Linux
   reports `DS=0` on this hardware, and in the non-secure view of a
   two-security-state distributor with affinity routing on, non-secure group 1
   is enabled by **bit 0**, bit 1 being reserved. With only bit 1 written, no
   SPI would ever be delivered — a machine that finishes interrupt setup and
   then goes quiet. `DS` is not readable outside the secure view, so both bits
   are written, as Linux does. This is the next thing in the way after the
   redistributor fix, not the current blocker. `GICD_CTLR` is now logged on
   readback so the next boot says which bits stuck.

4. **`arm64/efi: don't spin forever when the MADT lists more CPUs than we
   support`** — see §7.

### What the coalescing does with awkward firmware

> **Do not carry this coalescing pattern to PCI ECAM.** Coalescing is right *here*
> because these pieces describe **one** redistributor array that firmware split up.
> An MCFG's several allocations can instead be **one per root bridge** — genuinely
> separate bridges owning disjoint bus ranges — and coalescing those into one span
> and giving it to every bridge makes each bridge enumerate every device, which
> published one NVMe controller three times on the 96-vCPU `c8g`. There the correct
> operation is per-bridge *selection*, not union. See the boxed rule in
> `metal-pci-segment.md` §"The fix, and the reading it depends on". Same-shaped
> firmware description, opposite correct response; the deciding question is whether
> the pieces describe one object or several.

Asked of it explicitly, because it infers spacing from addresses:

* **MADT entries out of address order** — handled. The bases are insertion
  sorted before coalescing; ACPI does not require address order and Graviton's
  happen to be ordered, so this is not exercised by the machines we have.
* **A single CPU in a region** — the run ends immediately, no spacing can be
  inferred, and the region is sized `GICR_STRIDE_V3`: exactly the `RD_base` and
  `SGI_base` frames the kernel reads. The walk's guard is
  `frame + GICR_STRIDE_V3 <= end`, so it runs once and stops. Nothing is
  assumed about frames we never touch. This is the case Linux's ACPI path
  always takes, one region per CPU.
* **Gaps from absent or unusable CPUs** — a delta that is neither one stride nor
  the other ends the run, so a gap splits the list into two regions and both are
  walked. The same mechanism that handles Graviton's 16 GiB gap handles a small
  one. Note we currently record a base only for a GICC we could register, so a
  CPU past `SMP_MAX_CPUS` leaves a gap by construction — which is correct: we
  neither start that CPU nor need its redistributor.
* **An irregular layout** (mixed 0x20000 and 0x40000 spacing in one run) — the
  run is broken where the delta changes, producing more regions than strictly
  necessary but never a wrong one. Per-frame `VLPIS` stepping then walks each
  correctly. Degrades into more regions, not into a wrong address.
* **More than `INTC_MAX_GICR_REGIONS` (16) regions** — reported by the loader,
  and the CPUs behind the surplus will fail to find a redistributor. That is a
  loud panic in `_CurrentRedistributor()`, not silence. A machine that needs
  more than 16 disjoint runs would need the ceiling raised; 64 single-CPU
  regions is the pathological case and would hit it.
* **Bases the kernel cannot map** — `_MapRedistributors()` panics naming the
  region and address rather than continuing with a partial list.

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

## 8. Hardware results (CONFIRMED, observed)

Booted `ami-01a60ed26f29bb722` (branch head `d76a97940e`) on five instances.

### Gating: virtualised Graviton — PASS, no regression

```
c7g.large    gicv3: gicd 0x10000000 (size 0x10000), arch rev 3, typer 0x7a3003, stride 0x20000
             gicv3: redistributor region 0: 0x10200000 size 0xfdf0000
             gicv3: 2 redistributor(s) across 1 region(s) for 2 cpu(s)
c7g.4xlarge  16 redistributor(s) across 1 region(s) for 16 cpu(s)
```

Both take the `regs2` single-region fallback with firmware's own size, report
`arch rev 3` and stride `0x20000` — identical to the constant it replaced — and
boot through ENA link-up, MSI-X, first I/O interrupt and first-boot package
processing. The guest MADT is byte-identical across `c7g.large`, `t4g.small` and
`c8g.large` (one GICR subtable, every `GICC.gicr_address` zero), so the
coalescing path is unreachable on the guest fleet by firmware rather than by
luck.

### Metal: 64 of 64, on two hosts of different shapes

Two hosts were booted, deliberately, because they differ: host A came up as two
regions of 32, host B as three of 14 + 18 + 32. Both reach `64
redistributor(s)`, `found 64 logical cpus` and `lpis enabled on 64
redistributor(s)`. See the cluster-shape finding above for the full fleet
picture.

### Finding: `GICR_TYPER.Last` is not a reliable region terminator on Graviton3

Named separately because it is exactly what a future reader would lean on, and
because the original code used it as one of its two bounds.

| host | redistributors setting `Last` |
|---|---|
| A (2 regions, 32+32) | **none at all** |
| B (3 regions, 14+18+32) | **one**, mid-list, at the end of the low cluster (index 31, Aff2 = 32) |

The architecture has `Last` mark the final redistributor of a contiguous range,
but this firmware does not supply it consistently: one host omits it entirely,
the other sets it where a *cluster* ends rather than where the address range
does. So it cannot bound a walk. The region's size and the count derived from it
do the real work now, and `Last` is only an early exit — which is safe, since
exiting early on a spurious `Last` can only end a region that the count would
have ended anyway.

### Finding: nothing about cluster shape can be assumed

Region sizes observed across seven distinct `c7g.metal` hosts, all running
identical `GRVTN003` firmware:

```
12, 13, 14, 18, 20, 32, 33
```

as 2-region layouts (32+32) and 3-region layouts (14+18+32, 12+20+32,
33+13+18). Cores are fused off or harvested per host, and a hole in the Aff2
sequence splits what would otherwise be one run. There is no fixed cluster size,
no fixed region count, and no fixed number of regions per socket half. Any code
that special-cases "two runs of 32" is wrong on the majority of hosts. This list
is the evidence for coalescing by address delta rather than by any assumed
geometry — and it is also why one metal boot cannot validate the coalescing, so
two hosts of different shapes were booted.

### Theories killed by measurement

* **`GITS_TYPER.PTA` might be 1 on real hardware.** Dead. Metal reports
  `typer 0xbf700022f33` → **PTA = 0**, ITT entry size 4, 16 EventID bits, 18
  DeviceID bits. Collections name a processor number on metal exactly as on the
  guest.
* **`GICD_CTLR` bit 0 is needed when `DS=0`** — the premise of the group-1
  commit. Dead. We write `0x13`; metal reads back **`0x12`** (`ARE_NS` +
  `EnableGrp1`, bit 0 clear, `DS` clear) and guests read back **`0x52`**
  (`DS` + `ARE_NS` + `EnableGrp1`, bit 0 clear). Bit 0 is *write-ignored on both
  AWS platforms* — KVM's `vgic_mmio_write_v3_misc()` does not model Group 0 at
  all, and on metal non-secure software cannot set it. The commit's conclusion
  (harmless, keep it, Linux does) is right and now measured; its reasoning about
  which bit does the work was wrong. It does **not** enable Group 0 on the
  flagship.
* **`GICR_TYPER.VLPIS` distinguishes the stride empirically.** No: `vlpis 1` on
  every metal redistributor and `vlpis 0` on every guest one, so ArchRev and
  VLPIS agree on both platforms and cannot be told apart here. The revert to
  ArchRev rests on Arm IHI 0069G 12.10 alone, which is the right basis.

### Verification round on ami-0200e8f97df35c16c (branch head 08485ea811)

Four instances, one image. Gating first.

| class | result |
|---|---|
| `c7g.large` | **PASS** — `2 redistributor(s) across 1 region(s) for 2 cpu(s); 0 report vlpis, 1 report last`; `lpis enabled on 2, 0 skipped`; `ecam region: addr 20000000, segment: 0`; four PCI devices; `ena: attached` |
| `c7g.4xlarge` | **PASS** — `16 redistributor(s) … for 16 cpu(s)`; `lpis enabled on 16, 0 skipped, 1024 KiB`; four PCI devices; `ena: attached` |
| `c7g.metal` | **BOOTS TO USERLAND** — 53 PCI devices across buses 0–4, `bfs: mounted "Haiku"`, `ena: found an ENA device`, first-boot processing through package 15 |
| `c8g.24xlarge` | GIC **PASS**; boot stalls later, identically on both images — see below |

Note `1 report last` on the guests: KVM *does* set `GICR_TYPER.Last`, unlike metal
host A which sets it nowhere. The two platforms differ on exactly the bit that
cannot be relied upon.

The truncation fix is hardware-verified, and only this class exercises it:

```
c8g.24xlarge: gicv3: 96 redistributor(s) across 1 region(s) for 64 cpu(s)
              gicv3-its: lpis enabled on 64 redistributor(s), 32 skipped as
                         cpu-less, 4096 KiB of pending tables
```

64 enabled, 32 skipped, 4 MiB instead of 6 MiB, and no `GICR_CTLR.EnableLPIs`
latched on a redistributor belonging to a CPU that will never start.

Two caveats recorded rather than glossed:

* **Metal's ENA attach outcome is unknown.** `ena: found an ENA device` and the
  driver's build banner are the last ENA lines in the window; the console ring
  ended before an attach or a failure. Metal networking is **not** verified.
* **The 64 KiB console ring is now the binding constraint on metal.** 53 PCI
  devices at ~15 dumped lines each is around 800 lines, which evicted the GIC
  summary from this boot's window entirely — the same O(n) eviction that the
  per-redistributor `dprintf` was moved behind `TRACE_GICV3` to avoid, this time
  from `pci_print_info()`. Future metal verification should sample the console
  early, or that dump needs gating too.

### `c8g.24xlarge` stalls after NVMe, on both images

Reaches `publish device: … disk/nvme/0/raw`, `1/raw`, `2/raw` and stops, with the
capture unchanged across repeated polls minutes apart. **Identical on the
pre-PCI-fix image and this one**, so it is not a regression from either branch —
it is a separate, previously unverified instance class (Graviton4). Its GIC and
ITS come up correctly, which is what this round needed from it.

### 96 vCPU: the loader hang is fixed

`c8g.24xlarge` now gets past the MADT walk and reports `96 redistributor(s)
across 1 region(s) for 64 cpu(s)`, `found 64 logical cpus`, ITS ready, NVMe
disks published. It boots on 64 of its 96 CPUs instead of wedging.

That 96-vs-64 line exposed two further problems, both now fixed:

* The walk is bounded by region size and `Last`, not by the CPU count, so
  `_InitLpis` was allocating a 128 KiB contiguous pending table per
  *redistributor* — 12 MiB here, a third of it for CPUs that will never exist,
  24 MiB at 192 vCPUs — and latching `GICR_CTLR.EnableLPIs`, which is one-way,
  on redistributors belonging to parked CPUs. It now skips redistributors whose
  affinity matches no CPU, and reports the skipped count.
* The one-line-per-redistributor `dprintf` is O(PEs): 96 lines is ~9 KB, which
  wrapped the firmware console ring and evicted the loader's own discovery
  lines — destroying the log it was added to serve. It is now behind
  `TRACE_GICV3`, with its aggregate signal (how many report VLPIS, how many
  report Last) folded into the O(1) summary. The O(1) instruments stay: they are
  what made this round decidable without `/dev/mem`.

## 9. Note on the DeviceID cap

`_InitTables()` caps the ITS device table at `min(fDeviceIDBits, 16)` = 65536
entries. Metal reports **18** DeviceID bits where the guest reports 16, so this
cap is now load-bearing on metal where it was slack on the guest. ENA on metal is
bus `0x24`, requester ID `0x2400`, comfortably inside 65536 — but a device above
RID 65535 would silently have no translation.

## 10. ~~What remains: metal does not boot, and it is not the GIC~~ — RESOLVED 2026-08-24

> **This section's heading is stale and is kept only so the diagnosis below stays
> findable. `c7g.metal` boots to userland.** The PCI ECAM blocker described here was
> fixed by `f5367b3602` ("pci/ecam: give each root bridge the ECAM region for its own
> buses"), merged via `e270548f33` — see `metal-pci-segment.md`. The half of this
> section that was true and remains true is its first claim: **the interrupt
> controller was done at this point, and none of what follows was the GIC.**

At the time of writing, all metal hosts ended at:

```
PCI: mechanism addr: e010000000, seg: 1, start: 0, end: ff
PCI: multiple segments not supported!driver busses/pci/ecam/driver_v1 init failed: General system error
...
PANIC: did not find any boot partitions!
```

The GIC brings up all 64 CPUs and the ITS is ready well before this point, so the
interrupt controller is done. ~~The next blocker is in the PCI ECAM controller~~ —
**that blocker is FIXED**, `f5367b3602` via `e270548f33`; `metal-pci-segment.md`
carries the analysis and the hardware result.

Still open beyond that, re-checked 2026-08-24:

* The ITS surface at scale — `GITS_BASER.Indirect` (Linux picks an indirect
  device table on this hardware; we request flat and do not check the readback)
  and the 256-vector / 32-device ceilings. ~~Untested because no MSI device
  attaches on metal yet.~~ **The stated reason no longer holds:** metal now
  enumerates 53 PCI devices and mounts an NVMe root, so devices do attach. Whether
  the ITS *at scale* has been exercised is **UNVERIFIED as of 2026-08-24** — it has
  not been re-measured since the ECAM fix, and this bullet should be re-run rather
  than either ticked off or left implying metal has no PCI.
* `INTC_MAX_GICR_REGIONS` is 16. The worst case is one region per CPU, which
  64 single-redistributor regions would exceed. Not observed; smallest region
  seen is 12.
