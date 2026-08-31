# DeBeOS on RK3588 — board bring-up seed

Board bring-up notes and findings for **DeBeOS (Haiku) on RK3588 hardware**, starting with the
**Radxa ROCK 5 ITX**. This directory (and the `rk3588-bringup` branch it lives on) plays the same
role for the hardware-expansion phase that
[haiku-on-ec2](https://github.com/felipedbene/haiku-on-ec2) played for the EC2/Graviton work: a
place for recon reports, firmware analysis, and decompiled artifacts *before* any code lands. It
is the **seed** of what may later split into its own repository; it sits on a branch of
[Haiku-Graviton](https://github.com/felipedbene/Haiku-Graviton) because the loader/kernel work it
feeds (ACPI path, GICv3/ITS, ECAM PCI) lives here on the `graviton` branch.

## Status — honest

**Recon + firmware analysis phase. DeBeOS has not booted on this hardware yet.**

What exists so far:

- A full hardware/firmware recon of the ROCK 5 ITX under Fedora (DT mode *and* an `acpi=force`
  boot): [`docs/rock5itx-recon.md`](docs/rock5itx-recon.md) — firmware boot chain (read out of the
  SPI NOR), CPU topology, GICv3/ITS layout, PCIe/peripheral map, NPU (Rocket) status, display, and
  a §7 deep dive into the ACPI tables including a source-level comparison against this repo's
  arm64 EFI loader and against upstream `edk2-rk3588`.
- The raw `acpidump` and all decompiled tables: [`acpi/`](acpi/).
- A concrete, sized work list with the open/unclear decision points:
  [`docs/planned-work.md`](docs/planned-work.md).

Headline finding: with the stock edk2-rk3588 v1.1 firmware, this repo's existing ACPI loader path
is **three small patches away** (SPCR interface-type 0x12, `_SEG`-aware MCFG matching, MPIDR
dedupe for dual-table boots) from serial + SMP + PCIe-INTx on this board. MSI/ITS is the one deep
gap — blocked jointly by firmware table omissions and Rockchip erratum RK3588001. Details and
adversarial counterpoints in the report.

## AI disclosure

As with the other repos in this project family (Haiku-Graviton, haiku-on-ec2): this work is
**AI-assisted**. The recon, table decompilation, source cross-referencing, and these documents
were produced with Claude (Anthropic) working under human direction, with findings verified
against live hardware and upstream source where stated. Mistakes remain the human's.
