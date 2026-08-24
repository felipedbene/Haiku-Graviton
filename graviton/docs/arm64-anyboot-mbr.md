# arm64 `@nightly-anyboot`: the `base_mbr.bin` / `-m32` problem

Status: **fix MERGED and build-validated** — `d5d9a4809e` "anyboot: build a signed
empty MBR on non-x86 targets" (2026-08-21). `base_mbr.bin` builds without `-m32`;
sector 0 has the 0x55AA signature and the anyboot tool overlays the 0xeb/0xef
partition entries correctly.

> **Second half of this status corrected 2026-08-24.** It used to read: *"NOTE: the
> full `@nightly-anyboot` image is additionally blocked by an unrelated, pre-existing
> arm64 failure — see 'Separate blocker' below."* **That blocker is FIXED**
> (`949bac43cc`) — see the "Separate blocker" section, which now carries its
> resolution. Nothing here is blocked on it.
>
> **Still genuinely outstanding:** the **UEFI boot-test** of an anyboot image. That
> was gated on the separate blocker and is now unblocked, but I found no record of it
> having been run — **UNVERIFIED as of 2026-08-24**.
>
> Note also that the two headings below — "Proposed fix (to validate)" and "Test plan
> (must pass before applying)" — are stale as *headings*: the fix is applied and
> merged. They are kept because the reasoning and the hexdump checks are the useful
> part.

## Symptom

Building `@nightly-anyboot` for arm64 fails:

```
BuildMBR objects/haiku/arm64/release/base_mbr.bin
aarch64-unknown-haiku-gcc: error: unrecognized command-line option '-m32'
../src/bin/writembr/mbr.S -o .../base_mbr.bin -nostdlib -m32 -Wl,--oformat,binary ...
```

`build/jam/images/AnybootImage:23-25` unconditionally assembles `src/bin/writembr/mbr.S`
(16/32-bit **x86** boot-sector assembly) via `BuildMBR` (`build/jam/BootRules:227-238`,
which hardcodes `-m32`). The arm64 cross-compiler can neither accept `-m32` nor
assemble x86 code, so the anyboot image can't be built on arm64.

## Why we cannot simply drop or zero the MBR

`src/tools/anyboot/anyboot.cpp` builds an **MBR-partitioned hybrid image**, not a GPT:

1. `main()` copies `base_mbr.bin` (the `-b` arg) over sector 0 (`copyLoop(bios, out, 0)`).
2. `createPartition()` writes 16-byte MBR partition entries into that sector at
   `512 - 2 - 16*(4-index)` (offsets 446, 462, ...): partition 0 = type `0xeb`
   (Haiku BFS, active), partition 1 = type `0xef` (EFI System Partition).
3. anyboot does **not** write the `0x55AA` boot signature (bytes 510-511) itself —
   it relies on `base_mbr.bin` providing it (mbr.S ends with the signature).

On arm64 there is no BIOS, so the x86 **boot code** (bytes 0-445) is never executed.
But UEFI firmware still reads sector 0's **partition table** to find the `0xef` ESP and
load the EFI loader, and a valid MBR requires the **`0x55AA` signature**. A zeroed
sector 0 would lose the signature and the table location → firmware may not find the
ESP → no boot. This matches the maintainer's recollection that "it wouldn't pass the
boot loaders without that."

## Proposed fix (to validate)

In `build/jam/images/AnybootImage`, gate the x86 assembly by target arch:

- x86 arches (`x86_gcc2 x86 x86_64`): keep `BuildMBR base_mbr.bin : mbr.S` (real BIOS
  boot code + signature).
- other arches (arm64, riscv64, ...): produce `base_mbr.bin` as a **512-byte block of
  zeros with `0x55AA` at offset 510** (no x86 code). anyboot then overlays the
  partition table exactly as before; UEFI gets a valid MBR + ESP entry.

Sketch (new action, e.g. in `BootRules`):

```jam
# 512-byte MBR with only the boot signature; used on non-x86 (UEFI) targets where
# the x86 boot code is never executed but the partition table + 0x55AA are required.
actions CreateSignedEmptyMBR {
    dd if=/dev/zero of=$(1) bs=512 count=1 2>/dev/null
    printf '\125\252' | dd of=$(1) bs=1 seek=510 conv=notrunc 2>/dev/null
}
```

**IMPORTANT (dash):** jam actions run under `/bin/sh` = dash, whose `printf` does
**not** interpret `\xHH`. Using `printf '\x55\xAA'` writes the literal 8-byte string
and produces a 518-byte file. Use the POSIX **octal** form: `0x55=\125`, `0xAA=\252`.
This is implemented in `build/jam/BootRules` (rule/actions `CreateSignedEmptyMBR`) and
gated in `build/jam/images/AnybootImage`.

## Relationship to the EC2 image path

The AWS EC2 AMIs are built from a raw/mmc image converted by `haiku-on-ec2`'s
`scripts/make-gpt-image.sh` (GPT + its own protective MBR), **not** from the anyboot
image. So this anyboot fix is for USB/CD/local hybrid boot and build-completeness; it
is likely **not** on the EC2 boot path. Confirm during testing whether EC2 needs it at
all before prioritizing.

## Test plan (must pass before applying)

1. Build `@nightly-anyboot` for arm64 with the proposed fix on a builder.
2. `hexdump -C haiku-nightly-anyboot.iso | head` — verify sector 0 has: partition entry
   with type `0xef` at the ESP offset, and `55 aa` at bytes 510-511.
3. Boot the image under UEFI (QEMU aarch64 + EDK2, or convert + launch on EC2) and
   confirm the EFI loader is found and Haiku boots.
4. Compare against the current EC2 (`make-gpt-image.sh`) image to document whether the
   anyboot artifact is needed for EC2 or only for USB/CD.
5. Record results (hexdump + boot outcome) below.

## ~~Separate blocker~~ RESOLVED 2026-08-24 (full @nightly-anyboot, unrelated to the MBR)

> **FIXED and merged: `949bac43cc` "anyboot/cd: build a pure-UEFI El Torito ISO on
> EFI-only arches".**
>
> The diagnosis below is right about the numbers and wrong about what they mean. The
> boot floppy is **only the BIOS El Torito image** embedded in the CD/anyboot ISO —
> and **arm64 has no BIOS**; it boots via the EFI System Partition, so the floppy is
> **vestigial** there, exactly parallel to the x86 MBR boot code this very document
> is about. The fix is therefore not "make the loader smaller" or "grow the floppy
> budget": it is to stop building a BIOS artefact on an arch that has no BIOS. The
> floppy is passed only on x86/x86_64; elsewhere `-b`/`-eltorito-alt-boot` are
> omitted and a pure-UEFI El Torito ISO is built with `-no-emul-boot -e esp.image`.
>
> Validated: `haiku-boot-cd.iso` builds with the loader in the ESP
> (`/EFI/BOOT/BOOTAA64.EFI`) and no BIOS boot entry.
>
> **The generalisable point, and it is the same one twice in one file:** two separate
> arm64 image failures both turned out to be *x86 boot artefacts being built for an
> arch that cannot use them*. When an image step fails on arm64 with a size or layout
> budget, ask first whether the artefact should exist at all.

As it stood, with the MBR fix in place, the full `@nightly-anyboot` still failed
earlier in image assembly, on arm64, independent of this change:

    BuildFloppyBootImage1 haiku-boot-floppy.image
    haiku_loader.efi is too big (427627) to fit before the boot archive starting at 196608

The arm64 `haiku_loader.efi` (~427 KB) exceeds the boot-floppy layout budget (offset
196608 = 192 KB). ~~This is its own issue (loader size / floppy layout on arm64) and
must be addressed separately before a full anyboot image can be produced.~~ It was
its own issue, and it was addressed — but **not** as a loader-size or floppy-layout
problem. See the banner above.

## Results (build-validated 2026-08-21)

- `base_mbr.bin` builds via `CreateSignedEmptyMBR` — no `-m32`, `jam` exit 0.
- `hexdump -C base_mbr.bin`: exactly 512 bytes, all zero except the signature:

      000001f0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 55 aa
      00000200

- Running the built `anyboot` host tool with the new MBR + dummy parts and hexdumping
  sector 0 confirmed the overlay works and the signature is preserved:
  partition entry 0 type `0xeb` (offset 450), entry 1 type `0xef` (offset 466, EFI
  System Partition), `55 aa` at 510-511. Boot-code area (0-445) all zero.
- UEFI boot-test: **still to do as far as I can establish — UNVERIFIED as of
  2026-08-24** (QEMU aarch64 + EDK2, or EC2). ~~Gated on the separate
  `haiku_loader.efi`-too-big issue above for a full image~~ — **that gate is
  removed** (`949bac43cc`), so this is now actionable. Caveat from elsewhere in this
  tree: **a QEMU pass is not an EC2 pass** — the GED-vs-PL061 power-button and the
  PL011-vs-16550 serial divergences both bit this project. Prefer EC2 for the
  boot-test, or treat a QEMU result as provisional.
