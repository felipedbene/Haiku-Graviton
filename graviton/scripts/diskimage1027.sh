#!/bin/sh
#
# diskimage1027.sh -- file data on a volume backed by a diskimage-registered file
# (issues 1026 and 1027).
#
# On a kernel without the #1027 fix, every page write-back to such a volume fails
# ("nvme_disk: failed to lock memory: Bad address"), so file data never reaches
# the image; on one without the #1026 fix, unmounting a FAT volume with a page
# still modified then KDLs ("acquire_vnode(): node wasn't used!").
#
# Run the phases as separate commands: a register and a mount in one SSM command
# have failed with ENOENT before (#1013).
#   register <image>...          diskimage register each image
#   write <dev> <mnt> <fstype>   mount; md5 the Linux-written files lin-1m and
#                                lin-small; write h-2b (2 B), h-4k (4 KiB) and
#                                h-1m (1 MiB, random); sync; wait 30 s; count
#                                the write-back failures the kernel logged
#   unmount <mnt>                unmount (does not return on a KDL)
#   verify <dev> <mnt> <fstype>  mount again and md5 every file from the disk
#                                (the cache went away with the unmount)
#   unmount <mnt>                ...and unmount again
# Expected after both fixes: no failures logged, the unmount returns, and every
# md5 in "verify" matches the one recorded in "write" (and Linux's, for lin-*).

state=/boot/home/d1027
mkdir -p $state

failures()
{
	grep -c "failed to lock memory" /var/log/syslog
}

case "$1" in
	register)
		shift
		for image in "$@"; do
			diskimage register "$image" || exit 1
		done
		diskimage list
		;;
	write)
		dev=$2; mnt=$3; fs=$4
		before=$(failures)
		mkdir -p $mnt && mount -t $fs $dev $mnt || exit 1
		md5sum $mnt/lin-1m $mnt/lin-small
		printf 'x\n' > $mnt/h-2b
		dd if=/dev/zero bs=4096 count=1 2>/dev/null | tr '\0' 'k' > $mnt/h-4k
		dd if=/dev/urandom of=$mnt/h-1m bs=65536 count=16 2>/dev/null
		(cd $mnt && md5sum h-2b h-4k h-1m) > $state/$fs.md5
		cat $state/$fs.md5
		sync
		sleep 30
		echo "write-back failures logged during this phase: $(( $(failures) - before ))"
		;;
	unmount)
		unmount $2
		echo "unmount $2 rc=$?"
		;;
	verify)
		dev=$2; mnt=$3; fs=$4
		mkdir -p $mnt && mount -t $fs $dev $mnt || exit 1
		md5sum $mnt/lin-1m $mnt/lin-small
		(cd $mnt && md5sum -c $state/$fs.md5)
		echo "verify rc=$?"
		;;
	*)
		echo "usage: $0 register|write|unmount|verify ..." >&2
		exit 2
		;;
esac
