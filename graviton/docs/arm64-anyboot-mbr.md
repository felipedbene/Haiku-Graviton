# arm64 `@nightly-anyboot`: the `base_mbr.bin` / `-m32` problem

Status: **investigated, fix proposed, NOT yet tested.** Do not apply the fix until
the boot test below passes — the MBR is load-bearing for UEFI partition discovery.

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
    printf '\x55\xAA' | dd of=$(1) bs=1 seek=510 conv=notrunc 2>/dev/null
}
```

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

## Results

_(pending test)_
