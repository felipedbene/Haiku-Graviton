# Planned work — DeBeOS on ROCK 5 ITX (RK3588)

Derived from the verdicts in [`rock5itx-recon.md`](rock5itx-recon.md) §7.7 (source references are
to this repo's `graviton` branch at `09e47fb`, and to `edk2-porting/edk2-rk3588` at tag v1.1
`6a682c0e`). Ordered so that each phase produces a bootable milestone.

## Phase 1 — serial + SMP + PCIe-INTx on stock firmware (small, sized)

| # | Change | Where | Size |
|---|---|---|---|
| 1.1 | Recognize SPCR/DBG2 interface type `0x12` ("16550 with GAS parameters") → `UART_KIND_8250` | `src/system/boot/platform/efi/arch/arm64/arch_acpi.cpp:46-60` (`arch_acpi_uart_kind`) + enum in `headers/private/kernel/acpi.h:381-385` | ~2 lines |
| 1.2 | `_SEG`-aware MCFG matching: evaluate `_SEG` on each `PNP0A08` bridge and require `alloc->pci_segment` to match (today only bus ranges are compared; all five RK3588 allocations are bus 1–1, so every bridge resolves to segment 0 — the empty M.2 slot) | `src/add-ons/kernel/busses/pci/ecam/ECAMPCIControllerACPI.cpp:63-93` (`ReadResourceInfo`) | ~20 lines |
| 1.3 | MPIDR dedupe in CPU registration — firmware `ConfigTableMode=3` installs ACPI *and* DTB; `dtb_init()` runs after `acpi_init()` and `arm64_handle_fdt_cpu_node()` re-registers all 8 cores unguarded | `src/system/boot/platform/efi/arch/arm64/arch_smp.cpp:232-263` / `arch_smp_register_cpu()` | ~10 lines |
| 1.4 | (nice-to-have) 24 MHz DW-APB clock fallback instead of 1843200 when SPCR clock is 0 — latent only, since ACPI-discovered UARTs are never reprogrammed (`gUARTSkipInit`) | `arch_acpi.cpp:35-42` | 1 line |
| 1.5 | Enumeration hardening: tolerate the ECAM shim's phantom devfn-31 (header `0x7f`) reads on bus 1 | PCI enumerator | small |

**Milestone: DeBeOS loader + kernel on serial console, 8 CPUs up via PSCI/SMC, SATA + both NICs
visible on legacy INTx (per-segment GSIVs 292/287/272/277/282 from trivial `_PRT`s).**

## Phase 2 — MSI via ITS (the deep gap)

| # | Change | Where | Notes |
|---|---|---|---|
| 2.1 | **`dtb_init` ITS backfill:** in `arch_handle_fdt()`, add a matcher for `"arm,gic-v3-its"` *outside* the `if (interrupt_controller.kind[0] == 0)` guard at `arch_dtb.cpp:64`; guard on `kind[0] != 0 && regs3.start == 0`; fill via `dtb_get_reg(fdt, node, 0, regs3)` (`dtb.cpp:386-459`, handles `ranges`). Kernel side needs nothing — `arch_int_init_io()` already feeds `regs3` to `GICv3InterruptController::InitITS` | `src/system/boot/platform/efi/arch/arm64/arch_dtb.cpp` | The ITS is a *child node* of the GIC in the RK3588 DT (`msi-controller@fe640000/fe660000`), not reg index 2 — the existing per-node reg pattern can't reach it |
| 2.2 | **Dual-ITS `intc_info` extension:** the board has two ITSes with fixed segment routing (pcie3x* → its1 @`0xFE660000`, pcie2x1l* → its0 @`0xFE640000`, per DT `msi-map`). Single `regs3` can serve only one group. Extend `intc_info` with an ITS array mirroring `gicr_regions[]` | `headers/private/kernel/boot/interrupt_controller.h:41-67`, both loader paths, `arch_int.cpp:113-129` | Do 2.1 first with first-match; this is the follow-up |
| 2.3 | **ITS non-coherent / erratum RK3588001 path:** `GICv3ITS::Init` (`gicv3_its.cpp`) assumes coherent table walks. RK3588 needs: non-shareable/non-cacheable attributes in `GITS_BASER`/`GITS_CBASER`, cache-clean before every command-queue write, and the RK3588001 pre-ITS addressing workaround (what Linux's `its_enable_rk3588001` + non-coherent quirks do). Key off FDT `dma-noncoherent` or, on ACPI boots, the `RKCP`/`RK3588` OEM IDs | `src/system/kernel/arch/arm64/gicv3_its.cpp` | **Without this, a backfilled ITS trades "no MSI" for "corrupted MSI"** — hard blocker for 2.1/2.2 being useful |

## Phase 3 — candidate edk2-rk3588 firmware fixes (fork; upstream declared not_planned)

| # | Patch | Where (v1.1 tree) | Size |
|---|---|---|---|
| 3.1 | MADT ITS ×2: uncomment struct members + `EFI_ACPI_6_0_GIC_ITS_FRAME_INIT(0, 0xfe640000)/(1, 0xfe660000)` | `Silicon/Rockchip/RK3588/AcpiTables/Madt.aslc:29-30,199-202` | ~4 lines |
| 3.2 | Clear FADT "MSI Not Supported": `EFI_ACPI_6_3_MSI_NOT_SUPPORTED` → `EFI_ACPI_RESERVED_WORD` (revert of commit `4684b3ad`) | `.../Fadt.aslc:60` | 1 line |
| 3.3 | IORT: re-wire the existing-but-unwired `RK3588PcieIort.aslc` (2 ITS groups + 1 RC, removed from `.inf`s by `1ea0d782`) into `Platform/Radxa/ROCK5ITX/AcpiTables/AcpiTables.inf`; extend to 5 RC nodes with per-segment ITS routing | `.../RK3588PcieIort.aslc` + board `.inf` | 1 line to re-wire; ~90–120 to do properly |

Context: upstream issue #65 closed *not_planned* — "Not possible to have MSIs through ITS due to
Rockchip erratum 3588001"; ACPI is tested against Windows only. These patches are only useful
**paired with 2.3** (DeBeOS controls its own ITS driver; Linux would still be broken because its
quirk is DT-gated). Cost of the fork: reflashing 16 MiB SPI NOR per release, MaskROM recovery as
safety net.

## Alternative MSI path (erratum-free, larger)

- MADT **`GicMsiFrame`** (type 0x0D, GICv2m-style) pointing at the GICv3 **MBI frame
  `0xFE610000`** (SPIs 424–479, verified live) — the upstream maintainer's own suggestion
  ("definition already there but commented out, needs more testing"). Bypasses the ITS and its
  erratum entirely. DeBeOS has no type-0x0D consumer and no v2m/MBI driver — new-driver-sized
  effort, but no silicon quirk. Decide after 2.3 is scoped.

## NPU path (project B, from recon §5)

- Patch `rk3588-rock-5-itx.dtb`: flip `status = "okay"` on 3× `npu@` + 3× `iommu@` nodes and add
  `npu-supply`/`sram-supply = <&vdd_npu_s0>` (exactly as `rk3588-rock-5b.dts` does); load via GRUB
  `devicetree` (verified available). Expected result: `rocket.ko` binds → `/dev/accel/accel0`.
- Userspace: Mesa build with the Teflon frontend (`libteflon.so` is not shipped by the distro).
- Fall back to the vendor `rknpu` stack only when upstream `rocket` op coverage runs out.

## Outstanding discovery / unclear paths

1. **Does the patched-DTB NPU path actually produce `/dev/accel/accel0`?** Unknowns: power
   domains 9/10/11 coming up when firmware never touched them; the 200 MHz
   `assigned-clock-rates` being satisfiable through SCMI clocks. Needs a reboot to test.
2. **Full firmware-DTB vs distro-DTB diff** before replacing the firmware DTB wholesale — only
   ~18 of ~250 nodes were compared; ComboPHY/SATA/HDMI routing is firmware-configured and a
   mismatch could drop the boot disk.
3. **Does the V2M/MBI MSI path work on RK3588 silicon at all?** Upstream: "needs more testing";
   nobody has. Cheap to probe once a loader boots.
4. **`pcie@fe170000` (segment 2): "Device found, but not active"** — something detected, never
   links. Needs physical inspection of the M.2 E-key slot.
5. **HDMI0 disabled in *both* DTBs** — unfinished upstream board file or deliberate ITX routing?
   Determines whether one-display is a permanent constraint. (HDMI1 itself never verified lit —
   nothing was connected during recon.)
6. **PPTT leaf-flag bug** (cores misread as SMT threads) — worth an upstream edk2-rk3588 report
   independent of everything else; also decide whether DeBeOS should consume PPTT at all or keep
   deriving topology from MPIDR.
7. **Which ITS the ACPI path should prefer** when both exist, and whether DeBeOS needs IORT-style
   RC→ITS routing knowledge or can hardcode the RK3588 segment map.
8. **ACPI ECAM shim robustness** under config-space hammering — only enumeration-time behaviour
   observed.
9. **Native SATA (`sata@fe21x`) disabled in both DTBs** — confirm it's the ITX board design
   (ports wired to the ASM1164 instead) before writing any RK3588 AHCI support.
10. **TF-A/OP-TEE provenance strings** (`bl31-v1.45` vs `v2.12.0` in flash) — matters only if EL3
    policy ever needs rebuilding; unresolved from strings alone.
