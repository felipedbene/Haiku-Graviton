# ROCK 5 ITX (RK3588) — Firmware & Hardware Recon

**Target:** Radxa ROCK 5 ITX (accessed over SSH), Fedora 44, kernel `7.0.7-200.fc44.aarch64`
**Recon date:** 2026-08-31 (board uptime 14 min at time of capture)
**Firmware:** EDK II `v1.1`, build date 2025-04-09, from `edk2-rk3588` (target `ROCK5ITX`, `RELEASE_GCC`)
**Purpose:** anchor two projects — (A) porting the **DeBeOS/Haiku boot loader** to this board, (B) finding the **real NPU access path**.

Session was read-only with respect to configuration. Five diagnostic packages were installed with the
user's explicit go-ahead (`dtc`, `efibootmgr`, `i2c-tools`, `nvme-cli`, `dmidecode`). One module
(`rocket`) was loaded as a bind test and immediately unloaded. No reboot, no UEFI setting changed, no
config file edited.

**One premise correction up front:** this board does **not** boot from NVMe. Root is on a 240 GB
**SATA SSD behind the on-board ASMedia ASM1164** (PCIe→AHCI, `0001:11:00.0`). The M.2 M-key slot
(`pcie@fe150000`) reports *"Device not found"* — it is empty. There is no `/dev/nvme*`, no
`/sys/class/nvme`. Details in §4.

---

## 1. Firmware mode: ACPI or Device Tree?

### Raw findings

| Probe | Result |
|---|---|
| `/sys/firmware/acpi/tables` | **does not exist** |
| `/sys/firmware/efi/systab` | `ACPI20=0xeed8b018`, `SMBIOS3=0xefe60000`, `SMBIOS=0xefe80000` |
| `dmesg` EFI banner | `efi: EFI v2.7 by EDK II` … `MEMATTR=… ACPI 2.0=0xeed8b018 MOKvar=… INITRD=… RNG=0xeed8b918 MEMRESERVE=…` |
| `dmesg` ACPI | `ACPI: Interpreter disabled.` / `pnp: PnP ACPI: disabled` |
| `/sys/firmware/fdt` | present, **180312 bytes** |
| `/proc/device-tree/compatible` | `radxa,rock-5-itx` + `rockchip,rk3588` |
| `/proc/device-tree/model` | `Radxa ROCK 5 ITX` |
| `/sys/class/dmi/id/bios_version` | `v1.1` (vendor `EDK II`, date `04/09/2025`) |
| `fw_platform_size` | `64` |
| Secure Boot | `secureboot: Secure boot disabled` (shim + MOK var store present) |
| EFI var `ConfigTableMode` | **`0x00000003`** |
| EFI var `AcpiPcieEcamCompatMode` | `0x00000003` |
| EFI var `FdtCompatMode` | `0x00000002` |
| EFI var `FdtSupportOverrides` | `0x00` (**disabled**) |

### The answer: hybrid, with the OS choosing DT

The firmware is in **"both tables" mode**. `ConfigTableMode = 3` in `edk2-rk3588` is the
ACPI + Device Tree setting, and this is not an inference from the enum alone — the boot log proves
both were physically handed over: the EFI configuration table contains an **ACPI 2.0 RSDP at
`0xeed8b018`**, *and* the kernel simultaneously received a complete 180 KB **FDT** (`/sys/firmware/fdt`,
which then populated `/proc/device-tree` with ~250 top-level nodes).

Linux then picked DT, not because ACPI was missing but because of arm64's own policy: if the
firmware supplies a device tree with more than a bare `/chosen` node, `acpi_boot_table_init()` disables
ACPI. Hence `ACPI: Interpreter disabled.` with an RSDP sitting right there in memory.

Two details in the handed-over `/chosen` matter a lot for a boot loader port:

```
chosen {
        secure-boot-mode = <0x02>;
        linux,uefi-mmap-desc-ver = <0x01>;
        linux,uefi-mmap-desc-size = <0x30>;
        linux,uefi-mmap-size = <0x1b00>;
        linux,uefi-mmap-start = <0x00 0xed324040>;
        linux,uefi-system-table = <0x00 0xeffd0018>;
        bootargs = "BOOT_IMAGE=(hd0,msdos2)/vmlinuz-… root=/dev/mapper/systemVG-LVRoot ro";
        stdout-path = "serial2:1500000n8";
};
```

* There is **no `/memory` node anywhere in the DT.** RAM is described *only* by the UEFI memory map.
  A loader that parses `/memory` from the FDT will find zero RAM on this board.
* `stdout-path = serial2:1500000n8` → `/serial@feb50000` (UART2, `snps,dw-apb-uart`,
  reg-shift 2, reg-io-width 4, **1.5 Mbaud**). That is the debug console, and 1500000 is not a
  baud rate anyone guesses.

### Boot-stage chain (read out of the SPI NOR, not guessed)

The board boots from a 16 MiB SPI NOR (`mtd0`, `spi5.0`). Reading `/dev/mtd0ro` and pulling strings
gives the real chain:

| Stage | Evidence in flash |
|---|---|
| BootROM (MaskROM) | `%a: Recovery key pressed - entering MASKROM.` |
| Rockchip DDR init blob | `ddr-v1.18-9fa84341ce`, `DDR 9fa84341ce typ 24/09/06-09:51:11, fwver: v1.18` |
| Rockchip U-Boot **SPL** (loader only) | `U-Boot SPL 2017.09-g5f53abfa1e-221223 #zzz (Dec 26 2022)`, `Jumping to %s via ARM Trusted Firmware` |
| FIT payload | `FIT Image with ATF/OP-TEE/UEFI` |
| TF-A **BL31** | `ARM Trusted Firmware`, `bl31-v1.45`, `v2.3():v2.3-682-g4ca8a8422 … fwver: v1.45` (a second string `v2.12.0(release):d5c68fd` also appears) |
| OP-TEE **BL32** | `bl32-v1.17`, `OP-TEE`, `BL31: Initializing BL32` |
| EDK II (UEFI payload) | `edk2`, `*EDK2`, `…/edk2-rk3588/workspace/Build/ROCK5ITX/RELEASE_GCC/…/PeilessSec.dll` |
| shim → GRUB → Linux | `Boot0007* Fedora  HD(1,MBR,0xdaf1b6f2,…)/\EFI\fedora\shimaa64.efi` |

Runtime corroboration: `psci: PSCIv1.1 detected in firmware`, `SMC Calling Convention v1.5`,
`arm-scmi: Using scmi_smc_transport` / `SCMI Protocol v2.0 'rockchip:'`, and
`hw_random: smccc_trng` — i.e. EL3 is live and serving PSCI, SCMI (clocks protocol `0x14`,
resets `0x16`, SMC ID `0x82000010`) and TRNG. Note `EDK II` is a *peiless* build (SEC→DXE, no PEI).

> **Verdict — §1:** Hybrid firmware: EDK II hands over **both** an ACPI 2.0 RSDP and a full FDT, and the
> OS decides. So an ACPI-first ARM64 EFI loader has tables to consume here (contents unverified — see
> Open Questions), while an FDT-first loader works today with zero firmware changes. Either way the
> loader **must** take RAM from `EFI_GET_MEMORY_MAP`, not from DT, and CPU bring-up is PSCI 1.1 via
> SMC to TF-A BL31 — no board-specific secondary-core code needed.

---

## 2. CPU topology

`lscpu` / `/proc/cpuinfo`: 8 cores, one socket, no SMT.

| CPUs | MIDR part | Core | I-cache | cpufreq policy | DVFS range | Regulator |
|---|---|---|---|---|---|---|
| 0–3 | `0xd05` var `0x2` (r2p0) | Cortex-**A55** | VIPT | `policy0` (`related_cpus 0 1 2 3`) | 1008–1800 MHz | `vdd_cpu_lit_s0` |
| 4–5 | `0xd0b` var `0x4` (r4p0) | Cortex-**A76** | PIPT | `policy4` (`related_cpus 4 5`) | 1200–2400 MHz | `vdd_cpu_big0_s0` |
| 6–7 | `0xd0b` var `0x4` (r4p0) | Cortex-**A76** | PIPT | `policy6` (`related_cpus 6 7`) | 1200–2400 MHz | `vdd_cpu_big1_s0` |

4×A76 + 4×A55 confirmed. The non-obvious part: **the big cores are two separate 2-core clusters, not
one 4-core cluster.** `topology/cluster_id` reports 0/1/2 and there are **three** cpufreq policies —
`bigcore0` (4–5) and `bigcore1` (6–7) each have their own OPP table (`opp-table-cluster1`,
`opp-table-cluster2`), their own regulator, and their own thermal zone (`bigcore0-thermal`,
`bigcore2-thermal`). Driver is generic `cpufreq-dt` + `schedutil`; available steps
1008/1200/1416/1608/1800 (little) and 1200/1416/1608/1800/2016/2208/2400 (big).

Caches: L1 32K/32K per core, L2 128K per core (2.5 MiB total across 8), **L3 3 MiB shared, 12-way**
(single `l3-cache` DT node). `cpu-map` has cluster0 {core0..3}, cluster1 {core0,1}, cluster2 {core0,1}.

Feature set (identical string on both core types, so the kernel is exposing the intersection):
`fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp`.
Notably **no SVE, no LSE2-beyond-`atomics`, no `bf16`/`i8mm`** — relevant to §5, since NEON dotprod
(`asimddp`) is the best CPU fallback for quantised inference. `CPU features: detected: Virtualization
Host Extensions` (VHE, EL2 available), `32-bit EL0 Support`, KPTI forced on by KASLR.
Also detected: `ARM errata 1165522, 1319367, 1530923`, `ARM erratum 1286807/2441009`, `Spectre-v4`,
`Spectre-BHB`, `SSBS not fully self-synchronizing`. MPIDRs are `0x0`, `0x100`…`0x700`.

`dmidecode`: 24 GB **LPDDR4**, 1/1 slots populated, `Memory: 24099772K available`, chip form factor
(soldered). SMBIOS type 4 misreports the SoC as a 4-core 816 MHz part — cosmetic, but don't build
anything on SMBIOS CPU data here.

> **Verdict — §2:** For the DeBeOS port: bring cores up by MPIDR via PSCI, and treat the SoC as
> **three** frequency/voltage domains (A55×4, A76×2, A76×2), not the usual big.LITTLE pair — a
> scheduler or DVFS layer that assumes one "big cluster" will mis-model half the big cores. The
> A55 booting as CPU0 also means your first-instruction path runs on the *slow* core, and VIPT vs
> PIPT I-caches across clusters means cache maintenance must be correct before the first migration.

---

## 3. Interrupt controller

* `arm,gic-v3`, distributor `0xfe600000` (0x10000), redistributors `0xfe680000` (0x100000).
* `GICv3: 480 SPIs implemented`, `0 Extended SPIs`, `16 PPIs`, split EOI/Deactivate mode.
* `GICD_CTLR.DS=0, SCR_EL3.FIQ=1` → GIC is running in **two-security-state mode** with FIQs routed to EL3 (consistent with TF-A + OP-TEE from §1).
* **Two ITSes**: `ITS [mem 0xfe640000-0xfe65ffff]` and `ITS [mem 0xfe660000-0xfe67ffff]`, 8192 device
  table entries + 32768 collections each; LPI property table at `0x100800000`.
* **GICv3 MBI also enabled**: `MBI range [424:479]`, `Using MBI frame 0x00000000fe610000`.
* **PPI partitions** (because A55 and A76 PMUs differ):
  `interrupt-partition-0 { cpu@0..cpu@300 }`, `interrupt-partition-1 { cpu@400..cpu@700 }` — matching the `pmu-a55` / `pmu-a76` DT nodes.
* Errata/workarounds the kernel turns on here: `GIC: enabling workaround for GICv3: non-coherent
  attribute`, `ITS: Rockchip erratum RK3588001`, `ITS: non-coherent attribute`, `ITS: using cache
  flushing for cmd queue`, `GIC: using cache flushing for LPI property table`.

`/proc/interrupts` sample: `13 … GICv3 26 Level arch_timer`, `15 … GICv3 321 Level rk_timer`,
`21 … GICv3 365 Level ttyS2`, `37 … GICv3 23 Level arm-pmu, arm-pmu`, plus `arm-smmu-v3-evtq/priq/gerror`
at SPIs 401/406/403 and one non-GIC chip: `rockchip_gpio_irq 8 Level hym8563`.

> **Verdict — §3:** Standard GICv3 — a Haiku arm64 loader/kernel needs no exotic irqchip, but three
> things are mandatory and easy to miss: (1) the **`RK3588001` ITS erratum** and the GIC/ITS
> **non-coherent** quirk (tables must be cache-flushed, not assumed coherent) or MSIs corrupt
> silently; (2) **two ITS instances** must both be initialised, since PCIe MSI-X targets are split
> across them; (3) PMU PPIs are **partitioned per cluster** — a single global PPI registration is wrong
> on this SoC.

---

## 4. Peripheral enumeration

### PCIe (5 controllers, one segment each)

| DT node | ECAM base | Segment | Link | Device |
|---|---|---|---|---|
| `pcie@fe150000` | `a40000000` | `0000:00` | *Device not found* (LnkSta 2.5GT/s x1 on the RP) | **empty — this is the M.2 M-key slot** |
| `pcie@fe160000` | `a40400000` | `0001:10` | **Gen3 x2 link up** | ASMedia **ASM1164** AHCI `[1b21:1164]` → `sda` |
| `pcie@fe170000` | `a40800000` | `0002:20` | *Device found, but not active* | slot populated-ish/no link (M.2 E-key / WiFi?) |
| `pcie@fe180000` | `a40c00000` | `0003:30` | Gen2 x1 link up | Realtek **RTL8125** `[10ec:8125]` → `enP3p49s0` |
| `pcie@fe190000` | `a41000000` | `0004:40` | Gen2 x1 link up | Realtek **RTL8125** → `enP4p65s0` |

All five root ports use MSI (`Count=16/32`) and are in distinct IOMMU groups (6–10) behind
`arm-smmu-v3 fc900000.iommu` (`oas 48-bit`, features `0x001c1ebf`; note
`msi_domain absent - falling back to wired irqs`).

### Storage

```
sda    223.6G  sata  SATA SSD   (ATA behind ASM1164)
 ├─sda1 500M  FAT16  Id 06     /boot/efi     <-- ESP
 ├─sda2   2G  Id ea             /boot
 └─sda3 221G  LVM   systemVG-LVRoot  /
mmcblk0 7.3G  mmc   Samsung 8GTF4R eMMC (HS400 Enhanced strobe) + boot0/boot1 4M + rpmb
```

* Disk label type is **`dos` (MBR)**, not GPT, and the ESP is partition type **`0x06` FAT16**, not
  `0xEF`. EDK II boots it anyway via the explicit `Boot0007` load option.
* The ESP is a **recycled Raspberry Pi boot partition** — it still contains `bootcode.bin`,
  `config.txt`, `start*.elf`, `bcm27xx-*.dtb`, `rpi-u-boot.bin` and an `overlays/` tree of RPi
  `.dtbo` files, alongside `EFI/fedora/{shimaa64,grubaa64,mmaa64}.efi` and `EFI/BOOT/BOOTAA64.EFI`.
  Harmless, but expect to trip over it when you start writing loaders to this ESP.
* eMMC is present and holds a separate (older) boot path — `efibootmgr` lists
  `Boot0003/0004/0005` for eMMC User Data / Boot1 / Boot2. Native SATA (`sata@fe210000/20/30`) is
  **disabled** in DT; all SATA goes through the ASM1164. `nvme list` → `Failed to scan topology` (no NVMe).
* `BootOrder: 0007,0000,0001,0002,0003,0004,0005,0008…000F,0006`; `BootCurrent: 0007`; `Timeout: 5s`;
  `BootDiscoveryPolicy=2`. `Boot0002` is *"Reset to MaskROM"*, `Boot0001` is the UEFI Shell — both
  are useful during loader bring-up.

### Networking

Both NICs are **PCIe Realtek RTL8125B**, driver **`r8169`**, firmware `rtl8125b-2_0.0.2 07/13/20`:
`enP3p49s0` (`00:e0:4c:xx:xx:xx`, link down) and `enP4p65s0` (`00:e0:4c:xx:xx:xx`, up).
There is **no GMAC/stmmac in play at all** — `ethernet@fe1b0000` and `ethernet@fe1c0000` (the RK3588
`dwmac`) are both `status = "disabled"` in the running DT *and* in Fedora's DTB. (Additional virtual
interfaces from an unrelated container workload on the board are omitted here.)

### USB

`ehci-platform` ×2 (`fc800000`, `fc880000`), `ohci-platform` ×2 (`fc840000`, `fc8c0000`),
`xhci-hcd.5.auto` at `0xfc400000` (`hci version 0x110`, quirks `0x808002000010`) giving bus 5 (480M)
and bus 6 (5000M). Hubs: Terminus `1a40:0101`, Genesys `05e3:0610` + **GL3523** `05e3:0620`.
`usb@fcd00000` (the second DWC3) is `disabled` in this DT. Three `rockchip_usb2phy` IRQs are live.
USB-C: `fusb302` at `8-0022` (`typec_fusb302`).

### I²C / PMIC / RTC

`i2cdetect -l` → `i2c-0/1/6/7/8` all `rk3x-i2c`, plus `i2c-9` = `DesignWare HDMI QP` (the HDMI DDC bus).

| Client | Chip | Driver | DT node |
|---|---|---|---|
| `0-0042` | rk8602 | `fan53555-regulator` | `/i2c@fd880000/regulator@42` |
| `0-0043` | rk8603 | `fan53555-regulator` | `/i2c@fd880000/regulator@43` |
| `1-0042` | rk8602 | `fan53555-regulator` | `/i2c@fea90000/regulator@42` |
| `6-0051` | hym8563 | `rtc-hym8563` | `/i2c@fec80000/rtc@51` → `rtc0` |
| `7-0011` | es8316 | `es8316` | `/i2c@fec90000/audio-codec@11` |
| `8-0022` | fusb302 | `typec_fusb302` | `/i2c@feca0000/usb-typec@22` |

**The main PMIC is not on I²C** — it is `rk806` on **SPI2**: `spi2.0 modalias=spi:rk806`,
`/spi@feb20000/pmic@0` (that is also where `rk805-pwrkey` comes from). SPI5 (`spi@fe2b0000`) holds the
`spi-nor` boot flash. 36 regulator rails are exposed, including `vdd_cpu_lit_s0`, `vdd_cpu_big0_s0`,
`vdd_cpu_big1_s0`, `vdd_gpu_s0`, **`vdd_npu_s0`**, `vdd_log_s0`, `vdd_vdenc_s0`.

### Other dmesg items requested

* `dwmmc`: `dwmmc_rockchip fe2c0000.mmc` and `fe2d0000.mmc` — *"Version ID is 270a"*, IDMAC 32-bit
  address mode, 256-deep FIFO, irq 114/115; `fe2d0000` has an `mmc-pwrseq` (SDIO). eMMC is the
  separate `sdhci-dwcmshc fe2e0000.mmc` (HS400 ES; *"Can't reduce the clock below 52MHz in
  HS200/HS400 mode"*).
* `dwc3`: only the one at `0xfc400000` probes (as `xhci-hcd.5.auto`); `usb@fcd00000` disabled.
* `rockchip-iommu`: six instances bind — video codecs `fdb50800/fdba0800/fdba4800/fdba8800/fdbac800`
  (IOMMU groups 0–4) and `fdd97e00` for the VOP (group 5). **No NPU IOMMU appears** (see §5).
* `clk-rockchip`: nothing logged; clocks come from `clock-controller@fd7c0000` (CRU) plus
  SCMI-over-SMC for a subset (`arm-scmi … Enabling SCMI Quirk [quirk_clock_rates_triplet_out_of_spec]`).
* Thermal zones: `package`, `bigcore0`, `bigcore2`, `littlecore`, `center`, `gpu`, **`npu`** — all ~32–33 °C idle, via `tsadc@fec00000`; `pwm-fan` present, EFI `CoolingFanState=1`, `CoolingFanSpeed=0x32`.
* HW RNG: `rng_current = smccc_trng` (TF-A), with `rng@fe378000` also in DT and an EFI RNG protocol at `0xeed8b918`.

> **Verdict — §4:** Almost nothing on the I/O path is a Rockchip-specific block: DeBeOS needs
> **generic PCIe (5 segments!) + AHCI + r8169 + xHCI/EHCI/OHCI + SMMUv3** and gets storage,
> networking and USB. That is a much cheaper port than the usual ARM SBC, but note (a) five
> independent PCIe segments with per-segment ECAM must all be enumerated, (b) the boot disk is
> **MBR + FAT16 type-0x06**, so a loader that requires GPT/0xEF will not find this ESP, and (c) the
> genuinely board-specific pieces you cannot avoid are the **rk806 PMIC on SPI2**, `hym8563` RTC on
> I²C6, and the fact that `vdd_npu_s0` — the rail project (B) needs — lives behind that SPI PMIC.

---

## 5. NPU stack — the big one

### Raw findings

| Probe | Result |
|---|---|
| `lsmod \| grep -i rocket` | **empty** (not loaded) |
| `ls /dev/accel/` | `No such file or directory` |
| `find /proc/device-tree -iname '*npu*' -o -iname '*rknn*'` | **no matches** |
| `uname -r` | `7.0.7-200.fc44.aarch64` |
| `/proc/config.gz` | absent → used `/boot/config-7.0.7-200.fc44.aarch64` |
| `CONFIG_DRM_ACCEL_ROCKET` | **`=m`** (also `DRM_ACCEL=y`, `DRM_ACCEL_ARM_ETHOSU=m`, `DRM_ACCEL_QAIC=m`) |
| `modinfo rocket` | `/lib/modules/…/kernel/accel/rocket/rocket.ko.xz`, author Tomeu Vizoso, *"DRM driver for the Rockchip NPU IP"*, `intree: Y`, Fedora-signed, `alias: of:N*T*Crockchip,rk3588-rknn-core` |
| `dmesg \| grep -iE 'rknn\|rocket\|npu'` | **zero driver lines** (only `npu-thermal`, `vdd_npu_s0`, `npu-leakage@28`, `npu-pins`, `qos_npu*`) |

### The answer: present, loadable, and blocked by the device tree — in two places

**Empirical bind test** (loaded, then unloaded): `sudo modprobe rocket` returns 0, `rocket` appears in
`lsmod` and pulls in `gpu_sched` — and **`/dev/accel` still does not exist**. The driver is fine; there
is no device for it to bind to.

The reason is a two-layer DT gap, and both layers matter:

**Layer 1 — the DT actually in use has no NPU at all.**
The running DT is *not* Fedora's; it is the DTB embedded in the EDK II firmware (`/sys/firmware/fdt`,
180312 bytes; the flash even contains `fdtfile=rockchip/rk3588-rock-5-itx.dtb`). Fedora's own
`/boot/dtb-7.0.7-200.fc44.aarch64/rockchip/rk3588-rock-5-itx.dtb` is 105222 bytes — a different file.
Decompiling both:

```
firmware FDT:   npu@…            -> ABSENT   (no npu@fdab0000/fdac0000/fdad0000, no iommu@fdab9000/fdaca000/fdada000)
Fedora DTB:     npu@fdab0000     -> present, compatible = "rockchip,rk3588-rknn-core", status = "disabled"
                npu@fdac0000     -> present, status = "disabled"
                npu@fdad0000     -> present, status = "disabled"
                iommu@fdab9000/fdaca000/fdada000 -> present, status = "disabled"
```

What the firmware DT *does* keep is every NPU satellite: `thermal-zones/npu-thermal`,
`efuse@fecc0000/npu-leakage@28`, `pinctrl/npu/npu-pins`, `qos_npu0_mro/mwr`, `qos_npu1`, `qos_npu2`,
`vdd_npu_s0`. So the power/thermal plumbing is described and the three compute cores are simply not
declared.

**Layer 2 — even mainline's ROCK 5 ITX DT leaves the NPU disabled.**
This is the part that will bite later. Across Fedora's 7.0.7 rk3588 DTBs, **24 boards enable
`npu@fdab0000`** — including `rk3588-rock-5b`, `rock-5b-plus`, `rock-5t`, `orangepi-5-plus`,
`nanopc-t6`, `jaguar`, `tiger-haikou`, `turing-rk1`, `cm3588-nas`. The **ROCK 5 ITX is not among
them** (nor is `rock-5a`). Reference, from `rk3588-rock-5b.dtb`:

```dts
npu@fdab0000 {
    compatible = "rockchip,rk3588-rknn-core";
    reg = <0 0xfdab0000 0 0x1000>, <0 0xfdab1000 0 0x1000>, <0 0xfdab3000 0 0x1000>;
    reg-names = "pc", "cna", "core";
    interrupts = <0 0x6e 4 0>;
    clock-names = "aclk", "hclk", "npu", "pclk";
    assigned-clock-rates = <0xbebc200>;      /* 200 MHz */
    reset-names = "srst_a", "srst_h";
    power-domains = <&power 9>;
    iommus = <&npu_mmu_0>;
    status = "okay";
    npu-supply  = <&vdd_npu_s0>;
    sram-supply = <&vdd_npu_s0>;
};
```
The ITX DTB has an identical node minus `status = "okay"` and minus the `npu-supply`/`sram-supply`
phandles — and it *does* already define the `vdd_npu_s0` regulator. So the upstream delta is
small and mechanical: flip `status` on 3 `npu@` + 3 `iommu@` nodes and wire `npu-supply`/`sram-supply`
to `vdd_npu_s0`, exactly as `rock-5b.dts` does.

**Userspace status:** `mesa-dri-drivers-26.0.6-2.fc44` is installed but there is **no
`libteflon.so`** anywhere on the filesystem and no `librknn*`/`rknpu`/`tflite` packages. So even after
the kernel side lights up, the Teflon delegate has to come from a Mesa build with the
`teflon` frontend enabled (Fedora's stock Mesa package doesn't ship it here).

**Enabling mechanisms available, best first:**

1. **GRUB `devicetree`** — verified available, not speculation: `grubaa64.efi` contains the
   `devicetree` command and `Load DTB file.` help string, and `/usr/lib/grub/arm64-efi/fdt.mod` exists.
   Adding `devicetree /dtb-$kver/rockchip/rk3588-rock-5-itx.dtb` (with a locally patched DTB) overrides
   the firmware FDT.
2. **Firmware FDT override** — the EDK II build exposes `FdtSupportOverrides` (currently `0x00`,
   disabled) plus `FdtOverrideBasePath` / `FdtOverrideOverlayPath` / `FdtOverrideFixup` EFI vars, i.e.
   the firmware can load a DTB/overlay from the ESP. Requires a UEFI setup change + reboot.
3. **Runtime overlay** — `CONFIG_OF_OVERLAY=y` and `CONFIG_OF_DYNAMIC=y`, and the firmware DTB
   *includes a `__symbols__` node* (Fedora's does not), so it is overlay-ready. But
   `/sys/kernel/config` has no `device-tree` directory in this kernel, so there is no userspace entry
   point without a helper module. Not a path today.

> **Verdict — §5:** **Present but unbindable, at two layers.** `rocket.ko` ships in Fedora's kernel
> (`CONFIG_DRM_ACCEL_ROCKET=m`, in-tree, signed) and loads cleanly, but the EDK II-supplied DTB
> declares **no NPU cores at all**, and the mainline ROCK 5 ITX DTB that *does* declare them keeps all
> three `status = "disabled"` — unlike 24 other rk3588 boards. So the fork point is **not**
> "vendor kernel vs mainline": you are one patched DTB + a GRUB `devicetree` line away from
> `/dev/accel/accel0` on the stock Fedora kernel, which unlocks the basic-Teflon-ops path (and even
> then you must supply your own Mesa/Teflon build, since no `libteflon.so` is installed). Reach for
> the vendor `rknpu` stack only when you need what upstream `rocket` still doesn't expose — full
> matmul-class op coverage and multi-core scheduling across all three cores.

---

## 6. Display

* `cat /sys/class/drm/*/status` → **exactly one connector**: `card0-HDMI-A-1: disconnected`.
* Two DRM devices: `card0` = `rockchip-drm` (`display-subsystem`), `card1` + `renderD128` = **`panthor`**.
* VOP2: `rockchip-drm display-subsystem: bound fdd90000.vop (ops vop2_component_ops)`, node
  `compatible = "rockchip,rk3588-vop"`, reg `0xfdd90000` + `0xfdd95000`, 4 video ports (`port@0..3`),
  9 clocks (`dclk_vp0..3`, `pll_hdmiphy0/1`), IOMMU group 5 via `fdd97e00.iommu`.
* HDMI: only **`hdmi@fdea0000` (HDMI1)** is `okay` — `dwhdmiqp-rockchip fdea0000.hdmi: registered
  DesignWare HDMI QP I2C bus driver`, bound via `dw_hdmi_qp_rockchip_ops`, with a CEC/`rc0` input
  device. **`hdmi@fde80000` (HDMI0) is `disabled`** in the running DT *and* in Fedora's ITX DTB.
  `hdmi_receiver@fdee0000` (HDMI-in) is disabled too, and its 160 MB `hdmi-receiver-cma` reservation is
  `status = "disabled"`.
* **No `dp@`, `edp@` or `dsi@` nodes exist anywhere in this DT** — even though `dw_dp`,
  `dw_mipi_dsi`, `dw_mipi_dsi2`, `analogix_dp` and `inno_hdmi` modules are all loaded as
  `rockchipdrm` dependencies. DP-alt-mode over USB-C is therefore not described to the OS (the
  `fusb302` and USB-DP PHYs are there; `UsbDpPhy0Usb3State`/`UsbDpPhy1Usb3State` EFI vars exist).
* Harmless-but-telling log lines: `Fixed dependency cycle(s)` between `/vop@fdd90000`,
  `/hdmi@fdea0000` and `/hdmi1-con`; then `rockchip-drm display-subsystem: [drm] Cannot find any crtc
  or sizes` (×2) — expected with nothing plugged in.
* **EFI GOP is real and was consumed.** At `0.32 s`, long before `rockchipdrm` probes at `22.6 s`:
  `simple-framebuffer simple-framebuffer.0: [drm] Registered 1 planes with drm panic` /
  `Initialized simpledrm 1.0.0 … on minor 0` / `fb0: simpledrmdrmfb frame buffer device`. There is no
  `simple-framebuffer` DT node — this came from **sysfb off the UEFI GOP**, and `rockchip-drm` later
  took over minor 0 (`/sys/class/graphics` now holds only `fbcon`). Related EFI vars:
  `DisplayModePreset=0x80000000` (auto), `DisplayForceOutput=0x01`, `DisplayDuplicateOutput=0x00`,
  `DisplayConnectorsPriority`, `DisplayRotation`, `HdmiSignalingMode=0x00`, `FdtForceGop=0x00`.
* GPU, for completeness: `panthor fb000000.gpu` → `Mali-G610 id 0xa867`, `shader_present=0x50005`,
  CSF FW interface v1.5.0, `[drm] Initialized panthor 1.7.0 … on minor 1`, clock 198 MHz at probe.

> **Verdict — §6:** DeBeOS gets a **working linear framebuffer for free from the UEFI GOP** — Linux
> proved it by running on `simpledrm` for the first 22 seconds of boot — so bring-up needs no VOP2,
> no HDMI PHY and no `panthor` code. Plan for exactly **one usable output (HDMI1)**: HDMI0, HDMI-in,
> DP/eDP/DSI are all absent-or-disabled in this DT, so multi-head or USB-C DP is a later,
> DT-and-driver-sized project, not a loader detail. `FdtForceGop` and `DisplayForceOutput` are the
> firmware knobs to force a framebuffer when no monitor is attached.

---

## 7. ACPI table deep dive (acpi=force boot)

**Sources:** `acpidump` from the board booted with `acpi=force` (edk2-rk3588 v1.1 firmware), decompiled offline with ACPICA 20260408 (`acpixtract -a` + `iasl -d`); `journalctl -p err` from that boot; a second live look at the same ACPI-mode boot; and two source-level cross-checks — the `felipedbene/Haiku-Graviton` loader (branch `graviton`, commit `09e47fb`) and upstream `edk2-porting/edk2-rk3588` at the v1.1 tag (`6a682c0e`, matching this firmware's 2025-04-09 build). This section resolves Open Questions **1, 2 and 3**.

The firmware installs exactly nine tables — `XSDT → FACP, DSDT, DBG2, GTDT, APIC, MCFG, PPTT, SPCR, BGRT` (all OEM `RKCP`/`RK3588  `, compiler `EDK2`). No SSDT, **no IORT**, no FACS, no SRAT/SLIT, no SPMI/TPM. In the source, these are *static C structs* (`.aslc`) under `edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/`, pulled in by `Platform/Radxa/ROCK5ITX/AcpiTables/AcpiTables.inf`; only checksums and a few DSDT integers are patched at install time by `AcpiPlatformDxe`.

### 7.1 MADT (`APIC`, 724 bytes, revision 4)

Subtable census: **8 × GICC (type 0x0B) + 1 × GICD (type 0x0C) + 1 × GICR (type 0x0E) + 0 × ITS (type 0x0F)**. Zero ITS entries confirmed both by the census and by the live kernel: `ITS: No ITS available, not enabling LPIs`.

All eight GICC entries, decoded:

| UID | MPIDR | Flags | PMU intr | vGIC maint intr | GICC/GICV/GICH base | Redist base (per-GICC) | Efficiency class |
|---|---|---|---|---|---|---|---|
| 0 | `0x000` | `0x1` (Enabled) | 23 (0x17) | 25 (0x19) | 0 / 0 / 0 | 0 | 0 |
| 1 | `0x100` | `0x1` | 23 | 25 | 0 | 0 | 0 |
| 2 | `0x200` | `0x1` | 23 | 25 | 0 | 0 | 0 |
| 3 | `0x300` | `0x1` | 23 | 25 | 0 | 0 | 0 |
| 4 | `0x400` | `0x1` | 23 | 25 | 0 | 0 | 1 |
| 5 | `0x500` | `0x1` | 23 | 25 | 0 | 0 | 1 |
| 6 | `0x600` | `0x1` | 23 | 25 | 0 | 0 | 1 |
| 7 | `0x700` | `0x1` | 23 | 25 | 0 | 0 | 1 |

* All 8 CPUs present, all `Processor Enabled : 1`, MPIDRs matching the DT/PSCI values from §2. PMU = PPI 23 and vGIC maintenance = PPI 25 on every entry (uniform, despite the A55/A76 PMU split that DT models with PPI partitions — fine for ACPI, which has no partition concept). Parking Protocol Version 0 + parked address 0 = PSCI boot.
* Efficiency class 0 (A55, UIDs 0–3) vs 1 (A76, UIDs 4–7) — **correct** per spec (lower = more efficient).
* Entries are the 80-byte (`Length : 50`) layout; SPE overflow field is 0. The `TRBE Interrupt : 500B` lines in the disassembly are an ACPICA artifact — it prints two bytes *past* the 0x50-byte entry, which are the next subtable's type+length (`0B 50`). Not a real field.
* **Per-GICC `Redistributor Base Address` is 0 on every entry** — redistributors are described solely by the one GICR subtable: base `0xFE680000`, length `0x100000` (matches DT `reg` and the live per-CPU probe: `CPU0..7: found redistributor n00 region 0:0xfe680000 + n*0x20000`).
* GICD: base `0xFE600000`, GIC version field = **3**, interrupt base 0.
* In the firmware source the ITS entries exist but are **commented out**, with the exact bases already written (`Madt.aslc:195-202`): `// EFI_ACPI_6_0_GIC_ITS_FRAME_INIT(0, 0xfe640000) / (1, 0xfe660000)`, under the comment *"ITS is broken on RK35xx! Linux has a workaround for the erratum, but no way to describe this in ACPI yet."* A `GicMsiFrame` (GICv2m-style, type 0x0D) member is commented out alongside it.

> **Verdict — 7.1:** The MADT is complete and correct for GICv3 + PSCI SMP bring-up — 8 enabled GICCs with true MPIDRs, GICD v3, one GICR region — but with **zero ITS entries** the OS is told LPIs/MSIs don't exist; that omission is deliberate upstream (erratum RK3588001), and the fix is literally four commented-out lines in `Madt.aslc`.

### 7.2 FADT (`FACP`, 276 bytes, revision 6.3)

* `Flags : 0x00100021` → **`Hardware Reduced (V5) : 1`** (plus WBINVD, Control Method Sleep Button). No SCI (`SCI Interrupt : 0000`), no PM blocks, no FACS — a pure hardware-reduced ACPI platform, as expected for arm64.
* **`ARM Flags (decoded below) : 0001` → `PSCI Compliant : 1`, `Must use HVC for PSCI : 0`** — PSCI via **SMC**, matching the DT `psci { method = "smc" }` and TF-A BL31 from §1.
* The flag behind Linux's message is at offset 0x6D, the field ACPICA prints as **`Boot Flags`** (the `IAPC_BOOT_ARCH` word):

  ```
  [06Dh 0109 002h]  Boot Flags (decoded below) : 0008
                          MSI Not Supported (V4) : 1
  ```

  Bit 3 = `EFI_ACPI_6_3_MSI_NOT_SUPPORTED`, set in one line of source (`Fadt.aslc:60`). Live kernel, 0.031 s: `ACPI FADT declares the system doesn't support MSI, so disable it`. The flag was added by edk2-rk3588 commit `4684b3ad` (2024-02-11) with the stated rationale *"This stops Linux from attempting to enable MSI and ultimately failing"* — i.e. it papers over the missing ITS rather than being an independent statement about the hardware.
* Knock-on effect measured live: with MSI globally off, Linux's `_OSC` negotiation refuses control on every root bridge — `acpi PNP0A08:0x: _OSC: not requesting OS control; OS requires [ExtendedConfig ASPM ClockPM MSI]` ×5 — and every PCIe device (`ahci`, both `r8169`) runs `MSI: Enable-` on wired SPIs (`ahci[0001:01:00.0]` on GIC SPI 287, NICs on 277/282), single-CPU-steered.

> **Verdict — 7.2:** `ARM_BOOT_ARCH = PSCI_COMPLIANT, SMC` — exactly what a loader wants; but the `MSI Not Supported` boot-arch bit (one source line, a deliberate Linux workaround) forces the entire PCIe subsystem to legacy interrupts on any spec-conforming OS.

### 7.3 SPCR — and what Haiku-Graviton would do with it

Decoded table (revision **2**, 0x50 bytes):

```
Interface Type            : 12                       <-- DBG2 subtype 0x12: "16550 with parameters defined in GAS"
Serial Port Register (GAS): SystemMemory, BitWidth 0x20, AccessWidth 03 [DWord], Address 0xFEB50000
Interrupt Type            : 08 (GIC)   Interrupt : 0x16D (= GSIV 365, SPI 333)
Baud Rate                 : 00         <-- "as-is / keep firmware-configured rate"
Parity 0, Stop Bits 1, Flow Control 0, Terminal Type 2 (VT-UTF8)
Uart Clock Freq           : (absent)
```

The trailing ACPICA warning — *"table terminates in the middle of a data structure"* — is it attempting to read the revision-3+ `UartClockFrequency`/`PreciseBaudRate` fields past the end of an exactly-80-byte rev-2 table. **The SPCR carries no clock at all**, and the baud byte is 0 ("use current settings"). The real parameters live in the DSDT companion device `UAR2` (`HISI0031` + `_DSD`: `reg-shift = 2`, `reg-io-width = 4`, `clock-frequency = 0x016E3600` = 24,000,000). Linux stitched both together: `SPCR: console: uart,mmio32,0xfeb50000` early, then `HISI0031:00: ttyS0 at MMIO 0xfeb50000 (irq = 26, base_baud = 1500000) is a 16550A` (1500000 = 24 MHz ÷ 16). Same UART2 as §1, still at 1.5 Mbaud.

**Against `src/system/boot/platform/efi/arch/arm64/arch_acpi.cpp` (Haiku-Graviton, verified in the source):**

| SPCR feature on this board | Haiku loader behaviour | Status |
|---|---|---|
| Interface type **0x12** | `arch_acpi_uart_kind()` (`arch_acpi.cpp:46-60`) knows only `0, 1, 3, 0x0d, 0x0e` (`acpi.h:381-385`) → returns NULL → `uart.kind` stays empty → no `gUART`, no console. DBG2 fallback dead too: same subtype 0x12. | **MISMATCH — the one real blocker** |
| GAS DWord access / 32-bit width (→ reg-shift 2) | `arch_acpi_uart_reg_shift()` (`arch_acpi.cpp:67-86`): `access_size == ACPI_GAS_ACCESS_SIZE_DWORD \|\| bit_width == 32` → `return 2`; honoured by `DebugUART8250::Out8` (`debug_uart_8250.cpp:110-129`: `Base() + (reg << fRegShift)`) | handled |
| Baud byte 0 ("as-is") | `spcr->baud` is never read anywhere; `arch_acpi_setup_uart()` sets `gUARTSkipInit = true` (`arch_acpi.cpp:96`) and `serial_enable()` skips `InitPort` (`serial.cpp:109-110`). Firmware's 1.5 Mbaud is preserved. | handled (by design: ACPI-discovered UARTs are never reprogrammed) |
| Clock field absent/0 | Driver fallback: `uart.clock != 0 ? uart.clock : 1843200` (`arch_acpi.cpp:35-42`). 1.8432 MHz is *wrong* for this 24 MHz DW-APB — but harmless today, since nothing ever calls `InitPort` on this path. Would only bite if someone later adds baud reprogramming. | latent, currently harmless |

One-line fix: map `0x12` → `UART_KIND_8250` in the switch; everything downstream is already correct. (In hybrid mode the FDT path saves the day regardless — `uart.kind[0] == 0` lets `dtb_handle_fdt()` claim `serial@feb50000`, and `"snps,dw-apb-uart"` is in the loader's supported table with `reg-shift` parsing.)

> **Verdict — 7.3:** Baud-0, 32-bit access and reg-shift are all safe in Haiku-Graviton's SPCR code; the sole blocker is that interface type `0x12` is unrecognized, which on a pure-ACPI boot means **a completely silent loader and kernel** — a one-line switch-case fix.

### 7.4 MCFG, GTDT, PPTT

**MCFG — all 5 segments, ECAM bases (quoted):**

| Seg | MCFG Base Address | Bus range | ECAM actually used (live) | Controller (§4 DT table) | Endpoint |
|---|---|---|---|---|---|
| 0 | `0x0000000900008000` | 1–1 | `0x900108000` | `pcie@fe150000` (M.2 M-key) | *empty* |
| 1 | `0x0000000940000000` | 1–1 | `0x940100000` | `pcie@fe160000` | ASM1164 SATA |
| 2 | `0x0000000980008000` | 1–1 | `0x980108000` | `pcie@fe170000` (E-key) | no link |
| 3 | `0x00000009C0008000` | 1–1 | `0x9c0108000` | `pcie@fe180000` | RTL8125 #1 |
| 4 | `0x0000000A00008000` | 1–1 | `0xa00108000` | `pcie@fe190000` | RTL8125 #2 |

Diff vs the DT recon (§4): same five controllers, same segment numbering, but a very different *shape*. Under DT, Linux drives the DesignWare bridges natively (config via the `a4xxxxxxx.pcie` iATU windows) and sees the root ports as bus-0 devices. Under ACPI the firmware instead exposes a **synthetic bus-1-only ECAM window inside each segment's 64-bit MEM aperture** — start-bus = end-bus = 1, root ports hidden entirely (`_BBN` returns 1; live `lspci` shows only the three endpoints, no RK3588 bridges). The bases are offset so `base + (bus<<20)` lands on the iATU window (hence the odd `…008000`). The DSDT `RES1` devices reserve each ECAM area (`ECAM area … reserved by PNP0C02:0x`), and the `RES0` devices reserve the DW DBI spaces at `0xA40000000…0xA41000000`. Two decode artifacts leak through: `pci 0003:01:1f.0 / 0004:01:1f.0: unknown header type 7f` — phantom devfn 31, because the shim only truly decodes device 0 (upstream maintainer, verbatim: *"nothing can be done to make ECAM look right on this platform"*). Per-segment INTx routing comes from trivial `_PRT`s — one fixed GSIV per segment, all four pins: **292 / 287 / 272 / 277 / 282** for segments 0–4.

**GTDT (96 bytes, rev 2):** no counter block (`0xFFFF…F`), no platform timers. Interrupts: Secure EL1 = **29**, Non-Secure EL1 = **30**, Virtual = **27**, Non-Secure EL2 = **26**, all flags `0x2` (level-triggered, active-low). Matches the architectural PPIs the DT boot used (`arch_timer` on PPI 26 under VHE). Haiku doesn't read GTDT at all — its arm64 timer hardcodes the virtual-timer PPI 27 (`arch_timer.cpp:23`) and takes frequency from `CNTFRQ_EL0` — and on this board both happen to be correct (firmware programs CNTFRQ to 24 MHz).

**PPTT (544 bytes):** hierarchy = 1 package node (UID 8, private resource → **L3: 3 MiB, 12-way, 4096 sets, unified, WB** at offset 0x178) → **3 cluster nodes** (UID 9 with 4 children, UID 10 with 2, UID 11 with 2) → 8 CPU nodes (ACPI Processor IDs 0–7). Per-core caches: A55 = 32K D / 32K I / 128K L2; A76 = 64K D / 64K I / 512K L2. So the **4 + 2 + 2 topology with the shared L3 matches the DT ground truth exactly** — including the three-cluster split that §2 flagged.

One real bug: **every CPU node has `Flags : 0x00000002` — `ACPI Processor ID valid` only; `Node is a leaf : 0`.** The PPTT leaf flag is missing, so an OS that trusts it cannot tell cores from SMT threads. Measured live on the ACPI boot: `lscpu` reports *"Thread(s) per core: 4"* for the A55 cluster, and sysfs shows `cpu0..3: core_id=9, thread_siblings=0-3` — Linux collapsed each cluster into one "core" with N "threads". Cosmetic for correctness, poisonous for any scheduler that penalizes SMT siblings.

Also absent, with consequences confirmed live: **no `_CPC`/CPPC objects** (→ `/sys/devices/system/cpu/cpufreq/` is empty — no DVFS whatsoever, cores pinned at firmware-preset clocks) and **no ThermalZone objects** (→ zero `thermal_zone*`; the pwm-fan is invisible; the only thermal management is the firmware's own `CoolingFanState/Speed` EFI-variable control).

> **Verdict — 7.4:** MCFG is honest about all five segments and the `_PRT`s are trivial single-GSIV — good news for a simple loader; GTDT matches the hardcoded assumptions Haiku already makes; PPTT gets the 4+2+2+L3 shape right but its missing leaf flag misleads any OS that consumes it, and the absence of CPPC/thermal means an ACPI-booted OS runs at fixed clocks with no thermal view — acceptable for bring-up, unacceptable for production.

### 7.5 DSDT device inventory (14,024 bytes — all 44 devices)

**Functional, real (or workable) HIDs:**

| Device | _HID / _CID | MMIO | Notes |
|---|---|---|---|
| `UAR2` | `HISI0031` | `0xFEB50000` | dw-apb-uart; `_DSD` reg-shift 2 / io-width 4 / clock 24 MHz. **Works** (ttyS0) |
| `DMA0-2` | `ARMH0330` | — | PL330 ×3; bind (`dma-pl330`) |
| `EHC0/1` | `RKCP0D20` + CID `PNP0D20` | `0xFC800000/0xFC880000` | bind via generic CID |
| `OHC0/1` | `PRP0001` → `"generic-ohci"` | `0xFC840000/0xFC8C0000` | the one PRP0001 shim that *works* |
| `XHC0/1/2` | `PNP0D10` | `0xFC000000/0xFC400000/0xFCD00000` | all three bind — including XHC2, which DT keeps *disabled* |
| `SDHC` | `RKCPFE2C` (`_DSD` compat `rockchip,rk3588-dwcmshc`) | `0xFE2C0000` | SD; `_STA 0x0F` |
| `SDC3` | `RKCP0D40` | `0xFE2E0000` | eMMC (sdhci-dwcmshc) |
| `PCI0-4` | `PNP0A08`/`PNP0A03` | — | `_SEG` 0–4, `_BBN`=1, `_PRT` fixed GSIVs, `_OSC` masks native hotplug |
| `RES0` ×5 | **`AMZN0001`** + CID `PNP0C02` | DBI `0xA40000000…` | HID copy-pasted from **Amazon Graviton** EDK2 — the ACPI port's ancestry, literally in the namespace |
| `SCMI` | `PNP0C02` | shmem `0x0010F000`, doorbell `0xFEC60030` | full SCMI clock client *written in AML* (`SMT`/`CLRG`/`CLRS`/`CLCS` methods, protocol 0x14) — clock get/set via shared memory + doorbell |
| `PKG0/CLU0-2/CPU0-7` | `ACPI0010`/`ACPI0007` | — | processor containers mirroring PPTT |
| `JACK` | `ESSX8316` | I2C7 child | es8316 codec — declared, but unreachable (see below) |

**Deliberately disabled:** `ATA0/1/2` (`RKCP0161`, compat `rockchip,rk-ahci`) — `_STA = 0x00`. Correct for this board: native SATA is unused (ASM1164 does the work), matching the DT where `sata@fe2*` are disabled.

**Broken PRP0001 shims (confirmed live):**
* `I2C1–8` (`RKCP3001` + CID `PRP0001`): `_DSD` has `i2c,clk-rate` etc. but **no `"compatible"` entry** → kernel: `ACPI: \_SB_.I2Cx: PRP0001 requires 'compatible' property` ×8 → **zero I2C buses**. Everything behind them is unreachable: `hym8563` RTC (live: `/dev/rtc*` absent — which is also why every journal boot-timestamp starts at a bogus epoch date), FUSB302 USB-C, ES8316 codec.
* `PINC` (`PRP0001` → `"rockchip,rk3588-pinctrl"`): the Linux driver is OF-only → `rockchip-pinctrl PRP0001:00: error -ENODEV: device tree node not found`. `GPI0–4` (`RKCP3002`, `"rockchip,gpio-bank"`) depend on it → all GPIO dead.

**Missing entirely from the namespace** (vs the DT world of §2–§6): GPU (Mali G610), the whole display path (VOP2, HDMI TX/RX — only the BGRT logo + GOP `simpledrm` framebuffer exist), **NPU** (under ACPI even the thermal/QoS satellites vanish), TSADC/thermal, **all SPI controllers** (→ no rk806 PMIC access, no NOR-flash MTD — live: `/dev/mtd*` gone), PWM/fan, GMAC ethernets (also disabled in DT — genuinely absent hardware paths), SARADC, video codecs, RGA, **all IOMMUs** (no SMMU device, no IORT — coupled with `_CCA = Zero` on all 27 DMA-capable devices, i.e. non-coherent DMA correctly declared but never remapped), crypto/TRNG node, watchdog, eFuse.

> **Verdict — 7.5:** The ACPI namespace is a *server-shaped* subset — UART, USB, PCIe, SD/eMMC, DMA and CPUs are real and bind; everything SoC-flavoured (pinctrl/GPIO/I2C/SPI/PMIC/RTC/thermal/GPU/display/NPU) is either a broken PRP0001 shim or absent — so a pure-ACPI DeBeOS could never see the PMIC, RTC, fan or NPU no matter how good its ACPI support; those need DT or hardcoded board knowledge.

### 7.6 What actually broke in the ACPI boot (error log ↔ table mapping)

From `journalctl -p err` of the `acpi=force` boot (plus the err-adjacent dmesg lines the `-p err` filter missed):

| Failure (verbatim) | Root cause table | Severity |
|---|---|---|
| `PCI: OF: of_root node is NULL, cannot create PCI host bridge node` ×5 | No DT at all (expected under `acpi=force`) — Linux OF-glue noise, one per host bridge | benign |
| `pci 0003:01:1f.0 / 0004:01:1f.0: unknown header type 7f, ignoring device` | MCFG: synthetic bus-1 ECAM shim decodes only device 0; devfn 31 reads float | benign, diagnostic of the shim |
| `rockchip-pinctrl PRP0001:00: error -ENODEV: device tree node not found` | DSDT `PINC`: PRP0001 shim for an OF-only driver | **pinctrl + all GPIO dead** |
| `ACPI: \_SB_.I2Cx: PRP0001 requires 'compatible' property` ×8 *(dmesg)* | DSDT `I2C1–8`: `_DSD` missing `"compatible"` | **all I2C dead → no RTC, no USB-C, no codec** |
| `ACPI FADT declares the system doesn't support MSI, so disable it` *(dmesg)* | FADT boot-arch bit 3 (§7.2) | **all PCIe on legacy INTx** |
| `ITS: No ITS available, not enabling LPIs` *(dmesg)* | MADT: zero ITS subtables (§7.1) | same consequence as above |
| `_OSC: not requesting OS control` ×5 *(dmesg)* | downstream of the FADT MSI bit | no native AER/hotplug/ASPM control |
| Journal dates stuck at a bogus epoch | DSDT: RTC unreachable (I2C dead) | wall clock wrong until NTP |
| tpm2-tss users, firewalld zone, SELinux rpcbind, NFS-mount timeouts to a storage server | none — userspace/environment noise unrelated to ACPI | ignore |

> **Verdict — 7.6:** Nothing in the ACPI boot *crashed* — the failures are all quiet capability losses (MSI, I2C/RTC, pinctrl/GPIO, DVFS, thermal), each traceable to exactly one table defect, and the two ugliest (`MSI Not Supported`, missing ITS) are one commit's worth of firmware source.

### 7.7 Verdicts for the two projects

#### (a) Can Haiku-Graviton's existing ACPI path reach serial + SMP + PCIe-INTx on this board as-is?

**Serial: NO (one line away).** SPCR interface type `0x12` is not in `arch_acpi_uart_kind()`'s switch (`arch_acpi.cpp:46-60` — knows 0/1/3/0x0d/0x0e only); DBG2 carries the same subtype. `uart.kind` stays empty → no loader console *and* no kernel debug console. Everything else — DWord-GAS→reg-shift-2, never reprogramming baud (`gUARTSkipInit`), clock-0 fallback — is already correct or harmless. Fix: add `case 0x12: return UART_KIND_8250;`.

**SMP: YES, as-is.** Verified end-to-end in source against this exact MADT: the GICC loop honours `ACPI_MADT_GICC_ENABLED`, registers MPIDRs (`arch_acpi.cpp:305-350`); the standalone GICR subtable is parsed (`arch_acpi.cpp:362-366` → `gicr_base = 0xFE680000, size 0x100000`) so the all-zero per-GICC redistributor fields don't matter; the GICv3 branch fires (`gicd_base && gicr_base && version >= 3`); FADT `PSCI Compliant/SMC` is consumed (`arch_acpi.cpp:414-422` → `PSCI_CONDUIT_SMC`) and CPUs start via `PSCI_CPU_ON` with the MADT MPIDRs. PMU GSIV 23 is carried in `intc_info.pmu_gsiv`.

**PCIe-INTx: NO — one real bug.** The pieces exist: kernel `ECAMPCIControllerACPI` reads MCFG, walks `_CRS`, and *does* consume `_PRT` via `prepare_irq_routing()`/`enable_irq_routing()` (`ECAMPCIControllerACPI.cpp:331-336`) — and this DSDT's `_PRT`s are the trivial fixed-GSIV kind. But the MCFG-allocation matcher selects by **bus range only and never evaluates `_SEG`** (`ECAMPCIControllerACPI.cpp:63-93`; the only `pci_segment` uses are a print, a segment-0 fallback, and a window-join check). On this board all five allocations are bus 1–1 — indistinguishable by bus range — so **all five host bridges resolve to segment 0's ECAM: the empty M.2 slot.** Result: SATA and both NICs invisible. Fix: evaluate `_SEG` on each `PNP0A08` and match `alloc->pci_segment`. Domain capacity is fine (`MAX_PCI_DOMAINS = 8` ≥ 5). The enumerator must also tolerate the phantom devfn-31 (header 0x7f) reads.

**Plus one landmine before any boot on this board:** firmware `ConfigTableMode = 3` installs **both** ACPI and DTB config-table entries, and `dtb_init()` runs unconditionally after `acpi_init()` (`start.cpp:249-261`). The UART and GIC are protected by `kind[0]` guards, but **CPU registration is not** — `arm64_handle_fdt_cpu_node()` (`arch_smp.cpp:232-263`) re-registers all 8 cores on top of the MADT's 8. Either dedupe by MPIDR in `arch_smp_register_cpu()` or set the firmware to a single-table mode for testing.

**Exact gap list:** (1) SPCR type 0x12 → 1 line; (2) `_SEG`-aware MCFG matching → ~20 lines in `ECAMPCIControllerACPI::ReadResourceInfo`; (3) MPIDR dedupe for dual-table boots → ~10 lines in `arch_smp.cpp`; (4) *nice-to-have:* correct 24 MHz DW-APB clock fallback. Nothing else stands between this firmware's tables and a serial+SMP+INTx'd Haiku.

#### (b) Spec: `dtb_init` backfills `intc_info.regs3` (ITS) when ACPI set the GIC

Current state, verified: `intc_info` (`headers/private/kernel/boot/interrupt_controller.h:41-67`) carries `regs1` = GICD, `regs2` = GICR, **`regs3` = ITS ("Zero when absent")**. Only the ACPI path ever fills `regs3` (`arch_acpi.cpp:356-361, 388-389`); the FDT path fills just `kind/regs1/regs2` and is entirely skipped when ACPI won, via the guard in `arch_handle_fdt()`:

```c
// src/system/boot/platform/efi/arch/arm64/arch_dtb.cpp:64
if (interrupt_controller.kind[0] == 0) {
```

Kernel consumption is already wired: `arch_int_init_io()` → `GICv3InterruptController::InitITS(regs3.start, regs3.size)` (`arch_int.cpp:113-129`), which returns benign `B_NAME_NOT_FOUND` when zero (`arch_int_gicv3.cpp:596-599`); on success the 979-line `GICv3ITS` driver registers as the `MSIInterface` and the PCI bus manager auto-discovers it via `msi_supported()`.

**The change** (all in `arch_handle_fdt()`, `arch_dtb.cpp` — the `:64` guard itself stays untouched):
1. Add a *parallel* matcher, outside/after the `kind[0] == 0` block, so it runs even when ACPI provided the GIC: match `dtb_has_fdt_string(compatible, compatibleLen, "arm,gic-v3-its")` — the ITS is a *child node* of the GIC in the RK3588 DT (`msi-controller@fe640000/fe660000`), not reg index 2 of the GIC node, so the existing `dtb_get_reg(node, 2, …)` pattern cannot reach it.
2. Guard: `if (interrupt_controller.kind[0] != 0 && interrupt_controller.regs3.start == 0)` — i.e. a GIC is known (from either path) and no ITS yet. Then `dtb_get_reg(fdt, node, 0, interrupt_controller.regs3)` (`dtb.cpp:386-459`, already does `ranges` translation).
3. Nothing else: handoff (`dtb_set_kernel_args()`) and the kernel side need zero changes.

Two caveats that belong in the same work item:
* **Single-`regs3` limitation:** this board has *two* ITSes (`0xFE640000` for the pcie2x1l* segments, `0xFE660000` for pcie3x*, per the DT `msi-map`). First-match backfill gets one of them — the ACPI path has the same "only the first ITS is used" comment. MSI-routing all five segments through one ITS won't work for the segments mapped to the other; extending `intc_info` with an ITS array (mirroring `gicr_regions[]`) is the eventual fix.
* **Erratum RK3588001 / non-coherent ITS:** Haiku's `GICv3ITS::Init` programs `GITS_BASER/CBASER` assuming coherent table walks. On RK3588 the ITS is non-coherent (§3: Linux enables *"workaround for ITS: Rockchip erratum RK3588001"* + *"non-coherent attribute"* + command-queue cache flushing). Backfilling `regs3` without porting that quirk trades "no MSI" for "corrupted MSIs". The driver needs: non-shareable/non-cacheable attributes in BASER/CBASER, cache-clean before every command-queue write, and the RK3588001 pre-ITS address workaround — keyed off the FDT `dma-noncoherent` property or, on ACPI boots, off the `RKCP`/`RK3588` OEM IDs.

#### (c) Fixing edk2-rk3588 itself (MADT ITS + IORT + FADT MSI flag)

All three patch sites located and sized in the v1.1 source tree:

| Patch | Where | Size | Notes |
|---|---|---|---|
| **MADT ITS ×2** | `edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/Madt.aslc` — uncomment struct members (lines 29–30) and `EFI_ACPI_6_0_GIC_ITS_FRAME_INIT(0, 0xfe640000) / (1, 0xfe660000)` (lines 199–202) | **~4 lines, 1 file** | The init macro already exists (`Include/AcpiTables.h:55-63`); table length is `sizeof()`-derived and the checksum is fixed by `AcpiTableDxe` — nothing manual |
| **Clear FADT MSI flag** | `…/Fadt.aslc:60`: `EFI_ACPI_6_3_MSI_NOT_SUPPORTED` → `EFI_ACPI_RESERVED_WORD` | **1 line, 1 file** | Exact revert of commit `4684b3ad` |
| **Minimal IORT** | `…/RK3588PcieIort.aslc` **already exists in-tree** (117 lines: 2 ITS group nodes with GicItsIds 0/1 + 1 root-complex node for segment 0, full-range ID map, CCA=0) — it shipped until commit `1ea0d782` (2023-06-24) removed it from every platform `.inf` | **1 line** to re-wire into `Platform/Radxa/ROCK5ITX/AcpiTables/AcpiTables.inf`; **~90–120 lines** to do it properly (5 RC nodes, `NumNodes` 3→7, per-segment ITS routing: seg 0/1 → its1, seg 2/3/4 → its0 per the DT `msi-map`) | Reference IORTs to crib: `Platform/RaspberryPi/AcpiTables/Iort.aslc` in the edk2-platforms submodule |

**Why upstream won't take it (and what that means):** issue #65 ("MSI support for PCIe") was closed *not_planned* 2025-02-28. Maintainer, verbatim: *"Not possible to have MSIs through ITS due to Rockchip erratum 3588001, but the OSes we're interested in do support SPI-based MSI by having a V2M entry in MADT. The definition is already there but commented out as it needs more testing."* And on #129: *"There's no upstreaming effort going on for ACPI on rk3588"* — ACPI is developed and tested **against Windows only** (README, verbatim: *"There are no plans to further improve functionality for other OSes"*). Crucially, Linux's ITS quirk (`its_enable_rk3588001`) is gated on `of_machine_is_compatible("rockchip,rk3588")` — **it cannot fire on an ACPI boot**, so even a patched firmware gives Linux broken MSIs without a kernel patch. DeBeOS controls its own ITS driver, so it can key the quirk off the ACPI OEM IDs — the firmware patch is only useful *paired with* the driver-side erratum work from (b).

The maintainer's hint is also a genuine third option: the commented-out **`GicMsiFrame`** (MADT type 0x0D, GICv2m-style) pointing at the GICv3 **MBI frame `0xFE610000`** (SPIs 424–479, live-verified in §3) would give SPI-based MSIs that bypass the ITS *and* its erratum entirely. Haiku currently has no type-0x0D consumer and no v2m/MBI driver — a new-driver-sized effort, but erratum-free.

### 7.8 Final review — adversarial counterpoints

Positions a skeptical reviewer should (and did) raise against this report's implicit direction:

**"Drop ACPI entirely — DT-first is strictly richer on this board."** Mostly true and worth saying plainly: even Linux, with the most mature ARM64 ACPI stack in existence, comes up degraded under these tables (no DVFS, no thermal, no I2C/RTC/PMIC, no MSI, phantom PCI devices), while the DT boot is fully featured. *But the counterpunch is real:* under DT, Haiku-Graviton has **no PCIe at all** — its FDT ECAM driver matches only `"pci-host-ecam-generic"`, and RK3588's `rockchip,rk3588-pcie` DesignWare bridges need a host driver Haiku doesn't have. Under ACPI, the firmware's ECAM shim *does the DesignWare iATU work for you* and a dumb ECAM driver sees real endpoints. The pragmatic answer is the hybrid the firmware already ships (mode 3): ACPI for CPUs/GIC/PCIe, DT for everything SoC-flavoured — which is exactly what the (a)+(b) gap list assumes.

**"Fix the firmware, not the loader."** The firmware patch is 6 lines and benefits every OS — but upstream explicitly won't take it (#65 closed not_planned; ACPI is Windows-only per README), so it means carrying a fork of an 8 MiB SPI-NOR image and reflashing on every release, with MaskROM recovery as the safety net. The loader-side backfill (b) keeps the board bootable on *stock* firmware. Do the loader work first; treat the firmware fork as an accelerator, not a dependency.

**"ITS/MSI is premature — INTx is fine for bring-up."** Correct, and the numbers back it: three endpoints, each alone on its segment with a dedicated SPI (292/287/272/277/282) — zero interrupt sharing, minimal latency cost at bring-up traffic levels. The counter is only medium-term: an NVMe drive in the M.2 slot plus multiqueue networking will eventually want MSI-X, and the erratum work is the long pole — starting it late means blocking on it later.

**"These tables are immature; trust nothing without cross-checking."** Sustained. This one recon found: the PPTT leaf-flag bug (cores misread as SMT threads), a rev-2 SPCR with a subtype (0x12) that mainstream parsers only grew support for recently, phantom devfn-31 devices, `_HID "AMZN0001"` copy-pasted from Graviton, and eight I2C shims that fail ACPI's own PRP0001 rules. Windows is the only tested consumer. Every table used going forward should be validated against the DT ground truth first — as done here — and the loader should carry defensive checks (bounds-checked MADT walks, tolerate header-0x7f reads) rather than assuming SBSA-grade firmware.

**"Hybrid mode 3 is a trap, not a gift."** Partially sustained. It's what makes the ACPI+DT best-of-both plan possible, but it's also the source of the CPU double-registration hazard (§7.7a) and of subtle "who owns the UART" questions. The loader must be *written for* dual-table boots (dedupe by MPIDR, kind[0]-style claim guards on every shared resource) — or bring-up should pin the firmware to one mode until that hardening exists.

**Scope check (what this section deliberately did *not* establish):** whether Windows' inbox drivers accept these tables (irrelevant to DeBeOS), whether the V2M/MBI path actually works on RK3588 silicon (upstream says "needs more testing" — nobody has), and whether the ACPI ECAM shim is robust under config-space hammering (only enumeration-time behaviour was observed). All three are cheap to test once a loader boots.

> **Verdict — §7:** The ACPI tables are real, minimal, and 90 % honest — good enough that Haiku-Graviton's existing ACPI path is **three small patches away** (SPCR 0x12, `_SEG`-aware ECAM matching, MPIDR dedupe) from serial + SMP + PCIe-INTx on stock firmware; MSI is the one deep gap, blocked jointly by a deliberate 1-line FADT flag, four commented-out MADT lines, and a silicon erratum whose workaround Haiku's ITS driver doesn't yet implement — and the NPU remains, as in every other mode, invisible to ACPI entirely.

---

## Open questions / needs a reboot or config change to confirm

1. **[RESOLVED — see §7]** **Which ACPI tables does the firmware actually install?** *(highest value for project A)*
   The RSDP is at `0xeed8b018` and `/proc/iomem` shows that page as `reserved`, but
   `CONFIG_STRICT_DEVMEM=y` + `CONFIG_IO_STRICT_DEVMEM=y` blocked reading it via `/dev/mem`, and
   scanning the SPI NOR found **zero** uncompressed ACPI signatures (`FACP`, `DSDT`, `APIC`, `GTDT`,
   `PPTT`, `MCFG`, `SPCR`, `DBG2`, `IORT`…) — they live inside LZMA-compressed firmware volumes.
   **Follow-up:** boot once with `acpi=force` on the kernel cmdline (arm64 honours it, and it overrides
   the "DT present ⇒ ACPI off" rule), then read `/sys/firmware/acpi/tables/` and `acpidump`. That
   tells you whether MADT/GTDT/PPTT/DBG2/MCFG/IORT are complete enough for an ACPI-first loader, or
   whether the ACPI path is a stub. Expect peripherals to regress under `acpi=force` — this is a
   throwaway diagnostic boot, and the board runs an unrelated workload, so schedule it.
2. **[RESOLVED — see §7: it is a bitmask; 3 = Both, per `VarStoreData.h:61-66`]** **Is `ConfigTableMode=3` literally "ACPI+FDT" in this firmware build?** The empirical evidence
   (both an RSDP *and* a full FDT delivered) is conclusive about behaviour; the enum-to-label mapping
   is from `edk2-rk3588` convention. Confirm in the UEFI setup menu (visual, needs a monitor +
   reboot) or against the `edk2-rk3588` source for tag `v1.1`.
3. **[RESOLVED — see §7.7a: ACPI-first, FDT fills gaps behind per-resource guards]** **Which loader path does DeBeOS/Haiku actually take on arm64 — ACPI-first or FDT-first?** Not
   answerable from this board. It changes the plan completely: FDT-first works today untouched;
   ACPI-first depends entirely on item 1. Worth settling before any code is written.
4. **Does the patched-DTB NPU path actually produce `/dev/accel/accel0`?** Requires a reboot with a
   modified DTB (patch `status`+`npu-supply` on 3 `npu@` + 3 `iommu@` nodes, then GRUB `devicetree`).
   Unknown until tried: whether power domains 9/10/11 come up cleanly when the firmware never touched
   them, and whether `assigned-clock-rates = <200000000>` is satisfiable through SCMI-backed clocks.
5. **Side effect risk of replacing the firmware DTB.** The firmware DTB (180 KB, with `__symbols__`)
   and Fedora's (105 KB) agree on every node status I compared — but I compared ~18 nodes, not all
   ~250. Before switching wholesale, diff the full node/property set; PCIe ComboPHY mode, SATA and
   HDMI routing are all firmware-configured (`ComboPhy0/1/2Mode=1`, `Pcie30State=1`,
   `Pcie30PhyMode=0`) and a mismatched DTB could drop the boot disk.
6. **What is on `pcie@fe170000` (segment `0002:20`)?** *"Device found, but not active"* — something is
   detected but never links. Needs physical inspection of the M.2 E-key / WiFi slot.
7. **Why is HDMI0 (`hdmi@fde80000`) disabled upstream for this board?** It is disabled in *both*
   DTBs. Could be an unfinished upstream board file or a deliberate ITX routing decision. Affects
   whether "one output" is a permanent constraint. (Also unverified: whether HDMI1 lights up at all —
   nothing was connected during recon; `card0-HDMI-A-1` read `disconnected` throughout.)
8. **Native SATA (`sata@fe210000/20/30`) disabled in both DTBs** while the board's ports run through
   the ASM1164. Confirm that is the ITX design (vs. a DT gap) before writing any RK3588 AHCI support —
   it may be dead code for this board.
9. **Whether TF-A's SCMI clock domain is required for the NPU.** The NPU node's `npu` clock comes from
   the SCMI clock provider (`protocol@14`), not the CRU. If EL3 doesn't expose it, a bare-metal
   (non-Linux) NPU driver — or a DeBeOS one — needs the SMC path, not just MMIO on the CRU.
10. **TF-A/OP-TEE exact provenance.** Flash carries both `bl31-v1.45` / `v2.3-682-g4ca8a8422` and a
    `v2.12.0(release):d5c68fd` string; `bl32-v1.17` for OP-TEE. Which binary owns which string wasn't
    resolved from strings alone, and it matters if you ever need to rebuild BL31 to relax an EL3 policy.

---

### Appendix — artifacts left on the board

* `/tmp/dtrecon/fw.dtb`, `fw.dts` — the live firmware DTB and its decompilation
* `/tmp/dtrecon/fedora.dtb`, `fedora.dts`, `rock5b.dts` — Fedora's ITX DTB and the ROCK 5 B reference
* `/tmp/dtrecon/spinor.bin` — 16 MiB dump of `/dev/mtd0ro` (the EDK II + BL31 + OP-TEE + SPL image)
* Installed packages: `dtc`, `efibootmgr`, `i2c-tools`, `nvme-cli`, `dmidecode`, `libfdt`, `libi2c`
* `rocket` module was loaded for the bind test and **unloaded again** (`lsmod | grep -c rocket` → 0)
