#!/bin/sh
#
# ext2prealloc1088.sh -- issue 1088: read and write Linux-fallocated (uninitialized) extents
#
# Run on Haiku with the data volume of graviton/evidence/1090/*_seed3.log attached (plus
# *_seed3b*/seed3c* for the crafted files); then check it on Linux with e2fsck -fn and the
# md5s that log records (graviton/evidence/1090/*linux-check.log shows the check).
# step 2 (#1088), arm <arm>: on x4k, read the Linux-fallocated fa (16 MiB, over stale data:
# must read as zeros), then write 64 KiB into it at 1 MiB, and 256 KiB into fb at 0.
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
BOOT=$(df /boot | awk '/Device:/ { print $2 }')
DD=""
for d in /dev/disk/nvme/*; do case "$BOOT" in $d/*) continue;; esac; [ -e $d/1 ] && DD=$d; done
[ -n "$DD" ] || { echo "no data disk"; exit 1; }
echo "boot $BOOT, data disk $DD: $(ls $DD | tr '\n' ' ')"
mkdir -p /ex4k /ex1k
dd if=/dev/urandom of=/boot/home/c64k bs=65536 count=1 2>/dev/null
dd if=/dev/urandom of=/boot/home/c256k bs=65536 count=4 2>/dev/null
echo "c64k md5 $(md5sum < /boot/home/c64k)"; echo "c256k md5 $(md5sum < /boot/home/c256k)"
mount -t ext2 $DD/0 /ex4k || exit 1
cd /ex4k && ls -ls
echo "fa read md5 $(md5sum < fa)   (16 MiB of zeros: 2c7ab85a893283e98c931e9511add182)"
dd if=/boot/home/c64k of=fa bs=65536 seek=16 conv=notrunc 2>&1 | tail -1; echo "fa write rc=$?"
dd if=/boot/home/c256k of=fb bs=65536 conv=notrunc 2>&1 | tail -1; echo "fb write rc=$?"
sync; ls -ls fa fb; cd /
unmount /ex4k; echo "unmount x4k rc=$?"
