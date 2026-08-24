#!/bin/bash
# rebuild.sh <sshport> <port>... -- rebuild ports in one guest against the
# repaired, non-dirty chroot `haiku`.
#
# Differences from gworker.sh/cwork.sh, all of them needed by this particular job:
#
#  - It PUSHES the shared pool (/opt/haiku/hpkg-out/arm64-nondirty) into the guest
#    before each port, so a package rebuilt on any guest becomes available to all
#    of them. The chain is a spine plus leaves; without this, four guests cannot
#    cooperate on one dependency graph.
#  - It harvests into that pool AND into the canonical hpkg-out/arm64, with
#    `cp --remove-destination`, because the canonical file of the same name is a
#    pre-fix _dirty build that has to be replaced -- and a plain `cp -p` over a
#    hardlink would also rewrite the dirty snapshot kept for the record.
#  - It verifies each result twice: the hpkg exists (`ls`, never an exit code) and
#    the hpkg does NOT record a `_dirty` requirement. The second check is the whole
#    point of the rebuild, so it is not left to inference.
#  - It always passes -G (patch(1) instead of git) and keeps the FULL log.
PORT=${1:?usage: rebuild.sh <sshport> <port>...}; shift
KEY=/home/ubuntu/.ssh/haiku-ed25519
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30"
S="-n -p $PORT $OPTS -i $KEY"
SCP="-P $PORT $OPTS -i $KEY"
G=baron@127.0.0.1
GP=/boot/home/haikuports/packages
ISP=/boot/home/haikuports/input-source-packages
POOL=/opt/haiku/hpkg-out/arm64-nondirty
CANON=/opt/haiku/hpkg-out/arm64
S3=s3://haiku-graviton-668984504585-us-west-2/hpkg/arm64/
LOG=/opt/haiku/rebuild-$PORT.log
mkdir -p "$POOL" /opt/haiku/buildlogs
exec >> "$LOG" 2>&1

echo "=== REBUILD $PORT START $(date -u) : $* ==="
for p in "$@"; do
	echo "########## $p START $(date -u)"

	# 1. push every pool package the guest does not already have.
	have=$(ssh $S $G "ls $GP/ 2>/dev/null")
	pushed=0
	for f in "$POOL"/*.hpkg; do
		[ -e "$f" ] || continue
		b=$(basename "$f")
		printf '%s\n' "$have" | grep -qx "$b" && continue
		scp $SCP "$f" "$G:$GP/" >/dev/null 2>&1 && pushed=$((pushed+1))
	done
	echo "pool: pushed $pushed package(s)"

	# 2. haikuporter reuses a polluted work directory ("Skipping unpack of ..."),
	#    so a rebuild would measure the debris of the pre-fix build. A wedged one
	#    cannot be removed but can be renamed.
	ssh $S $G "for w in $ISP/develop/sources/$p-*/work-*; do
			[ -e \"\$w\" ] || continue
			rm -rf \"\$w\" 2>/dev/null || mv \"\$w\" \"\$w.wedged-\$\$\"
		done; echo 'work dirs cleared'"

	# 3. build. Full log kept in the guest and copied out -- never tail(1)ed here.
	ssh $S $G "haikuporter -y -G $p > /boot/home/r-$p.log 2>&1; echo RC=\$?"
	scp $SCP "$G:/boot/home/r-$p.log" "/opt/haiku/buildlogs/r-$p-$PORT.log" 2>/dev/null
	echo "log: $(wc -l < /opt/haiku/buildlogs/r-$p-$PORT.log 2>/dev/null) lines"

	# 4. verify by ls, not by exit code -- but exclude *_bootstrap*, or the check is
	#    a false positive for every port whose name also exists as a cross-built
	#    bootstrap package. `ls | grep '^m4[-_]'` matched m4-1.4.19_bootstrap and
	#    reported a successful m4 rebuild that had never happened.
	echo "---- hpkg check ----"
	ssh $S $G "ls $GP/ | grep -E \"^${p}[-_]\" | grep -v _bootstrap || echo NO_PKG_$p"

	# 5. verify the point of the exercise: no _dirty requirement anywhere.
	echo "---- dirty-requirement check (want 0 for each) ----"
	ssh $S $G "for f in $GP/${p}*.hpkg; do
			[ -e \"\$f\" ] || continue
			printf '%s %s\n' \"\$(basename \$f)\" \"\$(package list -i \"\$f\" 2>/dev/null | grep -c _dirty)\"
		done"

	# 6. harvest after EVERY port (a finished diffutils was lost to a guest swap
	#    once), excluding the chroot's own inputs. haiku*.hpkg especially: sweeping
	#    those into the guest seed directory is what made a stale, pre-clock-fix
	#    haiku.hpkg immortal for days.
	stage=$(mktemp -d)
	scp $SCP "$G:$GP/*.hpkg" "$stage/" >/dev/null 2>&1
	rm -f "$stage"/haiku.hpkg "$stage"/haiku_*.hpkg "$stage"/haiku-*.hpkg \
		"$stage"/*_bootstrap*.hpkg "$stage"/makefile_engine.hpkg \
		"$stage"/netfs.hpkg "$stage"/userland_fs.hpkg
	for f in "$stage"/*.hpkg; do
		[ -e "$f" ] || continue
		cp -p --remove-destination "$f" "$POOL"/
		cp -p --remove-destination "$f" "$CANON"/
	done
	rm -rf "$stage"
	aws s3 sync "$CANON"/ "$S3" --exclude "*" --include "*.hpkg" >/dev/null 2>&1
	echo "pool now: $(ls "$POOL"/*.hpkg 2>/dev/null | wc -l) hpkgs"
	echo "########## $p DONE $(date -u)"
done
echo "=== REBUILD $PORT DONE $(date -u) ==="
touch /opt/haiku/rebuild-$PORT.done
