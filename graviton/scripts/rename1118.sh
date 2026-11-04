#!/bin/bash
#
# rename1118 -- rename over existing NTFS targets, and unlink/rmdir of a
# target whose vnode is not loaded (issue 1118). Each case runs in its own
# directory, and the volume is remounted where a case needs the target's vnode
# not to be loaded (a fresh mount loads nothing). After each case the
# directory is checked for the expected contents and for leftover temporary
# (".ntfs-3g-") or ghost (".ghost-ntfs-3g-") names.
#
# Before the fix, the first rename over an existing file KDLs the box with
# "ntfs_inode_close: N already in cache!", so on a "before" box only the first
# case listed ever reports.
#
# Usage: rename1118.sh <device> <mount point> <rename1118 binary> [case...]
#   cases: file mvfile20 remount dir dirfull across open unlink rmdir
#          (default: all, in that order)
# At the end: unmount, remount, check every case again, write a manifest
# (path, type, sha256) to /tmp/rename1118.manifest, unmount.
# Exit status: the number of failed checks (capped at 255).

DEV=$1
MNT=$2
BIN=$3
shift 3
CASES=${*:-file mvfile20 remount dir dirfull across open unlink rmdir}
[ -n "$DEV" ] && [ -n "$MNT" ] && [ -x "$BIN" ] \
	|| { echo "usage: $0 device mountpoint rename1118-binary [case...]"; exit 2; }
# absolute: a remount cd's to /
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

fails=0
check() {	# check <label> <condition...>
	if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}
leftovers() {	# leftovers <dir>: no temporary or ghost names
	! ls -a "$1" | grep -qE '\.ntfs-3g-|\.ghost-ntfs-3g-'
}
content() { [ "$(cat "$1" 2> /dev/null)" = "$2" ]; }

mnt() {
	mkdir -p "$MNT"
	mount -t ntfs "$DEV" "$MNT" || { echo "FAIL mount"; exit 255; }
}
umnt() {
	cd /
	for try in 1 2 3 4 5; do
		unmount "$MNT" 2> /dev/null && return 0
		sleep 2
	done
	echo "FAIL unmount: still busy after 5 attempts"
	fails=$((fails + 1))
	return 1
}
remount() { umnt && mnt; }

verify() {	# verify <case>: the state each case should leave
	local D=$MNT/r-$1
	case $1 in
	file)		check "file: b holds a's data, a is gone, no leftovers" \
					'content $D/b A && [ ! -e $D/a ] && leftovers $D' ;;
	mvfile20)	check "mvfile20: 20 renames over an existing file, last data kept" \
					'content $D/t 20 && [ $(ls -A $D | wc -l) = 1 ]' ;;
	remount)	check "remount: rename() over a file whose vnode was not loaded" \
					'content $D/b A && [ ! -e $D/a ] && leftovers $D' ;;
	dir)		check "dir: d1 renamed over the empty d2, its file inside" \
					'[ ! -e $D/d1 ] && content $D/d2/x X && leftovers $D' ;;
	dirfull)	check "dirfull: rename over a non-empty directory refused, both intact" \
					'content $D/d1/x X && content $D/d2/y Y && leftovers $D' ;;
	across)		check "across: x/a renamed over y/b" \
					'[ ! -e $D/x/a ] && content $D/y/b A && leftovers $D/x && leftovers $D/y' ;;
	open)		check "open: b renamed over while open; the name now holds a's data" \
					'content $D/b A && [ ! -e $D/a ] && leftovers $D' ;;
	unlink)		check "unlink: unlink() of a file whose vnode was not loaded" \
					'[ ! -e $D/u ] && content $D/keep K && leftovers $D' ;;
	rmdir)		check "rmdir: rmdir() of a directory whose vnode was not loaded" \
					'[ ! -e $D/e ] && content $D/keep K && leftovers $D' ;;
	esac
}

run() {	# run <case>
	local D=$MNT/r-$1
	echo "== case $1"
	rm -rf "$D"; mkdir "$D" || { echo "FAIL $1: setup"; fails=$((fails + 1)); return; }
	case $1 in
	file)
		echo A > $D/a; echo B > $D/b
		mv $D/a $D/b; echo "mv rc=$?" ;;
	mvfile20)
		echo 0 > $D/t
		for i in $(seq 1 20); do echo $i > $D/n; mv $D/n $D/t || echo "mv $i failed"; done ;;
	remount)
		echo A > $D/a; echo B > $D/b
		remount && $BIN rename $D/a $D/b ;;
	dir)
		mkdir $D/d1 $D/d2; echo X > $D/d1/x
		remount && $BIN rename $D/d1 $D/d2 ;;
	dirfull)
		mkdir $D/d1 $D/d2; echo X > $D/d1/x; echo Y > $D/d2/y
		$BIN rename $D/d1 $D/d2; echo "(a refusal is expected)" ;;
	across)
		mkdir $D/x $D/y; echo A > $D/x/a; echo B > $D/y/b
		remount && $BIN rename $D/x/a $D/y/b ;;
	open)
		echo A > $D/a; echo B > $D/b
		$BIN open-rename $D/b $D/a $D/b ;;
	unlink)
		echo U > $D/u; echo K > $D/keep
		remount && $BIN unlink $D/u ;;
	rmdir)
		mkdir $D/e; echo K > $D/keep
		remount && $BIN rmdir $D/e ;;
	*)	echo "unknown case $1"; fails=$((fails + 1)); return ;;
	esac
	verify $1
}

mnt
for c in $CASES; do run $c; done
echo "== unmount, remount, check again"
if remount; then
	for c in $CASES; do verify $c; done
	(cd "$MNT" && find . -path ./System\ Volume\ Information -prune -o -print \
		| grep -v '^\.$' | sort | while read -r p; do
			if [ -d "$p" ]; then echo "d $p"; else echo "f $p $(sha256sum < "$p" | cut -c1-16)"; fi
		done) > /tmp/rename1118.manifest
	echo "manifest: $(wc -l < /tmp/rename1118.manifest) entries, sha256 $(sha256sum < /tmp/rename1118.manifest | cut -c1-16)"
	cat /tmp/rename1118.manifest
	umnt && echo "PASS final unmount"
fi
echo "SUMMARY failures=$fails"
[ $fails -gt 255 ] && fails=255
exit $fails
