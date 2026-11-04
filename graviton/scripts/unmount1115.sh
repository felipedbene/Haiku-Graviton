#!/bin/bash
#
# unmount1115 -- can an NTFS volume be unmounted after directory and file
# creation? (issue 1115)
#
# fs_create_dir() published the new directory's vnode and never put the
# reference, so after one mkdir() the volume stayed busy forever:
# "fs_unmount(): inode N is still referenced". This runs CYCLES rounds of
#   mount -> one workload -> unmount -> (next round remounts)
# on one device, so a leak in any round fails that round's unmount and every
# later mount. Workloads: nothing, mkdir, mkdir + rmdir, create, create +
# unlink, nested mkdir -p + files, rename of a file and of a directory, a
# symlink and a hard link (the driver implements neither; their rc is
# reported, not judged), and cp -r of a small tree. After the last round the
# volume is remounted once more and the surviving tree is checked by sha256.
#
# An unmount right after a mount can be briefly busy on any diskimage volume
# (seen on BFS too), so each unmount is retried up to 5 times, 2 s apart; a
# leaked reference never clears and fails all 5.
#
# Usage: unmount1115.sh <device> <mount point> [cycles [tag]]   (as root)
# Exit status: the number of failed unmounts (capped at 255).

DEV=$1
MNT=$2
CYCLES=${3:-3}
# Names carry a per-run tag, so a second run on the same volume never renames
# onto a name an earlier run left behind (that is #1118, a separate KDL).
P=${4:-p}
[ -n "$DEV" ] && [ -n "$MNT" ] || { echo "usage: $0 device mountpoint [cycles]"; exit 2; }

fails=0
rounds=0

do_unmount() {	# do_unmount <label>
	cd /
	for try in 1 2 3 4 5; do
		if unmount "$MNT" 2> /dev/null; then
			echo "PASS unmount after [$1] (attempt $try)"
			return 0
		fi
		sleep 2
	done
	echo "FAIL unmount after [$1]: still busy after 5 attempts"
	fails=$((fails + 1))
	return 1
}

do_mount() {
	mkdir -p "$MNT"
	mount -t ntfs "$DEV" "$MNT" 2> /dev/null && return 0
	echo "FAIL mount for [$1]: $(mount -t ntfs "$DEV" "$MNT" 2>&1)"
	return 1
}

round() {	# round <label> <commands>
	rounds=$((rounds + 1))
	do_mount "$1" || { fails=$((fails + 1)); return; }
	(cd "$MNT" && eval "$2") > /tmp/unmount1115.out 2>&1
	echo "     [$1] workload rc=$?$(tr '\n' ' ' < /tmp/unmount1115.out | cut -c1-160 | sed 's/^./: &/')"
	do_unmount "$1"
}

for c in $(seq 1 "$CYCLES"); do
	echo "== cycle $c"
	round "c$c idle" 'true'
	round "c$c mkdir" "mkdir ${P}d$c"
	round "c$c mkdir+rmdir" "mkdir ${P}t$c && rmdir ${P}t$c"
	round "c$c create" "echo ${P}f$c > ${P}f$c"
	round "c$c create+unlink" "echo u > ${P}u$c && rm ${P}u$c"
	round "c$c mkdir -p + files" "mkdir -p ${P}n$c/a/b && echo 1 > ${P}n$c/a/one && echo 2 > ${P}n$c/a/b/two"
	round "c$c rename file" "echo r > ${P}r$c && mv ${P}r$c ${P}rr$c"
	round "c$c rename dir" "mkdir ${P}rd$c && mv ${P}rd$c ${P}rrd$c && mv ${P}rrd$c ${P}n$c/a/"
	round "c$c symlink (unsupported)" "ln -s ${P}f$c ${P}s$c; echo ln-s-rc=\$?"
	round "c$c hard link (unsupported)" "ln ${P}f$c ${P}l$c; echo ln-rc=\$?"
	round "c$c cp -r tree" "cp -r ${P}n$c ${P}cp$c"
done

echo "== final check"
if do_mount final; then
	bad=0
	for c in $(seq 1 "$CYCLES"); do
		[ -d "$MNT/${P}d$c" ] && [ ! -e "$MNT/${P}t$c" ] && [ ! -e "$MNT/${P}u$c" ] \
			&& [ "$(cat "$MNT/${P}f$c")" = "${P}f$c" ] \
			&& [ "$(cat "$MNT/${P}rr$c")" = r ] && [ ! -e "$MNT/${P}r$c" ] \
			&& [ -d "$MNT/${P}n$c/a/${P}rrd$c" ] \
			&& [ "$(cd "$MNT/${P}n$c" && find . -type f | sort | xargs sha256sum)" \
				= "$(cd "$MNT/${P}cp$c" && find . -type f | sort | xargs sha256sum)" ] \
			|| { echo "FAIL tree of cycle $c is not as written"; bad=1; }
	done
	[ $bad = 0 ] && echo "PASS every cycle's tree survived the remounts"
	fails=$((fails + bad))
	do_unmount final
else
	fails=$((fails + 1))
fi

echo "SUMMARY rounds=$rounds failures=$fails"
[ $fails -gt 255 ] && fails=255
exit $fails
