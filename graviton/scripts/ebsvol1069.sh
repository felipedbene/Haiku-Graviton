#!/bin/sh
#
# ebsvol1069.sh -- single-page file I/O on a plain second EBS NVMe volume
# (issue 1069; first seen in #1064's runs).
#
# On a kernel without the #1069 fix, single-page file data on an ext2 data volume
# never reaches the disk ("nvme_disk: failed to lock memory: Bad address"), and a
# single-page read returns garbage: devfs hands nvme_disk a page mapped with
# vm_get_physical_page(), and lock_memory_etc() finds no area behind it. No
# diskimage or FileDevice is involved. The boot volume is checked too.
#
# Run the phases as separate commands (a mount right after a write to the raw
# device in one SSM command is asking for trouble):
#   seed <dev> <ext2.img>      write a Linux-made ext2 image onto the volume
#   write <dev> <mnt> <fs>     mount; md5 the Linux-written lin-1m and lin-small;
#                              write h-2b, h-3b, h-4k (single page) and h-1m;
#                              sync; wait 30 s; count the lock failures logged
#   unmount <mnt>
#   verify <dev> <mnt> <fs>    mount again, md5 every file from the disk
#   dump <dev> <file>          copy the first 256 MiB of the volume to a file,
#                              for a Linux fsck and md5 cross-check
#   mkbfs <dev>                initialise the volume as BFS (for a BFS round)
#   boot                       the same small writes on the boot volume, with
#                              the lock failures logged while they are synced
# Expected with the fix: no failures logged, and every md5 in "verify" (and on
# Linux) matches the one recorded in "write".

state=/boot/home/v1069
mkdir -p $state

failures()
{
	grep -c "failed to lock memory" /var/log/syslog
}

smallwrites()
{
	printf 'x\n' > $1/h-2b
	printf 'xyz' > $1/h-3b
	dd if=/dev/zero bs=4096 count=1 2>/dev/null | tr '\0' 'k' > $1/h-4k
	dd if=/dev/urandom of=$1/h-1m bs=65536 count=16 2>/dev/null
}

case "$1" in
	seed)
		dd if=$3 of=$2 bs=1048576 2>&1 | tail -1
		sync
		;;
	write)
		dev=$2; mnt=$3; fs=$4
		before=$(failures)
		mkdir -p $mnt && mount -t $fs $dev $mnt || exit 1
		[ -f $mnt/lin-small ] && md5sum $mnt/lin-1m $mnt/lin-small
		smallwrites $mnt
		(cd $mnt && md5sum h-2b h-3b h-4k h-1m) > $state/$fs.md5
		cat $state/$fs.md5
		sync
		sleep 30
		echo "lock failures logged during this phase: $(( $(failures) - before ))"
		;;
	unmount)
		unmount $2
		echo "unmount $2 rc=$?"
		;;
	verify)
		dev=$2; mnt=$3; fs=$4
		before=$(failures)
		mkdir -p $mnt && mount -t $fs $dev $mnt || exit 1
		[ -f $mnt/lin-small ] && md5sum $mnt/lin-1m $mnt/lin-small
		(cd $mnt && md5sum -c $state/$fs.md5)
		echo "verify rc=$?"
		echo "lock failures logged during this phase: $(( $(failures) - before ))"
		;;
	dump)
		dd if=$2 of=$3 bs=1048576 count=256 2>&1 | tail -1
		sha256sum $3
		;;
	mkbfs)
		mkfs -q -t bfs $2 v1069
		echo "mkfs rc=$?"
		;;
	boot)
		before=$(failures)
		dir=/boot/home/v1069/boot
		rm -rf $dir && mkdir -p $dir
		for i in 1 2 3 4 5 6 7 8; do
			mkdir -p $dir/$i && smallwrites $dir/$i
		done
		sync
		sleep 30
		(cd $dir && md5sum */h-2b */h-3b */h-4k | sort | head -6)
		echo "boot volume: lock failures logged while 32 small files were synced: $(( $(failures) - before ))"
		;;
	*)
		echo "usage: $0 seed|write|unmount|verify|dump|mkbfs|boot ..." >&2
		exit 2
		;;
esac
