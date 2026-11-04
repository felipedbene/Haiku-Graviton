#!/bin/sh
#
# ext2groupfree1095.sh -- issue 1095: remove Linux files whose one extent crosses a block group boundary
#
# Run on Haiku with the data volume of graviton/evidence/1090/*_seed3.log attached (plus
# *_seed3b*/seed3c* for the crafted files); then check it on Linux with e2fsck -fn and the
# md5s that log records (graviton/evidence/1090/*linux-check.log shows the check).
# step 1 (#1095), arm <arm>: on x1k (ext4, 1 KiB blocks), remove gx, whose one extent crosses
# from group 10 into 11 and ends on group 11's last block, and gy (crosses, ends mid-group).
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
BOOT=$(df /boot | awk '/Device:/ { print $2 }')
DD=""
for d in /dev/disk/nvme/*; do case "$BOOT" in $d/*) continue;; esac; [ -e $d/1 ] && DD=$d; done
[ -n "$DD" ] || { echo "no data disk"; exit 1; }
echo "boot $BOOT, data disk $DD: $(ls $DD | tr '\n' ' ')"
mkdir -p /ex4k /ex1k
mount -t ext2 $DD/1 /ex1k || exit 1
cd /ex1k && ls -ls
echo "gx md5 $(md5sum < gx)"
rm gx gy; echo "rm rc=$?"
ls -ls; df -k . | tail -1; sync; cd /
unmount /ex1k; echo "unmount x1k rc=$?"
