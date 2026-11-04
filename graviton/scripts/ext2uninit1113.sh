#!/bin/sh
#
# ext2uninit1113.sh -- issue 1113: the first allocation in a BLOCK_UNINIT ext4 group must not
# land on that group's superblock backup, descriptor table or other metadata.
#
# The data volume is a second EBS volume with four file systems made by Linux mke2fs
# (graviton/evidence/1113/*_seed1113.log), all with BLOCK_UNINIT groups:
#  p1 u4k: ext4, 4 KiB blocks, flex_bg, uninit_bg, 400 MiB (4 groups; the #1113 layout)
#  p2 u1k: ext4, 1 KiB blocks, flex_bg, uninit_bg, 64 MiB (8 groups)
#  p3 n1k: ext4, 1 KiB blocks, uninit_bg, no flex_bg, 64 MiB (8 groups; each group's bitmaps
#          and inode table are inside it, and groups 3, 5 and 7 hold backups)
#  p4 m4k: ext4, 4 KiB blocks, flex_bg, metadata_csum (which brings BLOCK_UNINIT), 256 MiB
# On each: fill with 1 MiB random chunks until ENOSPC, then with one-block files until one fails,
# md5 the fill, unmount and mount again (drops the caches), md5 it again, rm everything, mkdir and
# rmdir, unmount. Then mount once more: AllocationBlockGroup::Initialize() checks each group's
# free count against its bitmap.
# Expected after the fix: the two md5s agree, no "Mismatch" in the syslog, no KDL, and Linux
# e2fsck -fn clean on all four, on the primary superblock and on every backup.
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
DD=""
for d in /dev/disk/nvme/*; do [ -e $d/3 ] && [ ! -e $d/4 ] && DD=$d; done
[ -n "$DD" ] || { echo "no 4-partition data disk"; exit 1; }
dd if=/dev/urandom of=/boot/home/chunk1m bs=65536 count=16 2>/dev/null
n=0
for l in u4k u1k n1k m4k; do
	mkdir -p /v$l && mount -t ext2 $DD/$n /v$l || { echo "mount $l failed"; exit 1; }
	cd /v$l
	echo "=== $l"; df -k . | tail -1
	c=0
	while [ $c -lt 1000 ] && cat /boot/home/chunk1m >> fill 2>/dev/null; do c=$((c + 1)); done
	echo "1 MiB chunks: $c"
	ls -ls fill
	i=0
	while [ $i -lt 4000 ] && printf x > s$i 2>/dev/null; do i=$((i + 1)); done
	echo "one-block files: $i"; df -k . | tail -1
	sync
	m1=$(md5sum < fill); echo "fill md5 before remount $m1"
	cd /; unmount /v$l; echo "unmount $l rc=$?"
	mount -t ext2 $DD/$n /v$l; echo "remount $l rc=$?"
	cd /v$l
	m2=$(md5sum < fill); echo "fill md5 after remount $m2"
	[ "$m1" = "$m2" ] && echo "fill md5 $l: same" || echo "fill md5 $l: DIFFERENT"
	rm -f fill s*; echo "rm rc=$?"
	mkdir d; echo "mkdir after rm rc=$?"
	rmdir d; echo "rmdir rc=$?"
	ls -la; df -k . | tail -1
	sync; cd /
	unmount /v$l; echo "unmount $l rc=$?"
	n=$((n + 1))
done
sync
n=0
for l in u4k u1k n1k m4k; do
	mount -t ext2 $DD/$n /v$l; echo "final mount $l rc=$?"
	n=$((n + 1))
	touch /v$l/probe; echo "final mount $l write rc=$?"
	rm /v$l/probe
	unmount /v$l; echo "unmount $l rc=$?"
done
sync
grep -E "ext2|Mismatch|uninitialized group" /var/log/syslog | grep -v "disk system" | tail -40
