#!/bin/sh
#
# ext2lastgroup1087.sh -- issue 1087: allocations in a short last block group.
#
# Make the volume on Linux: a 32 MiB partition, mkfs.ext2 -q -F -b 1024 (4 groups; the last
# one is 8191 blocks long, not 8192). Then on Haiku:
#   ext2lastgroup1087.sh <device> <mountpoint>
# writes 25 MiB, enough to reach the last group, and unmounts. Check the volume with
# e2fsck -fn on Linux: with the bug, file data lands on the last group's bitmaps
# ("multiply-claimed ... <filesystem metadata>", random "Inode bitmap differences").
set -x
dev=$1; mnt=$2
mkdir -p $mnt && mount -t ext2 $dev $mnt || exit 1
dd if=/dev/urandom of=$mnt/fill bs=65536 count=400 2>&1 | tail -2; echo "dd fill rc=$?"
ls -ls $mnt/fill; sync
unmount $mnt; echo "unmount rc=$?"
