# ACPI tables — Radxa ROCK 5 ITX (RK3588)

Raw `acpidump` output and decompiled tables from a ROCK 5 ITX running:

- **Firmware:** EDK II from [`edk2-porting/edk2-rk3588`](https://github.com/edk2-porting/edk2-rk3588),
  version **v1.1** (tag `6a682c0e`), build date **2025-04-09**, build target **ROCK5ITX**
  (`RELEASE_GCC`), SMBIOS BIOS version `v1.1`
- **Config:** `ConfigTableMode = 3` (ACPI + FDT both installed); dump captured on a Linux boot
  with `acpi=force` (kernel 7.0.7, Fedora 44)
- **Tools:** ACPICA 20260408 (`acpixtract -a rock5itx-acpi.dat` + `iasl -d *.dat`)

| File | Table | Notes |
|---|---|---|
| `rock5itx-acpi.dat` | raw acpidump | source of everything below; reproducible with the tools above |
| `apic.dsl` | MADT | 8× GICC (all enabled, MPIDRs 0x0–0x700), 1× GICD v3 @`0xFE600000`, 1× GICR @`0xFE680000`+1 MiB, **zero ITS entries** |
| `facp.dsl` | FADT | HW-reduced 6.3; `ARM Boot Arch = PSCI Compliant, SMC`; **Boot Flags bit 3 "MSI Not Supported" set** |
| `spcr.dsl` | SPCR rev 2 | interface type **0x12**, DWord GAS @`0xFEB50000`, GSIV 365, baud byte 0, no clock field |
| `dbg2.dsl` | DBG2 | same UART, namepath `\_SB.UAR2` |
| `mcfg.dsl` | MCFG | 5 segments, bus 1–1 each (synthetic bus-1-only ECAM shims) |
| `gtdt.dsl` | GTDT | timer PPIs 29/30/27/26, no counter block, no platform timers |
| `pptt.dsl` | PPTT | package → 3 clusters (4×A55 + 2×A76 + 2×A76) + shared L3; **CPU leaf flag missing** |
| `dsdt.dsl` | DSDT | 44 devices; see recon report §7.5 for the full inventory |
| `bgrt.dsl` | BGRT | boot logo |

Analysis, live-boot verification, and the mapping of each defect to its source line in
edk2-rk3588 are in [`../docs/rock5itx-recon.md`](../docs/rock5itx-recon.md), §7.
