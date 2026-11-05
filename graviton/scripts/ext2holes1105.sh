#!/bin/sh
#
# ext2holes1105.sh -- issue 1105: write into the holes of sparse files made by Linux.
#
# Run on Haiku with the data volume of graviton/evidence/1105/*_seed4*.log attached (the
# volume layout and the files are described there); then check it on Linux with e2fsck -fn
# and the region md5s (graviton/evidence/1105/*linux-check.log shows the check). Steps:
#   th: ext4 th (truncate -s hole 1..6 MiB): 256 KiB at 3 MiB, 128 KiB at 6 MiB - 64 KiB (hole into data)
#   ph: ext4 ph (punched hole 2..4 MiB): 128 KiB at 3 MiB
#   tf: ext4 tf (4 extents: full root in the inode): 64 KiB at 1 MiB
#   ti: ext2 ti (indirect; holes with missing indirect blocks): 64 KiB at 32 KiB (direct into indirect range), 64 KiB at 20 MiB (double indirect range)
#   pi: ext2 pi (punched hole 2..4 MiB, if Linux made one): 128 KiB at 3 MiB
#   tk: ext4 1 KiB blocks tk (hole past 64 KiB): 1 KiB at 1 MiB + 512 (part of a page)
#   ta: ext4 ta (100 bytes, its only block a hole): append 128 KiB
#   tb: ext2 tb (100 bytes, its only block a hole): append 128 KiB
#   tl: ext4 tl (depth-1 tree, index key 256): 64 KiB at 0, into the leading hole
#   tz: ext4 tz (1 MiB hole, over stale free blocks): 10 bytes at 8192, without reading first
#   tu: ext4 tu (6000 bytes, all hole): ftruncate to 10000
#   rd: read every test file back on Haiku (holes read as zeros)

step() {
	case $1 in
	th)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		dd if=/boot/home/c256k of=th bs=262144 seek=12 conv=notrunc 2>&1 | tail -1; echo "th write1 rc=$?"
		dd if=/boot/home/c128k of=th bs=65536 seek=95 conv=notrunc 2>&1 | tail -1; echo "th write2 rc=$?"
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	ph)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		dd if=/boot/home/c128k of=ph bs=65536 seek=48 conv=notrunc 2>&1 | tail -1; echo "ph write rc=$?"
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	tf)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		dd if=/boot/home/c64k of=tf bs=65536 seek=16 conv=notrunc 2>&1 | tail -1; echo "tf write rc=$?"
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		;;
	ti)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/1 /he4k || exit 1
		cd /he4k && ls -ls
		dd if=/boot/home/c64k of=ti bs=4096 seek=8 conv=notrunc 2>&1 | tail -1; echo "ti write1 rc=$?"
		dd if=/boot/home/c64k of=ti bs=65536 seek=320 conv=notrunc 2>&1 | tail -1; echo "ti write2 rc=$?"
		sync; cd /; unmount /he4k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	pi)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/1 /he4k || exit 1
		cd /he4k && ls -ls
		dd if=/boot/home/c128k of=pi bs=65536 seek=48 conv=notrunc 2>&1 | tail -1; echo "pi write rc=$?"
		sync; cd /; unmount /he4k; echo "unmount rc=$?"
		;;
	tk)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/2 /hx1k || exit 1
		cd /hx1k && ls -ls
		dd if=/boot/home/c1k of=tk bs=512 seek=2049 conv=notrunc 2>&1 | tail -1; echo "tk write rc=$?"
		sync; cd /; unmount /hx1k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	ta)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		echo "c128k [0,3996) md5 $(head -c 3996 /boot/home/c128k | md5sum)  [3996,end) md5 $(tail -c +3997 /boot/home/c128k | md5sum)"
		cat /boot/home/c128k >> ta; echo "ta append rc=$?"
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	tb)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/1 /he4k || exit 1
		cd /he4k && ls -ls
		echo "c128k [0,3996) md5 $(head -c 3996 /boot/home/c128k | md5sum)  [3996,end) md5 $(tail -c +3997 /boot/home/c128k | md5sum)"
		cat /boot/home/c128k >> tb; echo "tb append rc=$?"
		sync; cd /; unmount /he4k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	tl)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		dd if=/boot/home/c64k of=tl bs=65536 conv=notrunc 2>&1 | tail -1; echo "tl write rc=$?"
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		;;
	tz)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		dd if=/boot/home/c1k of=tz bs=1 count=10 seek=8192 conv=notrunc 2>&1 | tail -1; echo "tz write rc=$?"
		echo "c1k first 10 bytes md5 $(head -c 10 /boot/home/c1k | md5sum)"
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		echo "lock-memory failures during this step (#1069): $(( $(grep -c 'failed to lock memory' /var/log/syslog) - lockbefore ))"
		;;
	tu)
		lockbefore=$(grep -c "failed to lock memory" /var/log/syslog)
		mount -t ext2 $DD/0 /hx4k || exit 1
		cd /hx4k && ls -ls
		truncate -s 10000 tu; echo "tu truncate rc=$?"; ls -ls tu
		sync; cd /; unmount /hx4k; echo "unmount rc=$?"
		;;
	rd)
		mount -t ext2 $DD/0 /hx4k || exit 1
		echo "th md5 $(md5sum < /hx4k/th)"
		echo "ta md5 $(md5sum < /hx4k/ta)"
		echo "tl md5 $(md5sum < /hx4k/tl)"
		echo "ph md5 $(md5sum < /hx4k/ph)"
		echo "tf md5 $(md5sum < /hx4k/tf)"
		unmount /hx4k
		mount -t ext2 $DD/1 /he4k || exit 1
		echo "ti md5 $(md5sum < /he4k/ti)"
		echo "tb md5 $(md5sum < /he4k/tb)"
		echo "pi md5 $(md5sum < /he4k/pi)"
		unmount /he4k
		mount -t ext2 $DD/2 /hx1k || exit 1
		echo "tk md5 $(md5sum < /hx1k/tk)"
		unmount /hx1k
		;;
	esac
}

uname -a
sha256sum /boot/system/add-ons/kernel/file_systems/ext2
BOOT=$(df /boot | awk '/Device:/ { print $2 }')
DD=""
for d in /dev/disk/nvme/*; do case "$BOOT" in $d/*) continue;; esac; [ -e $d/2 ] && DD=$d; done
[ -n "$DD" ] || { echo "no data disk"; exit 1; }
mkdir -p /hx4k /he4k /hx1k
[ -f /boot/home/c256k ] || { dd if=/dev/urandom of=/boot/home/c256k bs=65536 count=4; dd if=/dev/urandom of=/boot/home/c128k bs=65536 count=2; dd if=/dev/urandom of=/boot/home/c64k bs=65536 count=1; dd if=/dev/urandom of=/boot/home/c1k bs=1024 count=1; } 2>/dev/null
for c in c256k c128k c64k c1k; do echo "$c md5 $(md5sum < /boot/home/$c)"; done
for s in ${@:-th ph tf ti pi tk ta tb tl tz tu rd}; do step $s; done
