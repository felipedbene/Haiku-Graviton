#!/bin/sh
#
# ext2abort1110.sh -- issue 1110: an aborted ext2 transaction must not leave stale group
# descriptors behind for a later transaction to write out.
#
# The data volume is a second EBS volume with four file systems made by Linux mke2fs
# (graviton/evidence/1110/*_seed.log): p1 "e1k" (ext2, 1 KiB blocks, 4 groups), p2 "x1k"
# (ext4, 1 KiB blocks, flex_bg, 8 groups), p3 "x4k" (ext4, 4 KiB blocks, flex_bg, 4 groups)
# and p4 "e4k" (ext2, 4 KiB blocks, one group: the control). p3 is made without uninit_bg:
# with it, the first allocation in a BLOCK_UNINIT group overwrites that group's backup
# superblock and the page writer panics (issue 1113), on either side of this fix.
# On each: write until ENOSPC (the failed write aborts), mkdir (InitDirectory runs out of
# space after the inode was allocated: abort), rm the fillers (writes the descriptor
# blocks), unmount. Then mount again, which re-checks every group's free block count against
# its bitmap, and make and remove a directory to show the volume is writable.
# Expected after the fix: ENOSPC for dd and mkdir, the remount writable, and Linux
# e2fsck -fn clean on all four.
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
DD=""
for d in /dev/disk/nvme/*; do [ -e $d/3 ] && [ ! -e $d/4 ] && DD=$d; done
[ -n "$DD" ] || { echo "no 4-partition data disk"; exit 1; }
n=0
for l in e1k x1k x4k e4k; do
	mkdir -p /v$l && mount -t ext2 $DD/$n /v$l || { echo "mount $l failed"; exit 1; }
	n=$((n + 1))
	cd /v$l
	echo "=== $l"; df -k . | tail -1
	dd if=/dev/zero of=fill bs=65536 count=8000 2>&1 | tail -3; echo "dd fill rc=$?"
	ls -ls fill
	# A write that does not fit fails whole, so blocks can be left over: use them up
	# with one-block files until one fails.
	i=0
	while [ $i -lt 2000 ] && printf x > s$i 2>/dev/null; do i=$((i + 1)); done
	echo "one-block files: $i"; df -k . | tail -1
	mkdir d; echo "mkdir on full rc=$?"
	ls -ld d 2>&1
	mkdir d2; echo "mkdir again rc=$?"
	rm -f fill s*; echo "rm rc=$?"
	ls -la; df -k . | tail -1
	sync; cd /
	unmount /v$l; echo "unmount $l rc=$?"
done
sync
# Remount: AllocationBlockGroup::Initialize() compares each group's descriptor with its
# bitmap; a mismatch fails the mount or leaves it read-only.
n=0
for l in e1k x1k x4k e4k; do
	mount -t ext2 $DD/$n /v$l; echo "remount $l rc=$?"
	n=$((n + 1))
	mkdir /v$l/probe; echo "remount $l mkdir rc=$?"
	rmdir /v$l/probe; echo "remount $l rmdir rc=$?"
	unmount /v$l; echo "unmount $l rc=$?"
done
sync
grep -E "ext2|Mismatch" /var/log/syslog | tail -40
