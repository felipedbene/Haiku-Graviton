#!/bin/sh
#
# ext2full1079.sh -- issue 1079: write until an ext2 volume is full, on Haiku.
#
# The data volume is laid out as in graviton/evidence/1080/*_seed2.log: p1 "full4k" (ext2,
# 4 KiB blocks), p2 "full1k" (ext2, 1 KiB blocks) and p3 "fullx" (ext4, extents), 32 MiB
# each. On each: write 64 MiB until ENOSPC, ftruncate a file up to 100 MiB (also ENOSPC),
# remove both and write 2 MiB again. Expected: ENOSPC and no KDL; then e2fsck -fn clean on
# Linux, and "after" with the md5 printed here.
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
DD=""
for d in /dev/disk/nvme/*; do [ -e $d/4 ] && [ ! -e $d/5 ] && DD=$d; done
[ -n "$DD" ] || { echo "no 5-partition data disk"; exit 1; }
dd if=/dev/urandom of=/boot/home/chunk2m bs=65536 count=32 2>/dev/null
echo "chunk2m md5 $(md5sum < /boot/home/chunk2m)"
n=0
for l in full4k full1k fullx; do
	mkdir -p /e$l && mount -t ext2 $DD/$n /e$l || { echo "mount $l failed"; exit 1; }
	n=$((n + 1))
	cd /e$l
	echo "=== $l"; df -k . | tail -1
	dd if=/dev/urandom of=fill bs=65536 count=1000 2>&1 | tail -3; echo "dd fill rc=$?"
	ls -ls fill; df -k . | tail -1
	echo "fill md5 $(md5sum < fill)"
	truncate -s 100M big; echo "truncate up 100M rc=$?"
	ls -ls big 2>&1
	sync
	rm -f fill big; echo "rm rc=$?"
	cat /boot/home/chunk2m > after; echo "write 2M after rm rc=$?"
	ls -ls; df -k . | tail -1
	sync; cd /
	unmount /e$l; echo "unmount $l rc=$?"
done
sync
