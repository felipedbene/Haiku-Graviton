#!/bin/sh
#
# ext2sparse1080.sh -- issue 1080 (and the indirect-mapped case fixed with #1064):
# truncate, remove and append to sparse files that Linux made, on Haiku's ext2 driver.
#
# The data volume is laid out as in graviton/evidence/1080/*_seed2.log: p4 "lspx" (ext4,
# extents) holds xa/xrm (40 MiB, 4 KiB at 0, 5, 20, 39 MiB), xb (data at 4..6 MiB), xc
# (400 extents, a depth-1 tree), xd (as xa) and xe (8 MiB fallocate'd); p5 "lsp" (ext2,
# indirect) holds sa/srm (as xa). The seed log records the md5 each file must have after
# this script; check them, and e2fsck -fn, on Linux.
#
# p5 runs first: on a trunk-era driver the xa truncate to 3000 bytes never returns.
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
DD=""
for d in /dev/disk/nvme/*; do [ -e $d/4 ] && [ ! -e $d/5 ] && DD=$d; done
[ -n "$DD" ] || { echo "no 5-partition data disk"; exit 1; }
echo "data disk $DD: $(ls $DD | tr '\n' ' ')"
mkdir -p /elspx /elsp
mount -t ext2 $DD/3 /elspx || exit 1
mount -t ext2 $DD/4 /elsp || exit 1
echo "--- loaded ext2 image"; listimage 1 | grep ext2
dd if=/dev/urandom of=/boot/home/chunk128k bs=65536 count=2 2>/dev/null
echo "chunk128k md5 $(md5sum < /boot/home/chunk128k)"
echo "=== lsp (indirect)"
cd /elsp && ls -ls
truncate -s 22M sa; echo "sa truncate 22M rc=$?"
truncate -s 3000 sa; echo "sa truncate 3000 rc=$?"
rm srm; echo "rm srm rc=$?"
ls -ls; sync
echo "=== lspx (extents)"
cd /elspx && ls -ls
truncate -s 5M xb; echo "xb truncate 5M rc=$?"
truncate -s 13M xc; echo "xc truncate 13M rc=$?"
truncate -s 3M xe; echo "xe truncate 3M rc=$?"
cat /boot/home/chunk128k >> xd; echo "xd append 128K rc=$?"
sync
truncate -s 22M xa; echo "xa truncate 22M rc=$?"
truncate -s 3000 xa; echo "xa truncate 3000 rc=$?"
rm xrm; echo "rm xrm rc=$?"
ls -ls; sync
cd /
unmount /elsp; echo "unmount lsp rc=$?"
unmount /elspx; echo "unmount lspx rc=$?"
sync
