#!/usr/bin/env bash
# Turn Haiku's build output (haiku-mmc.image) into a raw disk image that the
# Haiku EFI loader will boot unattended, sized for an EBS volume.
#
#   ./make-gpt-image.sh <mmc-image> <output-image> [size-bytes]
#
# Run this on the builder (it needs sgdisk); feed the output to make-ami.sh.
#
# Why this script exists rather than a hand-rolled sgdisk session:
#
# The loader's EFI backend only accepts a boot partition whose *GPT partition
# type GUID* is Haiku's BFS GUID -- see device_contains_partition() in
# src/system/boot/platform/efi/devices.cpp, which walks kTypeMap looking for
# BFS_NAME. Any other type (notably EBD0A0A2-... "Microsoft basic data", which
# is what most partitioning tools default to) makes
# platform_get_boot_partitions() return B_ENTRY_NOT_FOUND. The loader then has
# no boot volume, so it drops to the menu showing
# "Select boot volume/state (Current: None)" and waits forever -- on EC2 that
# looks like a hang on the serial console.
#
# haiku-mmc.image is MBR-only: partition 2 has MBR type 0xEB (BeOS fs). sgdisk
# knows that type and maps it to Haiku's BFS GUID when it converts, so a plain
# `sgdisk -g` produces exactly the GPT the loader wants. Do NOT partition the
# disk from scratch -- that is how the 0700 image that hangs at the menu was
# produced.
set -euo pipefail

MMC="${1:?path to haiku-mmc.image}"
OUT="${2:?output raw image path}"
SIZE="${3:-2147483648}"		# 2 GiB, matches the AMI's root volume

HAIKU_BFS_GUID=42465331-3BA3-10F1-802A-4861696B7521

command -v sgdisk >/dev/null || { echo "sgdisk not found (apt install gdisk)" >&2; exit 1; }

MMC_SIZE=$(stat -c %s "$MMC")
[ "$SIZE" -ge "$MMC_SIZE" ] || { echo "target size $SIZE < image size $MMC_SIZE" >&2; exit 1; }

echo "==> copying $MMC -> $OUT and growing to $SIZE bytes"
cp "$MMC" "$OUT"
truncate -s "$SIZE" "$OUT"

# The build output is MBR-only, so convert. If a GPT is already present (e.g.
# this is being re-run on a converted image), just move the backup header to
# the new last sector -- growing the file strands it mid-disk. Either way the
# partition entries are derived, not authored, so the type GUIDs survive.
if sgdisk -p "$OUT" 2>&1 | grep -q "Found invalid GPT and valid MBR"; then
	echo "==> converting MBR to GPT"
	sgdisk -g "$OUT" >/dev/null
else
	echo "==> relocating backup GPT header to end of disk"
	sgdisk -e "$OUT" >/dev/null
fi

# Belt and braces: assert the boot partition still has the GUID the loader
# needs. If a future change to the image build loses it, fail loudly here
# rather than at the loader menu 20 minutes and one AMI import later.
echo "==> verifying boot partition type GUID"
ACTUAL=$(sgdisk -i 2 "$OUT" | sed -n 's/^Partition GUID code: \([0-9A-Fa-f-]*\).*/\1/p')
if [ "${ACTUAL^^}" != "$HAIKU_BFS_GUID" ]; then
	echo "    partition 2 is $ACTUAL, expected Haiku BFS $HAIKU_BFS_GUID -- fixing"
	sgdisk -t "2:$HAIKU_BFS_GUID" "$OUT" >/dev/null
	ACTUAL=$(sgdisk -i 2 "$OUT" | sed -n 's/^Partition GUID code: \([0-9A-Fa-f-]*\).*/\1/p')
	[ "${ACTUAL^^}" = "$HAIKU_BFS_GUID" ] || { echo "could not set type GUID" >&2; exit 1; }
fi

sgdisk -p "$OUT" | tail -5
echo "==> $OUT ready"
