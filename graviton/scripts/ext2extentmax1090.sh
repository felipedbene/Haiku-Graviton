#!/bin/sh
#
# ext2extentmax1090.sh -- issue 1090: files whose extents reach 32768 blocks
#
# Run on Haiku with the data volume of graviton/evidence/1090/*_seed3.log attached (plus
# *_seed3b*/seed3c* for the crafted files); then check it on Linux with e2fsck -fn and the
# md5s that log records (graviton/evidence/1090/*linux-check.log shows the check).
# step 4 (#1090), arm <arm>: on x4k (flex_bg: whole free groups of 32768 blocks), write a
# 200 MiB file in 1 MiB steps (extents grow to 32768 blocks), ftruncate a file up to 300 MiB
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
BOOT=$(df /boot | awk '/Device:/ { print $2 }')
DD=""
for d in /dev/disk/nvme/*; do case "$BOOT" in $d/*) continue;; esac; [ -e $d/1 ] && DD=$d; done
[ -n "$DD" ] || { echo "no data disk"; exit 1; }
echo "boot $BOOT, data disk $DD: $(ls $DD | tr '\n' ' ')"
mkdir -p /ex4k /ex1k
# (one large allocation), and cut the Linux-written l200 to 100 MiB and append 1 MiB to it.
mount -t ext2 $DD/0 /ex4k || exit 1
cd /ex4k
dd if=/dev/urandom of=h200 bs=1048576 count=200 2>&1 | tail -1; echo "h200 rc=$?"
echo "h200 md5 $(md5sum < h200)"
truncate -s 300M ht; echo "truncate ht rc=$?"
truncate -s 100M l200; echo "truncate l200 rc=$?"
cat /boot/home/c1m >> l200; echo "l200 append rc=$?"
sync; ls -ls h200 ht l200; cd /
unmount /ex4k; echo "unmount x4k rc=$?"
