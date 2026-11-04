#!/bin/sh
#
# ib1064.sh -- issue 1064: i_blocks written by the ext2 driver, over several
# write patterns. Run on Haiku against ext2 volumes made by Linux mkfs; then
# check them with e2fsck -fn on Linux (e2fsck recounts each inode's blocks).
#
# Usage: ib1064.sh <mountpoint> <pattern>...
# Patterns (each one a file, named after it, in <mountpoint>):
#   seq1m     16 x 64 KiB (the issue's dd): past the direct blocks, one indirect
#   seq8m     128 x 64 KiB: into the double indirect range on 4 KiB blocks
#   seq80m    80 x 1 MiB: into the triple indirect range on 1 KiB blocks
#   sparse    4 KiB at block 1000, then at 3000 (no truncation): the gaps are
#             filled with zeros by the driver
#   append    300 x 1000-byte O_APPEND writes (unaligned)
#   small     2 bytes, and 4 KiB
#   trunc     8 MiB, truncate to 100000 bytes, up to 3 MiB, then down to 0,
#             then 20 KiB more
#   tri-trunc 80 MiB, truncate to 70 MiB and then to 10 MiB (1 KiB blocks)
#   rm        8 MiB written, then removed (free count only)
# For each file it prints the size, st_blocks and the md5, for comparison
# with what Linux sees.

m=$1; shift
cd "$m" || exit 1
for p in "$@"; do
	case $p in
		seq1m)	dd if=/dev/urandom of=seq1m bs=65536 count=16 2>/dev/null ;;
		seq8m)	dd if=/dev/urandom of=seq8m bs=65536 count=128 2>/dev/null ;;
		seq80m)	dd if=/dev/urandom of=seq80m bs=1048576 count=80 2>/dev/null ;;
		sparse)	dd if=/dev/urandom of=sparse bs=4096 seek=1000 count=1 2>/dev/null
			dd if=/dev/urandom of=sparse bs=4096 seek=3000 count=1 conv=notrunc 2>/dev/null ;;
		append)	i=0; : > append
			while [ $i -lt 300 ]; do
				dd if=/dev/urandom bs=1000 count=1 2>/dev/null >> append
				i=$((i + 1))
			done ;;
		small)	printf 'x\n' > small2; dd if=/dev/urandom of=small4k bs=4096 count=1 2>/dev/null ;;
		trunc)	dd if=/dev/urandom of=trunc bs=65536 count=128 2>/dev/null
			truncate -s 100000 trunc; truncate -s 3M trunc; truncate -s 0 trunc
			dd if=/dev/urandom of=trunc bs=1024 count=20 conv=notrunc 2>/dev/null ;;
		tri-trunc) dd if=/dev/urandom of=tri-trunc bs=1048576 count=80 2>/dev/null
			truncate -s 70M tri-trunc; truncate -s 10M tri-trunc ;;
		rm)	dd if=/dev/urandom of=rmme bs=65536 count=128 2>/dev/null; sync; rm rmme ;;
		*)	echo "unknown pattern $p"; exit 2 ;;
	esac
	echo "pattern $p rc=$?"
done
sync
for f in *; do
	[ -f "$f" ] || continue
	printf '%-10s size=%-9s st_blocks=%-7s %s\n' "$f" "$(stat -c %s "$f")" \
		"$(stat -c %b "$f")" "$(md5sum < "$f" | cut -d' ' -f1)"
done
df -k . | tail -1
