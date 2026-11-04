#!/bin/sh
#
# ext2pasteof1089.sh -- issue 1089: grow and remove files with extents preallocated past EOF
#
# Run on Haiku with the data volume of graviton/evidence/1090/*_seed3.log attached (plus
# *_seed3b*/seed3c* for the crafted files); then check it on Linux with e2fsck -fn and the
# md5s that log records (graviton/evidence/1090/*linux-check.log shows the check).
# step 3 (#1089), arm <arm>: on x4k, files with extents preallocated past EOF (fallocate -n):
# append 2 MiB to pn (prealloc right after EOF), remove the empty pz, append 1 MiB to pg
set -x
uname -a
sha256sum /boot/system/kernel_arm64 /boot/system/add-ons/kernel/file_systems/ext2
BOOT=$(df /boot | awk '/Device:/ { print $2 }')
DD=""
for d in /dev/disk/nvme/*; do case "$BOOT" in $d/*) continue;; esac; [ -e $d/1 ] && DD=$d; done
[ -n "$DD" ] || { echo "no data disk"; exit 1; }
echo "boot $BOOT, data disk $DD: $(ls $DD | tr '\n' ' ')"
mkdir -p /ex4k /ex1k
# (prealloc after a gap).
dd if=/dev/urandom of=/boot/home/c2m bs=65536 count=32 2>/dev/null
dd if=/dev/urandom of=/boot/home/c1m bs=65536 count=16 2>/dev/null
echo "c2m md5 $(md5sum < /boot/home/c2m)"; echo "c1m md5 $(md5sum < /boot/home/c1m)"
mount -t ext2 $DD/0 /ex4k || exit 1
cd /ex4k && ls -ls pn pz pg
cat /boot/home/c2m >> pn; echo "pn append rc=$?"
rm pz; echo "rm pz rc=$?"
cat /boot/home/c1m >> pg; echo "pg append rc=$?"
sync; ls -ls pn pg; cd /
unmount /ex4k; echo "unmount x4k rc=$?"

# --- the file-map case: pm, one preallocated extent across EOF, read and then written at 512 KiB ---
dd if=/dev/urandom of=/boot/home/c64m bs=65536 count=1 2>/dev/null
echo "c64m md5 $(md5sum < /boot/home/c64m)"
mount -t ext2 $DD/0 /ex4k || exit 1
cd /ex4k && ls -ls pm
echo "pm read md5 $(md5sum < pm)  (6 KiB of zeros: $(head -c 6144 /dev/zero | md5sum))"
dd if=/boot/home/c64m of=pm bs=65536 seek=8 conv=notrunc 2>&1 | tail -1; echo "pm write rc=$?"
sync; ls -ls pm; cd /
unmount /ex4k; echo "unmount x4k rc=$?"
