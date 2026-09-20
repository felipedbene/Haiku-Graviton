# Platform portability audit: what a second platform would break on

Issue #94 asks for a bring-up plan for Raspberry Pi 5, Raspberry Pi 3 and RISC-V.

This document is deliberately **not** a schedule of hardware milestones. We have no Pi
and no RISC-V board, so any date attached to "Pi 5 boots" would be invented. What can be
established today, without that hardware, is the **audit**: every place this tree has
baked a Graviton/EC2 fact into a path that claims to be generic. That list is checkable,
it does not expire, and it is the actual content of a bring-up plan — the stages below
are named by the defects they clear, not by the hardware they need.

Every item carries `file:line` and one of three classifications:

| | meaning | owed |
|---|---|---|
| **(P)** | already generic | nothing |
| **(G)** | correctly platform-specific, properly gated (register readback, firmware table presence, device match) | nothing |
| **(X)** | a Graviton fact hard-coded on a path that claims to be generic | **this is the bring-up work** |

Counts: **42 (X)**, **28 (G)**, **14 (P)**. The (X) list is the answer to the issue. (G)
and (P) are reported because "we checked and it is fine" is a result: they are the parts
of a second bring-up that cost nothing, and several of them (the `CurrentEL` gates, the
`ID_AA64*` feature gates, the ITS-optional path) are the pattern the (X) items should be
fixed *into*.

---

## 1. The measured datum: does this tree boot on something that is not EC2?

Yes — on three of the four configurations tried, and the fourth failure is precise and
root-caused.

**Artifact under test.** `haiku-mmc.image`, a 19 GiB arm64 MMC image built from tree
`efcec66fe2` (a commit near the current branch tip, not identical to it). Its own banner:
`kernel build roottable-guard-1, compiled Sep 14 2026 23:39:26`, `Haiku revision
hrev60009+580`. Host: x86-64 Linux, QEMU 11.0.0, **TCG only — there is no `/dev/kvm` on
this host**, so every run below is pure emulation. Firmware: the distribution's
`edk2`/ArmVirtQemu build. No EC2 and no hypervisor of ours.

Common tail for all runs (`snapshot=on` so the image is never written):

```
-drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw \
-drive if=pflash,format=raw,unit=1,file=VARS.fd \
-drive id=hd0,if=none,format=raw,snapshot=on,file=haiku-mmc.image \
-device virtio-blk-pci,drive=hd0 -nographic -serial mon:stdio -no-reboot
```

### Run 1 — baseline `virt`, ACPI present

```
qemu-system-aarch64 -M virt -cpu max -smp 2 -m 4096 <common tail>
```

**Booted to `app_server`.** Verbatim highlights:

```
discovered gic from acpi: version=2, gicd=8000000, gicc=8010000
kind: pl011
kind: gicv2
PSCI conduit: hvc
MMU Enabled, Granularity 4KB, bits 52
initialize PCI controller from ACPI
bfs: mounted "Haiku"
```

19 first-boot package activations, then `app_server`. The only failure was
`app_server: Failed to initialize virtual screen configuration` — expected, no display
device was supplied. Also logged, and both correct:
`arm64 CPU: MIDR_EL1 0x000f0510 -- unknown implementer unknown part r0p0` (QEMU's
synthetic `max` CPU is not in the part table, and the code says so rather than printing
nothing), and `arm64_pmu: no overflow interrupt gsiv in madt; pmu sampling will stay off`.

### Run 2 — `acpi=off`: the FDT-only path, end to end

```
qemu-system-aarch64 -M virt,acpi=off -cpu max -smp 2 -m 4096 <common tail>
```

**Booted identically.** Verbatim:

```
efi/fdt: Valid FDT from UEFI table 6, size: 1052672
kind: pl011
kind: gicv2
acpi: AcpiInitializeTables failed AE_NOT_FOUND
publish device: ... bus/fdt/blob, module bus_managers/fdt/device/v1
finalize PCI controller from FDT
scheduler_init: found 2 logical cpus
bfs: mounted "Haiku"
```

19 packages, `app_server`. **This refutes the leading hypothesis in the issue.**
"A Pi is FDT-only and has no ACPI at all, which is probably the single largest structural
gap" is *wrong for this tree*: console, interrupt controller, SMP, PCI ECAM and the root
filesystem all come up from a device tree with no ACPI tables in the machine at all. The
discovery architecture is genuinely dual-firmware (see G14/G15). The FDT branch is
*thinner*, not absent, and the gaps in it are enumerated as X14-X17 below.

### Run 3 — Pi 5's core

```
qemu-system-aarch64 -M virt -cpu cortex-a76 -smp 4 -m 4096 <common tail>
```

**Booted identically** — 19 packages, `bfs: mounted "Haiku"`, `app_server`.

### Run 4 — Pi 3 shaped: Cortex-A53, GICv2, no MSI

```
qemu-system-aarch64 -M virt,gic-version=2,msi=off -cpu cortex-a53 -smp 4 -m 4096 <common tail>
```

**FAULTED, in our own EFI loader, before the kernel was ever loaded.** Verbatim, and this
is the first divergence:

```
0x000000013e8f6590 Partition::Partition
Synchronous Exception at 0x000000013EA8B77C
ESR 0x02000000
FAR 0x0000000000000000
```

Zero package activations — it never reached the kernel. `ESR 0x02000000` decodes to
`EC 0x00` ("unknown reason") with `IL 0x1`: the signature of an **undefined 32-bit
instruction**. The PC is inside `add_partitions_for`, during partition enumeration.

**Root cause, established two independent ways rather than assumed.**

The host has no aarch64-capable `objdump` and this tree's cross-tools are not present as
binaries, so the loader was disassembled with a purpose-written Capstone-based PE/COFF
section reader. `haiku_loader.efi` `.text` is at RVA `0x1000`, size `0x76fe8`, 48238
instructions, and contains **exactly 5 FEAT_LSE atomics, all `ldaddal`**, at RVAs
`0x14a38`, `0x14a70`, `0x1577c`, `0x19780`, `0x19988`.

Of the five candidate image bases that would place the faulting PC on one of those five
instructions, **only one is page-aligned**: `0x13EA76000` (the other four end `…d44`,
`…d0c`, `…ffc`, `…df4`). And `0x13EA76000 + 0x1577c = 0x13EA8B77C` — the faulting PC
exactly. That instruction is `ldaddal w0, w0, [x2]`. Corroborating: every frame in the
backtrace (`0x13EA8B770`, `0x13EA8BF3C`, `0x13EA8C04C`, `0x13EA87584`) maps to an RVA
inside `.text` under the same base, and the loader's own reported region was
`0x13EA00000 + 0x600000`.

**FEAT_LSE is an ARMv8.1 feature. Cortex-A53 is ARMv8.0. The instruction is undefined
there.** So: a Pi 3 does not fail to find its SD card or its interrupt controller — it
dies executing the loader's first atomic, and it would do so no matter how much of the
rest of this list were fixed. This is X1, and it is why X1 is stage 0.

### Measured ISA floor of the shipped binaries

Same technique, ELF sections, whole base system:

| binary | FEAT_LSE (v8.1) | `ldapr` FEAT_LRCPC (v8.2) | crypto (AES/SHA/PMULL) |
|---|---|---|---|
| `haiku_loader.efi` | 5 (`ldaddal`) | 0 | **0** |
| `kernel_arm64` | **1194** (`ldaddal` 650, `ldsetal` 180, `casal` 171, `ldclral` 154, `swpal` 39) | 216 | **0** |
| `libroot.so` | 94 | 17 | **0** |

The real floor of the shipped image is therefore **ARMv8.1 LSE + ARMv8.2 LRCPC** — legal
on Cortex-A76, which is why run 3 booted, and illegal on Cortex-A53.

**One theoretical hazard measured away.** `+crypto` in the build flags looked like a Pi 5
blocker, because the Pi 5's SoC does not implement the optional AES/SHA/PMULL extensions.
The scan found **zero** crypto instructions in loader, kernel or libroot: GCC does not
select them without intrinsics or an explicitly vectorised loop. The concern is real in
principle and **not realised in the current base system**. It would become real the moment
any crypto-using code is compiled with these flags, which is a reason to fix X1 anyway,
but it is not what breaks a Pi 5 today. Recorded here because a refuted concern is worth
as much as a confirmed one.

### Why the passing runs did not expose three of the (X) items

The three defects below would each be invisible to anyone who only ran the boots above.
Rather than leave that as a contradiction, the machine's actual device tree was dumped
(`-M virt,acpi=off,dumpdtb=virt.dtb`) and parsed:

```
psci            compatible = arm,psci-1.0 | arm,psci-0.2 | arm,psci
psci            method     = hvc
pcie@10000000   compatible = pci-host-ecam-generic
pcie@10000000   bus-range  = [0, 255]
timer           compatible = arm,armv8-timer | arm,armv7-timer
timer           interrupts = [1,13,772, 1,14,772, 1,11,772, 1,10,772]
intc@8000000    compatible = arm,cortex-a15-gic
cpus            #address-cells = [1]
cpu@0           compatible = arm,cortex-a57
```

- **X9/X10 (PSCI).** The loader compares only against the *first* `compatible` string.
  `arm,psci-1.0` happens to be first here, so the match succeeded, `sPsciCallFn` was set,
  and run 2 found two CPUs. The code worked **by string order, not by design**.
- **X38 (`bus-range`).** `bus-range = <0 255>` starts at bus 0, so discarding the property
  changed nothing observable. Exactly the latency predicted from reading it.
- **X22 (timer INTID).** PPIs 13/14/11/10 are GIC INTIDs 29/30/27/26 — precisely the
  values the kernel hard-codes. The hard-coded constant is right here because QEMU
  conforms to the same Base System Architecture convention the constant was copied from.

In all three cases the defect is **latent, not absent**. That distinction is the whole
reason this document pairs reading with measurement: reading tells you what the code
assumes, measurement tells you which assumptions this particular machine happens to
satisfy.

### What was not run

- **RISC-V.** `qemu-system-riscv64` was checked for and no riscv64 image was built, so
  there is **no measurement of any kind** on riscv64 — see §5 for what the tree says
  about it statically.
- **Display path — attempted, inconclusive.** `virtio-gpu-pci` is not a valid device model
  in this QEMU build (`'virtio-gpu-pci' is not a valid device model name`); only `ramfb`
  is offered. A fifth run with `-device ramfb` added to run 1's command line reached
  `bfs: mounted "Haiku"` and then degenerated into repeated
  `ReadFileData(...) failed to read data: I/O error` / `file_cache: read pages failed`
  against the virtio block device, and did not reach `app_server` within 300 s. Runs 1-4
  used byte-identical storage arguments and never did this, and this run was sharing a
  TCG-only host with other work, so **the I/O errors are not attributed to anything in
  this tree and no conclusion is drawn from run 5 either way.** `con_init: trying module
  console/frame_buffer/v1` appears in runs 1, 2 and 5 alike, so it is not evidence of a
  framebuffer being found. The `app_server` virtual-screen failure in runs 1-4 therefore
  remains attributable to "no display device was supplied" (and `video.cpp:200-205`
  handles that case deliberately, see §3), but **whether the framebuffer path initialises
  a screen when a display is present is untested here.**
- **Real Pi hardware of either generation.** None exists here. Nothing below claims a Pi
  was booted.

---

## 2. (X) — the full list, 42 items

### Build and toolchain (8)

**X1 — `build/jam/ArchitectureRules:52`.** `case arm64 : archFlags += -mcpu=neoverse-n1+crypto ;`
This is the **only** arm64 ISA line in the tree and it is entirely unconditional. `archFlags`
flows into `HAIKU_CCFLAGS/C++FLAGS/LINKFLAGS/ASFLAGS_arm64` (lines 87-90) and from there
into both `HAIKU_KERNEL_*FLAGS` (391-392) **and** `HAIKU_BOOT_*FLAGS` (403-404). There is
no `-march` floor, no `-moutline-atomics`, and no runtime ISA dispatch anywhere. Measured
consequence: §1 run 4. Note lines 53-66 are fourteen lines of *comment* about a
Graviton-generation ISA opt-in that added **zero build logic** here — the mechanism it
describes is a package-recipe convention, and it can only raise the baseline, never lower
it. By contrast `build/scripts/build_cross_tools_gcc4:83` gets this right
(`--with-arch=armv8-a --with-tune=neoverse-n1`: ISA floor low, tuning high) — but that
governs the package toolchain, not the OS build.

**X2 — `src/bin/sve_test/sve_test.c:46,56-57,105-106,128-129` and
`src/bin/sve_test/sve_fork_test.c:57,74`.** Bare `.arch_extension sve` and `rdvl` inline
asm with **zero** `#ifdef __aarch64__` guards (verified: `grep -c` is 0 in both files).
`src/bin/Jamfile:264` includes the directory ungated and `build/jam/packages/Haiku:256`
ships the binary on every architecture. This is a **hard compile failure for any riscv64
or x86 build of this tree** — the most immediate blocker to even attempting a second
architecture. `src/bin/sve_test/Jamfile` meanwhile comments "Verification instrument, not
shipped", contradicting the package list.

**X3 — `build/jam/packages/Haiku:149`.** The `ena` driver is listed unfiltered, so a
RISC-V or Pi image ships a driver for a NIC that platform cannot have.

**X4 — `build/jam/images/definitions/minimum:350-383`.** The conventional-NIC driver block
excludes `arm64` while *including* `riscv64`. On the reasoning that arm64 means EC2 and
EC2 means ENA. A Pi has neither ENA nor any of these NICs, so an arm64 Pi image would
contain **no network driver at all**.

**X5 — `build/jam/images/definitions/common-tail:127-131`, `minimum:502`, `minimum:523-525`.**
Three sites using the architecture token `arm64` as a proxy for "this is an AWS EC2
instance". The condition to test is a platform property; the token tests an ISA.

**X6 — `build/jam/images/MMCImage:170`.** No `boot.scr` is generated for arm64 and there
is no staging of `config.txt` or vendor firmware blobs. The MMC image is shaped for a
UEFI/EFI-boot machine. Pi 5 can UEFI-boot with third-party firmware; Pi 3 conventionally
cannot, so this is a real gap for Pi 3 and an open question for Pi 5.

**X7 — `build/jam/ArchitectureRules:638-640`.** `case arm : return ;` in
`ArchitectureSetupWarnings` disables `-Werror` tree-wide for 32-bit arm. Whatever the
original reason, the effect is that 32-bit arm is the one architecture whose decay is
invisible.

**X8 — `build/jam/board/`.** Dead weight: a board definition for a 2014-era device, with
no `BoardSetup` or `HAIKU_BOARD_*` consumer anywhere in the tree. Any bring-up that reads
it as the extension point will waste time; it is unreachable.

### EFI loader and FDT parsing (13)

**X9 — `src/system/boot/platform/efi/arch/arm64/arch_dtb.cpp:78`.**
`if (strcmp(compatible, "arm,psci-1.0") == 0)` — an exact compare against only the first
string of a stringlist property, where the tree's own `dtb_has_fdt_string()` helper exists
for precisely this. A device tree declaring `arm,psci-0.2` first, or only, never matches
and its `method` is never read. Measured: worked here only because of string order (§1).

**X10 — `src/system/boot/platform/efi/arch/arm64/arch_smp.cpp:255-258`, called from `:195`.
The most severe item in this list.** `arm64_handle_fdt_cpu_node()` sets
`sCpuEnableMethod = CpuEnableMethod::Psci` from the **cpu node's** `enable-method`
property — entirely independently of whether `arm64_handle_fdt_psci_node()` ever ran.
`sPsciCallFn` (`:52`, zero-initialised) is set *only* by that other function, which X9 can
skip. So a device tree with `enable-method = "psci"` on its CPUs and `arm,psci-0.2` first
on its psci node reaches `arch_smp_boot_other_cpus()` and executes
`sPsciCallFn(PSCI_CPU_ON, ...)` **through a NULL pointer** — the loader jumps to address 0
during secondary bring-up, on a completely ordinary device tree. `psci_conduit` also stays
`NONE`, so even a surviving kernel could never power off or reboot. The `default:` arm of
that switch is commented "Unreachable, we set sCpuCount to 0 already", which is doubly
wrong: the fallback sets it to **1**, not 0, and the Psci arm is reached with a NULL
pointer rather than being unreachable. The ACPI path does not have this split-state bug
(G24) and is the shape to copy.

**X11 — `arch_smp.cpp:270-272`.** `strcmp(method, "smc")` on the unchecked result of
`fdt_getprop(..., "method", NULL)`. A psci node with no `method` property faults the
loader.

**X12 — `arch_smp.cpp:236-247`.** Unchecked dereference of three `fdt_getprop()` results:
the parent's `#address-cells`, the node's `reg`, and `cpu-release-addr`. A `spin-table`
cpu node missing `cpu-release-addr`, or a `cpus` node without `#address-cells`, faults in
the loader. (This machine's tree happened to have `#address-cells`, §1.)

**X13 — `arch_smp.cpp:136-166`.** The MPIDR search loop in `arm64_secondary_startup()` has
**no termination condition** and a hard-coded `ldr x2, =24` for `sizeof(platform_cpu_info)`.
A secondary whose masked MPIDR is not in `sCpus` walks memory forever, with no stack. The
mask (a `bic` of bits 31 and 24) is also only correct for MPIDR layouts that leave Aff3
unused.

**X14 — `src/system/boot/platform/efi/arch/arm64/arch_dtb.cpp:63-76`.** The FDT interrupt-
controller branch fills only `regs1` and `regs2`. It never sets `regs3` (the ITS),
`gicr_region_count`/`gicr_regions`, `pe_count`, or `pmu_gsiv` — all of which the ACPI
branch does fill (`arch_acpi.cpp:115-182,243-408`; ITS specifically at `:356-361,388-389`).
Four consequences on an FDT-only machine: (a) MSI/LPI is unavailable even where the device
tree has a `gic-its` child; (b) the GICv3 redistributor extent is *guessed* as
`fGicrStride * smp_get_num_cpus()` (`arch_int_gicv3.cpp:115-118`), wrong whenever the
frames are non-contiguous or not one-per-online-CPU; (c) the `pe_count` bound on the
redistributor walk is dead code on FDT (`arch_int_gicv3.cpp:163`), so a too-generous guess
walks unmapped space; (d) PMU sampling is permanently off (`arch_pmu.cpp:1418-1427`) even
though the device tree's `arm,armv8-pmuv3` node states the PPI and `dtb_get_interrupt()`
could decode it. It also reads only `reg` cells 0 and 1, so a GICv3 tree listing GICD,
GICR, GICC, GICH, GICV loses everything after GICR.

**X15 — `arch_dtb.cpp:64-76`.** The match loop has no `break` and the `kind[0] == 0` guard
sits *outside* it, so a node whose `compatible` list matches two table rows takes the
**last** match's kind plus a second `dtb_get_reg()` pass. `compatible = "arm,gic-400",
"arm,cortex-a15-gic"` — a plausible Pi-era tree — hits two rows. Both map to GICv2, so
today the answer is right by luck rather than by construction.

**X16 — `arch_dtb.cpp:26`.** The file's own header comment:
`/* TODO: Code taken from ARM port just for building purposes */`. Concretely, it does
**no timer-node parsing at all**, whereas the 32-bit sibling
`src/system/boot/platform/efi/arch/arm/arch_dtb.cpp` carries a full `kSupportedTimers[]`.
This is the upstream cause of X22/X23: the loader never looks at the timer node the
firmware provides.

**X17 — `src/system/boot/platform/efi/dtb.cpp:548`.** `dtb_get_clock_frequency()` returns
`-1` with no resolution of a `clocks` phandle. Combined with
`arch/arm64/arch_start.cpp:212-213`, a console whose clock is only discoverable through a
phandle goes dead at `ExitBootServices` — i.e. the bring-up loses its console at exactly
the transition where it is most needed. `kSupportedUarts[]` (`dtb.cpp:78-97`) already
lists both `arm,pl011` and `brcm,bcm2835-aux-uart`, so the *match* is there and only the
clock is missing.

**X18 — `src/system/boot/platform/efi/devices.cpp:480-489`** (implementation `202-362`).
GPT auto-grow is gated by `#ifdef __aarch64__` — **architecture, not platform** — and runs
against **every physical block device on every boot**, with no `Media->ReadOnly` and no
`Media->RemovableMedia` check. The behaviour is correct and wanted for an EBS root that
was resized; it is not something to do unconditionally to whatever is plugged into a Pi.

**X19 — `devices.cpp:442`.** `efi_handle handles[noOfHandles];` — a variable-length array
sized by a value the firmware returns. This is the exact hazard the bounce buffer at
`85-96` in the same file was added to remove.

**X20 — `src/system/boot/platform/efi/arch/arm64/arch_acpi.cpp:31`.** A literal 24 MHz
PL011 clock. Correct for the platforms measured; a fact about a platform, written as a
constant.

**X21 — `arch_acpi.cpp:385,389`.** Hard-coded GIC register window sizes, and the MADT path
sets `regs1.start`/`regs2.start` without ever setting `.size`.

### Kernel (16)

**X22 — `src/system/kernel/arch/arm64/arch_timer.cpp:47-48`**, used at `:122`, installed at
`:148`. `#define TIMER_IRQ_EL1_PHYS 30` / `TIMER_IRQ_EL2_PHYS 26`, selected only by
`CurrentEL`. The ACPI GTDT is never parsed and the FDT `arm,armv8-timer` `interrupts`
property is never read, even though the loader already has a working 3-cell PPI/SPI decoder
for it (`dtb.cpp:494-519`) and never calls it here. Right on any Base-System-Architecture-
conformant platform, including Pi 5's GIC-400 — and definitively wrong on Pi 3, whose
per-core timer interrupts are numbered 0-3 in a SoC-local interrupt controller's own space.
Measured latent, §1.

**X23 — `headers/private/kernel/arch/arm64/arch_kernel_args.h:31-49`.** `arch_kernel_args`
has **no timer field at all**: no frequency, no INTID. So even a loader that parsed the
GTDT or the device tree would have nowhere to put the answer. X22 is not a one-line patch;
it needs this structure extended first.

**X24 — `arch_timer.cpp:108,117`.** `sTimerFrequency = READ_SPECIALREG(CNTFRQ_EL0);` with
no validity check, then used as a **divisor**:
`sTimerMaxInterval = ((uint64)INT32_MAX * 1000000) / sTimerFrequency;`. Firmware is not
guaranteed to program `CNTFRQ_EL0`; a zero is a divide-by-zero inside `arch_init_timer()`,
before the console is useful. `dtb_get_clock_frequency()` (`dtb.cpp:523-547`) exists and
is never applied to the timer node.

**X25 — `src/system/kernel/arch/arm64/arch_int.cpp:96-107` with
`src/system/kernel/arch/arm64/arch_smp.cpp:33-34,41,51-52`.**
`arch_int_init_post_vm()` constructs only `gicv2` and `gicv3` and returns `B_ERROR`
otherwise, leaving `InterruptController::sInstance == NULL`. `arch_smp_send_ici()`,
`arch_smp_send_multicast_ici()` and `arch_smp_send_broadcast_ici()` then dereference it
**with no NULL check** — unlike `arch_int_enable_io_interrupt()` (`arch_int.cpp:47-49`),
which does check. Pi 3's SoC-local interrupt controller matches nothing in the loader's
table, so a Pi 3 boot that got past X1 would be: kind never set → `B_ERROR` → NULL
dereference at the first inter-processor interrupt. The loader can also select
`INTC_KIND_GICV1`, `INTC_KIND_OMAP3` or `INTC_KIND_PXA` (`arch_dtb.cpp:35,39,40`), none of
which the kernel can construct — same silent path.

**X26 — `src/system/kernel/arch/arm/arch_int_gicv2.cpp:29,37`** (compiled into the arm64
kernel via `arch/arm64/Jamfile:28`). `gicd_addr ? gicd_addr : GICD_REG_START` and
`gicc_addr ? gicc_addr : GICC_REG_START`, i.e. `0x08000000`/`0x08010000` from
`src/system/kernel/arch/arm/gicv2_regs.h:4,27` — **QEMU `virt` addresses used as a silent
fallback**. On a Pi 5 that is unbacked MMIO, and the driver writes `GICD_CTLR` into nothing,
booting with a dead interrupt controller instead of reporting failure.

**X27 — `arch_int_gicv2.cpp:91-102`.** `_EnableInterrupt()` programs `GICD_ISENABLER` and
`GICD_IPRIORITYR` but **never `GICD_ITARGETSR`** — which is defined at `gicv2_regs.h:20`
and written nowhere in the tree. On GICv2 an SPI is delivered only to the CPUs named in
its 8-bit ITARGETSR byte, so an "enabled" interrupt can be enabled-but-unroutable
depending on what firmware left there. GICv3 has no analogue because it programs affinity
routing explicitly, which is why this has never been felt.

**X28 — `src/system/kernel/arch/arm/soc.h:25`.**
`virtual int32 AssignToCpu(int32 irq, int32 cpu) { return cpu; }`, not overridden by
`GICv2InterruptController`. So on any GICv2 platform `arch_int_assign_to_cpu()`
(`arch_int.cpp:62-69`) **reports success for every interrupt while nothing moves**, and
the generic IRQ-balancing code believes it. Implemented for real only on GICv3 — i.e. only
on Graviton.

**X29 — `src/system/kernel/arch/arm64/arch_int_gicv3.cpp:61,138,385`.** Three `panic()`s on
the GICv3 bring-up path, at exactly the three points an FDT-described GIC is most likely
to differ from a MADT-described one: distributor unmappable, redistributor region
unmappable, and `panic("gicv3: no redistributor for affinity ...")`. Given X14, the third
is the *likely* outcome of an FDT GICv3 boot, and it fires with no console fallback.

**X30 — `arch_int_gicv3.cpp:53-55`.** The distributor mapping is silently widened to at
least 64 KB regardless of "whatever a device tree claims". Defensible on the architecture
(GICD_PIDR2 sits at 0xffe8) but it **overrides rather than validates** the firmware-stated
size, so a platform genuinely mapping a smaller window gets a mapping over addresses it
does not decode.

**X31 — `arch_int.cpp:142,325`.** `static int page_bits = 12;` and
`CalcStartLevel(48, 12)` hard-code the translation granule and VA width inside the
page-fault fixup walker — a **third** copy of constants already duplicated at
`arch_vm_translation_map.cpp:42` and `:117-118`. Nothing anywhere reads
`ID_AA64MMFR0_EL1.PARange`/`TGran*` or `TCR_EL1.T1SZ` to check. Not a Pi break (both Pis
are 4 KB/48-bit) but a 16 KB-granule or 40-bit-PA platform would corrupt page tables
silently. The file flags it itself: `// TODO: reuse things from VMSAv8TranslationMap` at
`:139`.

**X32 — `arch_int.cpp:183`.** `uint64 asid = READ_SPECIALREG(TTBR0_EL1) >> 48;` assumes
8-bit ASIDs without reading `ID_AA64MMFR0_EL1.ASIDBits`, matching `kAsidBits = 8` in
`VMSAv8TranslationMap.cpp:29`. Architecturally safe (8 is the minimum) but true by
convention, and the shift captures 16 bits where 16 are implemented.

**X33 — `src/system/kernel/arch/arm64/arch_system_info.cpp:147`.**
`node->data.package.cache_line_size = CACHE_LINE_SIZE;` — a compile-time 64 from
`headers/private/kernel/arch/arm64/arch_cpu.h:10`, reported to userland through the
topology API. `arch_cpu_sync_icache()` (`arch_cpu.cpp:348-352`) already reads the real
sizes from `CTR_EL0`; the *reported* value does not. Right on both Pis by coincidence,
wrong on any 128-byte-line part.

**X34 — `src/system/kernel/arch/arm64/arch_cpu.cpp:184-190` and `:218-227`.** Topology is
filled as a single package with `topology_id[CPU_TOPOLOGY_CORE] = i` and SMT 0, and
`arm64_get_hwcap()` publishes the **boot CPU's** ID registers as the whole machine's
HWCAP, with "a homogeneous ISA across CPUs is assumed" written in the comment.
`arch_system_info.cpp:150-156` likewise samples `MIDR_EL1` on the boot CPU only. True on
every Graviton and true on both Pis; the objection is that it is **stated rather than
checked**, so a heterogeneous SoC gets wrong answers silently.

**X35 — `src/system/kernel/arch/arm/arch_debug_console.cpp:118`.**
`sArchDebugUART = arch_get_uart_pl011(0x9000000, 0x16e3600);` — the QEMU `virt` PL011 base
and clock, hard-coded. This is the 32-bit ARM console path, not the arm64 one (which is
clean, G13); it is listed because it is the only hard-coded UART base left in the tree and
it is the file a bring-up would be tempted to copy.

**X36 — `src/system/kernel/arch/arm64/arch_pmu.cpp`, diagnostic text.** Run 2 — a machine
with **no ACPI tables at all** — printed `arm64_pmu: no overflow interrupt gsiv in madt`.
The *behaviour* is right (G3), but the message names a table that does not exist on that
platform, which will mislead the first person bringing up an FDT-only board. Measured, §1.

**X37 — `src/add-ons/kernel/bus_managers/acpi/acpi.cpp:208`.** ACPI absence returns
silently. Correct not to fail, but on a platform where ACPI *should* have been present
this is indistinguishable from success, and it is the one place a firmware regression
would be caught.

### PCI, storage and network device paths (5)

These files are read-only for this audit — other work owns them — so every item here is an
observation with no accompanying change.

**X38 — `src/add-ons/kernel/busses/pci/ecam/ECAMPCIControllerFDT.cpp:27-32`.** The
`bus-range` property is read, printed to the log as `busBeg`/`busEnd`, and then **never
assigned** to `fBusOffset` or `fValidBuses`. A device tree whose ECAM window does not
start at bus 0 is misprogrammed. Measured latent here (`bus-range = <0 255>`, §1).

**X39 — `src/add-ons/kernel/busses/pci/ecam/ECAMPCIController.h:136-140`.** The comment
asserts the offset is "zero for a window that starts at bus 0, which is every case
measured on AWS **and the only case the device tree path produces**." The first clause is
a measurement and is fine. The second is a claim about all device trees everywhere,
derived from AWS measurements — the textbook shape of an (X): a platform fact promoted to
an invariant of a generic path.

**X40 — `ECAMPCIControllerFDT.cpp:113`.** `for (int bus = 0; bus < 8; bus++)` caps FDT INTx
interrupt-map routing to buses 0-7.

**X41 — `src/add-ons/kernel/busses/mmc/Jamfile:11-15`.** The `sdhci` add-on is built from
`sdhci_pci.cpp` and `sdhci_acpi.cpp` **only — there is no `sdhci_fdt.cpp`**, and
`sdhci_pci.cpp:269` requires `bus == "pci"` while `sdhci_acpi.cpp:182` requires ACPI. Yet
`sdhci`, `mmc` and `mmc_disk` are all in the arm64 boot module links
(`build/jam/packages/Haiku:353,368`, `minimum:420`). A Pi's SD host controller is an
on-SoC MMIO device described in the device tree, so it can never bind: **SD-card boot is
impossible on either Pi**, and the boot module list currently implies otherwise.

**X42 — USB, tree-wide.** There is **no `dwc2`/`dwc_otg` driver at all**, and `xhci`,
`ehci`, `ohci` and `uhci` all match on `bus == "pci"` exclusively. On a Pi 3, whose
Ethernet hangs off USB, this combines with X4 and X41 to mean there is **no working
storage path and no working network path whatsoever** — which is the honest reason Pi 3 is
staged last below, independent of X1.

---

## 3. (G) — correctly platform-specific, 28 items

Abbreviated; these are closed, and several are the templates the (X) fixes should follow.

**Gating done by register readback, not by platform identity (5).**
`arch_timer.cpp:121` `sUseEL2Timer = (READ_SPECIALREG(CurrentEL) >> 2) >= 2;` and
`arch_pmu.cpp:404` `PMEVTYPER_NSH` use the identical mechanism — **so the EL2/VHE handling
is generic, not Graviton-specific**: a Pi whose firmware enters at EL2 takes the same
branch as bare-metal EC2, for the same reason. `arch_cpu.cpp:67-72` gates SVE on
`ID_AA64PFR0_EL1.SVE` (neither Pi core implements SVE; both take the correct branch);
`arch_cpu.cpp:150-158` and `arch_vm_translation_map.cpp:61-72` gate hardware access/dirty
bits on `ID_AA64MMFR1_EL1.HAFDBS` and CnP on `ID_AA64MMFR2_EL1.CNP` (Cortex-A53 has
neither); `arch_cpu.cpp:231-318` derives every HWCAP bit from `ID_AA64ISAR0/1_EL1` and
`ID_AA64PFR0_EL1`.

**PMU (4).** `arch_pmu.cpp:1399-1412` takes presence from `ID_AA64DFR0_EL1.PMUVer`,
declining both "none" and "implementation-defined", and gates 64-bit counters on the
version. `:1418-1427,701-711` install sampling only on a firmware-stated interrupt and
fall back to the software profiling timer otherwise — the *gate* is right for a Pi; only
the FDT plumbing that would supply the value is missing (X14). `:1432` makes the whole
facility opt-in because a hypervisor may trap EL1 PMU accesses — detect-and-default-off
rather than assume, and the model the rest of the tree should follow.

**Graceful absence (5).** `arch_real_time_clock.cpp:28,44` — firmware time, `0` means
unknown, `set_hw_time` a documented no-op: **neither Pi has a battery-backed clock and
this degrades rather than faults.** `arch_int_gicv3.cpp:598` + `arch_int.cpp:119-126` —
**the ITS is genuinely optional**, `B_NAME_NOT_FOUND` is non-fatal, a machine without one
boots with no MSI. `arch_cpu.cpp:325-329` returns `B_ERROR` with a diagnostic instead of
executing `smc` where there is no secure monitor. `arch_platform.cpp:26-28` publishes the
ACPI root only if present while `gFDT` is unconditional. `src/system/boot/platform/efi/video.cpp:200-205`
handles GOP absence generically (`frame_buffer.enabled = false`, `B_ERROR`) with the
mode-enumeration/EDID/32-30-24-16bpp path intact — **headless is a consequence of the
hardware, not an assumption in the code**.

**Dual-firmware discovery (2).** `src/system/boot/platform/efi/start.cpp:259,261` calls
`acpi_init()` then `dtb_init()`, and the FDT branch fills only fields ACPI left zero
(`if (uart.kind[0] == 0)`, `if (interrupt_controller.kind[0] == 0)`). The device_manager
root node carries `B_FIND_MULTIPLE_CHILDREN` so the ACPI and FDT bus managers coexist.
The *architecture* is right; only the FDT branch's coverage is thin (X14).

**Console (1).** `arch/arm64/arch_debug_console.cpp:161-188` dispatches purely on
`uart.kind` across PL011/LINFLEX/8250/Samsung with **no hard-coded base**, returning
`B_ERROR` when nothing matched; selection is SPCR → DBG2 → FDT (`compatible` plus
`/chosen stdout-path`) → EFI `serial_io`, and `kSupportedUarts[]` already lists both Pi
console bindings with `dtb_get_reg_shift()` supplying the 8250's shift.

**GIC negotiated at runtime (5).** `arch_int_gicv3.cpp:70-72` takes the GICv4-vs-v3
redistributor stride from `GICD_PIDR2.ArchRev`. `:265-283,412-422` gate extended SPI and
extended PPI on `GICD_TYPER.ESPI` and `GICR_TYPER.PPInum`, so a GIC-400-class part takes
the base path. `gicv3_its.cpp` (whole file) negotiates everything — `GITS_TYPER` for ITT
entry size and ID widths, a `GITS_BASER` page-size retry loop, `GICD_TYPER.IDbits` for the
LPI range, per-CPU collections capped by `fMaxCollections` — and `gicv3_its.h:30-43`
explicitly refuses to synthesise a DeviceID. `gicv3_regs.h:24-29,246-254` records why the
inline-asm accessors exist and records a metal-versus-virtualised register difference as a
measurement. `arch_int.cpp:96-107` is genuinely a runtime dispatch on the loader-supplied
kind (its defect, X25, is the missing NULL check, not the dispatch).

**GICv2 is genuinely supported (1).** `src/system/kernel/arch/arm/arch_int_gicv2.cpp` is
compiled into the arm64 kernel (`arch/arm64/Jamfile:8,28`), implements
`SendMulticastIci`/`SendBroadcastIci` via `GICD_SGIR`, and both drivers run `_PerCpuInit()`
through `call_all_cpus_sync()`. `GICD_SGIR`'s 8-bit target list caps GICv2 IPIs at 8 CPUs —
fine for both Pis. It is the older, less careful driver (X26, X27, X28), but it exists and
run 1 and run 2 both used it.

**SMP mechanisms (3).** `src/system/boot/platform/efi/arch/arm64/arch_smp.cpp:53-58,197-199` —
**spin-table is implemented**, which is what Pi 3 needs. `:99-100` boots single-CPU when
the release mechanism is unidentified rather than attempting a bring-up it cannot do
(defeated in practice by X10, which sets the method from a different node). `arch_acpi.cpp:414-422`
with `arch_smp.cpp:78-85` sets `sPsciCallFn`, `sCpuEnableMethod` **and** `psci_conduit`
together from the FADT — the correct shape, and the fix template for X10.

**Honest reporting (2).** `arch_system_info.cpp:37,43,72,191-194,219-220` already has
Cortex-A53, Cortex-A76 and the Pi SoC vendor in its part table, prints the raw MIDR and
says so when a part is unrecognised (observed verbatim in run 1), and returns
`B_NOT_SUPPORTED` rather than 0 Hz when the PMU-based frequency measurement is
unavailable — which is exactly what a Pi with the PMU off hits.

---

## 4. (P) — already generic, 14 items

`src/system/kernel/arch/arm64/arch_vm.cpp` (all 140 lines; `arch_vm_supports_protection()`
encodes PAN/AP semantics from the architecture). `VMSAv8TranslationMap.cpp:127-128,239-252,323-327,589-597`
— `fPageBits`/`fVaBits` are constructor parameters used consistently and `CalcStartLevel()`
is a real computation, so the file is granule-agnostic and only its caller hard-codes
(X31). `arch_int.cpp:302-451` — exception-class decode, the syscall-immediate bounds check,
and user/kernel time accounting are architected-only. `arch_cpu.cpp:373-402` — range and
list TLB invalidation degrade to a global `tlbi vmalle1` using only architected
instructions: slow, portable, honest. `arch_cpu.cpp:58-63` — the comment explaining why no
`CurrentEL` gate is needed for the `CPACR_EL1`/`ZCR_EL1` writes is correct and
platform-independent. `arch_cpu.cpp:348-352` — real I- and D-line sizes from `CTR_EL0`.
`gicv3_regs.h` register and field definitions, straight from the architecture spec.
`headers/private/kernel/arch/arm64/arch_kernel.h:14,28` — a VA layout valid for any
48-bit-VA platform. The generic UARTs under `src/system/kernel/arch/generic/`
(`debug_uart_8250.cpp`, PL011, LINFLEX, Samsung) all take base, clock and `reg_shift` as
parameters. `src/system/kernel/arch/generic/acpi_irq_routing_table.cpp` is compiled into
the arm64 kernel but reached only through the ACPI module, so an FDT-only boot never
enters it. `src/system/boot/platform/efi/dtb.cpp` as a whole (696 lines) is a real,
working FDT consumer compiled for every non-x86 target (`Jamfile:19-21,51-54`,
`_BOOT_FDT_SUPPORT`) with `dtb_get_interrupt()`, `dtb_get_reg_shift()` and
`/chosen stdout-path` handling — the helpers X16/X22 need already exist here.
`src/add-ons/kernel/bus_managers/fdt/` is a working FDT bus manager; run 2 exercised it
(`publish device: ... bus/fdt/blob`). `ECAMPCIController.cpp:29-50` dispatches on
`B_DEVICE_BUS == "fdt"` with `compatible == "pci-host-ecam-generic"` — **an FDT PCI path
exists on arm64**, confirmed by run 2's `finalize PCI controller from FDT`. And
`src/add-ons/kernel/network/.../generic_msi.cpp` degrades cleanly with no ITS.

---

## 5. RISC-V and 32-bit arm: the honest state

**No riscv64 measurement was taken.** No riscv64 image was built and none was booted.
What the tree says statically:

`riscv64` is **stub-complete and correctly gated** in the build system.
`build/jam/ArchitectureRules:68` is `case riscv64 : archFlags += -march=rv64gc ;` — which
is, ironically, the *best-constructed* ISA line in the file: a named baseline profile
rather than a specific vendor part. The architecture appears in the image definitions and
`minimum:350-383` even includes it in the conventional-NIC block that arm64 is excluded
from (X4).

But it is **sweep-maintained, not developed**. The only substantive riscv-specific commit
in four years is `src/system/libroot/os/arch/riscv64` (2026-04-27);
`src/system/libroot/posix/arch/riscv64` still sits on its original 2021-05-30 commit and
`glue/arch/riscv64` on 2021-05-17. And the immediate blocker is not riscv-specific at all:
**X2 means a riscv64 build of this tree does not compile**, because unguarded aarch64
inline assembly is in the default package list.

**32-bit `arm` is actively decayed**, and the historical Pi route was deliberately removed:
`63816858af` (2022-06-28) "boot/platform/u-boot: Drop ARM support." and `84360889a2`
"ldscripts: remove linker script for Raspberry Pi". `build/jam/board/` is unreachable dead
weight (X8), `-Werror` is off for the whole architecture (X7),
`src/system/libroot/posix/arch/arm` has not been touched since 2016-08-14, and the one
hard-coded UART base left in the tree is on this path (X35). This **mostly does not
matter**: a Pi 5 or a Pi 3 would be brought up as an **arm64** target, so 32-bit arm is
not on the critical path for either. It matters only in that the 32-bit arm loader is the
one place a working FDT timer parser already exists (X16), so it is worth reading, not
reviving.

**On `-mcpu=neoverse-n1` and the Pi cores, measured rather than reasoned:** it produces a
binary that runs on Cortex-A76 (run 3 booted) and does not run on Cortex-A53 (run 4
faulted on an `ldaddal`). The `+crypto` half of the flag is currently inert (no crypto
instructions in loader, kernel or libroot) but remains a latent hazard on the Pi 5 SoC,
which does not implement those extensions.

---

## 6. Staged plan, named by the (X) items each stage clears

No stage below is dated. Stages 0-2 need **no hardware we do not have** and are verifiable
on the QEMU configurations already demonstrated in §1. Stages 3-5 are marked where they
need hardware that does not exist here.

### Stage 0 — make a second ISA compile and a non-v8.2 core execute
**Clears X1, X2, X3, X7, X8.** No hardware needed.

X2 first, because it is a compile failure and nothing else can be attempted past it: guard
the SVE test sources, or gate their `SubInclude` and package entry on arm64. Then X1: a
per-target ISA baseline, with `-march` set to a *profile* and `-mtune` to a part — the
shape `build/scripts/build_cross_tools_gcc4:83` already uses. **Verification without a
Pi:** rebuild, re-run the §1 Capstone scan and confirm the LSE and `ldapr` counts drop to
zero for a v8.0 target, then re-run run 4's exact command and confirm it gets past
`Partition::Partition`. That is a real, repeatable gate on this host.

### Stage 1 — stop the FDT loader crashing on ordinary device trees
**Clears X9, X10, X11, X12, X13, X15.** No hardware needed.

X10 is the priority: a NULL indirect call during secondary bring-up on a perfectly normal
device tree. Fix it the way the ACPI path already does (G24) — set conduit, call function
and enable-method in one place — and have the Psci arm refuse to run when `sPsciCallFn` is
NULL. X9 becomes a call to the existing `dtb_has_fdt_string()`. X11/X12 are NULL checks.
X13 needs a loop bound and `sizeof` instead of a literal. **Verification without a Pi:**
QEMU accepts a supplied device tree, so a tree with `arm,psci-0.2` first, a psci node with
no `method`, and a `cpus` node without `#address-cells` each reproduce a specific defect
now and must boot after. Three negative tests, no hardware.

### Stage 2 — make the FDT path as capable as the ACPI path
**Clears X14, X16, X17, X22, X23, X24, X36. Touches X29, X30 by removing their trigger.**
No hardware needed for most of it.

Extend `arch_kernel_args` with the timer fields (X23) — an ABI change and therefore the
first move, not the last. Parse the timer node in the arm64 loader using the decoder that
already exists (X16, X22), with `CNTFRQ_EL0` as the *fallback* and a zero check (X24).
Resolve `clocks` phandles (X17). Fill `regs3`, `gicr_regions`, `pe_count` and `pmu_gsiv`
from the device tree (X14), which also turns MSI and PMU sampling on for FDT platforms and
removes the guess that makes X29's third panic likely. Correct X36's message text.
**Verification without a Pi:** run 2's command already exercises all of it; a GICv3 FDT
configuration (`-M virt,acpi=off,gic-version=3`) exercises the redistributor and ITS paths
that MADT currently monopolises.

### Stage 3 — Pi 5 platform work
**Clears X6 (Pi 5 half), X18, X19, X20, X21, X26, X27, X28, X38, X39, X40, X41 (FDT sdhci).**
**Needs hardware we do not have** for the final verification; most items can be *written*
and partly tested without it.

X18 and X19 are safe and testable anywhere: gate GPT auto-grow on a platform condition
rather than `#ifdef __aarch64__` and add the `ReadOnly`/`RemovableMedia` checks; replace
the firmware-sized VLA. X26, X27 and X28 are the GICv2 driver debt — remove the QEMU-virt
fallback bases in favour of an honest failure, program `ITARGETSR`, and either implement
`AssignToCpu` for GICv2 or report `B_NOT_SUPPORTED` instead of false success. X41 needs a
new `sdhci_fdt.cpp`; until it exists, SD boot on a Pi is impossible and the arm64 boot
module list is misleading. X38/X39/X40 sit in files other work currently owns and are
listed as follow-ups rather than actioned. **The part that genuinely needs a Pi 5:**
confirming its GIC-400 base addresses come from the device tree and are correct, that the
SD controller binds and the root filesystem mounts, and that the timer INTID read in stage
2 matches the hardware. None of that can be faked.

### Stage 4 — RISC-V
**Clears X4 (riscv64 half), and validates stage 0 generalised.** **Needs hardware we do
not have**, or at minimum a riscv64 build and a `qemu-system-riscv64` run, neither of
which was done here.

The first honest step is not a plan but a build: attempt `riscv64` after stage 0 and
publish the actual error list. Everything else is speculation until that list exists. What
is known statically is in §5.

### Stage 5 — Pi 3
**Clears X25, X42, X4 (arm64 half), X5, X6 (Pi 3 half), X22 (the non-conformant-INTID
half).** **Needs hardware we do not have**, and needs the most new code of any stage.

Pi 3 is last for reasons beyond X1. Its SoC-local interrupt controller matches nothing the
loader knows and the kernel's response to that is a NULL dereference (X25). Its per-core
timer interrupts are not at the Base System Architecture INTIDs the kernel hard-codes
(X22) — so stage 2's "read it from firmware" is a prerequisite, not an optimisation. Its
Ethernet is behind USB and there is no `dwc2` driver in the tree at all (X42), and no
MMIO-attached SD driver either (X41), so it has neither a storage path nor a network path
today. X4 and X5 must be fixed for the image to contain a network driver at all. This is a
port, not a fix list.

---

## 7. Proposed follow-up issues

Not filed from here; listed so they can be, with the file each would touch.

1. **`arm64: replace the unconditional -mcpu=neoverse-n1+crypto with a per-target ISA baseline`** —
   `build/jam/ArchitectureRules:52`. Measured: the current flag puts FEAT_LSE atomics in
   the boot loader and faults on any ARMv8.0 core.
2. **`sve_test: guard the aarch64 assembly so a non-arm64 build compiles`** —
   `src/bin/sve_test/*`, `src/bin/Jamfile:264`, `build/jam/packages/Haiku:256`. Currently a
   hard compile failure for riscv64 and x86.
3. **`efi/arm64: the FDT SMP path can call a NULL PSCI function pointer`** —
   `src/system/boot/platform/efi/arch/arm64/arch_smp.cpp:255-258`, `arch_dtb.cpp:78`.
4. **`efi/arm64: harden the FDT cpu/psci node parsing against missing properties`** —
   `arch_smp.cpp:236-247,270-272`, plus the unbounded MPIDR search at `:136-166`.
5. **`arm64: read the timer INTID and frequency from firmware instead of hard-coding`** —
   `headers/private/kernel/arch/arm64/arch_kernel_args.h` (new fields),
   `src/system/boot/platform/efi/arch/arm64/arch_dtb.cpp`,
   `src/system/kernel/arch/arm64/arch_timer.cpp:47-48,108,117`.
6. **`efi/arm64: fill ITS, redistributor regions, pe_count and pmu_gsiv from the device tree`** —
   `src/system/boot/platform/efi/arch/arm64/arch_dtb.cpp:63-76`.
7. **`efi: resolve clocks phandles in dtb_get_clock_frequency`** —
   `src/system/boot/platform/efi/dtb.cpp:548`.
8. **`arm64: NULL-check the interrupt controller in the three ICI senders`** —
   `src/system/kernel/arch/arm64/arch_smp.cpp:33-34,41,51-52`.
9. **`gicv2: drop the QEMU-virt fallback base addresses and program ITARGETSR`** —
   `src/system/kernel/arch/arm/arch_int_gicv2.cpp:29,37,91-102`.
10. **`gicv2: AssignToCpu reports success without moving the interrupt`** —
    `src/system/kernel/arch/arm/soc.h:25`.
11. **`efi: gate GPT auto-grow on a platform condition, not on __aarch64__, and skip
    read-only/removable media`** — `src/system/boot/platform/efi/devices.cpp:480-489`.
12. **`efi: replace the firmware-sized VLA in the handle enumeration`** —
    `src/system/boot/platform/efi/devices.cpp:442`.
13. **`pci/ecam: the FDT controller reads bus-range and discards it`** —
    `src/add-ons/kernel/busses/pci/ecam/ECAMPCIControllerFDT.cpp:27-32`, and the comment at
    `ECAMPCIController.h:136-140` that asserts an AWS measurement as a device-tree-path
    invariant. **Coordinate — these files are owned by other in-flight work.**
14. **`pci/ecam: FDT INTx routing is capped at buses 0-7`** —
    `ECAMPCIControllerFDT.cpp:113`. **Same coordination caveat.**
15. **`mmc: sdhci has no FDT attach path, so SD boot is impossible on an MMIO SD host`** —
    `src/add-ons/kernel/busses/mmc/`, and the boot module lists in
    `build/jam/packages/Haiku:353,368` that imply otherwise.
16. **`images: stop using the arm64 architecture token as a proxy for AWS EC2`** —
    `build/jam/images/definitions/common-tail:127-131`, `minimum:350-383,502,523-525`,
    `build/jam/packages/Haiku:149`.
17. **`build: remove the dead board/ directory and re-enable -Werror for arm`** —
    `build/jam/board/`, `build/jam/ArchitectureRules:638-640`.
18. **`riscv64: publish the actual build error list`** — no file; the deliverable is the
    output of an attempted build after follow-up 2 lands.
19. **`arm64: consolidate the three copies of the page-granule and VA-width constants`** —
    `src/system/kernel/arch/arm64/arch_int.cpp:142,325`,
    `arch_vm_translation_map.cpp:42,117-118`.
20. **`arm64: report the measured cache line size rather than a compile-time constant`** —
    `src/system/kernel/arch/arm64/arch_system_info.cpp:147`.

---

## 8. Method and limits

The classifications come from a read of the arm64 kernel, the EFI loader, the build
configuration, and the PCI/storage/network device paths, cross-checked against four QEMU
boots. Where a reading and a measurement disagreed, the measurement won: an earlier draft
of this audit recorded "arm64 has no device-tree PCI path", which run 2's
`finalize PCI controller from FDT` refutes outright, and it has been removed rather than
softened.

What this document does **not** establish: that any stage above takes a particular amount
of time; that a Pi of either generation boots (no Pi was booted); that riscv64 builds (it
was not attempted); or that fixing all 42 (X) items is sufficient for a second platform.
An audit finds what is wrong with what exists. Only hardware finds what is missing.

The (X) count is a floor, not a total. Four areas were audited; the rest of the tree —
app_server, the network stack above the driver, the file systems, userland — was not.
