#!/bin/bash
# prepguest.sh <sshport> -- make one guest ready for the non-dirty rebuild.
#
# Fixing the chroot clock changed the chroot `haiku` package's version from
# r1~beta6_hrev59996_dirty-1 to r1~beta6_hrev59996-1, and `_dirty-1` sorts ABOVE
# `-1`. Every hpkg built before the fix records `requires haiku >= ..._dirty-1`,
# so on a repaired guest those requirements are unsatisfiable and the chain
# cannot rebuild -- libiconv-1.18 blocks gettext, and so on down the set.
#
# So a rebuild guest must start with a packages/ directory that contains ONLY
# the chroot's inputs: the repaired haiku*.hpkg and the cross-built _bootstrap
# set. Everything else is moved to /boot/home/dirtypkgs/ -- moved, not deleted,
# so `package extract` still works on it and so nothing is destroyed by a
# mistaken diagnosis.
#
# It also reinstalls the recipe edits. Those live in a guest's
# input-source-packages tree, which is re-extracted from the source hpkg whenever
# mtime(recipe) <= mtime(sourcePackage) -- so an edit that is not mtime-pinned
# vanishes silently. That has already happened: the libtool and (earlier) the
# perl/autoconf edits were reverted this way, which is why the canonical copies
# now live on the metal in /opt/haiku/recipe-overlay and in the repo under
# graviton/haikuports-patches/recipes/.
#
# Refuses to prepare a guest whose chroot haiku is still _dirty: that guest is
# consistent with the OLD package set and must be left alone.
set -u
PORT=${1:?usage: prepguest.sh <sshport>}
KEY=/home/ubuntu/.ssh/haiku-ed25519
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30"
S="-n -p $PORT $OPTS -i $KEY"
SCP="-P $PORT $OPTS -i $KEY"
G=baron@127.0.0.1
GP=/boot/home/haikuports/packages
ISP=/boot/home/haikuports/input-source-packages
OVERLAY=/opt/haiku/recipe-overlay
LOG=/opt/haiku/prepguest-$PORT.log
exec >> "$LOG" 2>&1

echo "=== PREPGUEST $PORT START $(date -u) ==="

ssh $S $G true || { echo "FAIL: guest $PORT unreachable"; exit 1; }

# haikuports.conf: a fresh guest without it fails every build instantly with
# "Unable to find haikuports.conf" -- and that looks like a recipe problem.
ssh $S $G 'test -f /boot/home/config/settings/haikuports.conf' \
	|| { echo "installing haikuports.conf"; \
	     scp $SCP /opt/haiku/hp.conf $G:/boot/home/config/settings/haikuports.conf; }

echo "--- chroot haiku version"
# `package info` answers on ONE line: "name: haiku  version: r1~beta6_hrev59996-1".
# An anchored ^version grep silently matches nothing and reads as a failure to
# find the package -- which is exactly the "empty output means error, not nothing
# to do" trap this project has already paid for once.
ver=$(ssh $S $G "package info $GP/haiku.hpkg 2>/dev/null" | grep -o 'version: [^ ]*')
echo "$ver"
case "$ver" in
	*_dirty*) echo "REFUSING: chroot haiku is still $ver -- this guest matches the OLD set"; exit 2 ;;
	*hrev*) ;;
	*) echo "FAIL: could not read the chroot haiku version"; exit 3 ;;
esac

echo "--- quarantining non-input hpkgs out of $GP"
ssh $S $G "mkdir -p /boot/home/dirtypkgs
	cd $GP || exit 1
	moved=0
	for f in *.hpkg; do
		case \"\$f\" in
			haiku.hpkg|haiku_*|haiku-*|*_bootstrap*|makefile_engine.hpkg|netfs.hpkg|userland_fs.hpkg) ;;
			*) mv -f \"\$f\" /boot/home/dirtypkgs/ && moved=\$((moved+1)) ;;
		esac
	done
	echo \"moved \$moved ; remaining \$(ls *.hpkg | wc -l)\""

echo "--- installing the edited recipes and pinning their mtimes"
for r in "$OVERLAY"/*.recipe; do
	[ -e "$r" ] || continue
	b=$(basename "$r")                 # e.g. tar-1.35.recipe
	stem=${b%.recipe}                  # tar-1.35
	port=${stem%-*}                    # tar
	scp $SCP "$r" "$G:/boot/home/$b" || { echo "scp $b FAILED"; continue; }
	ssh $S $G "d=\$(ls -d $ISP/develop/sources/$stem-* 2>/dev/null | head -1)
		[ -z \"\$d\" ] && { echo '$b: no ISP source dir'; exit 0; }
		sp=\$(ls $ISP/${port}_source_rigged-*.hpkg 2>/dev/null | head -1)
		[ -z \"\$sp\" ] && { echo '$b: no source package to pin against'; exit 0; }
		cp -f /boot/home/$b \"\$d/$b\"
		# -r <sourcePackage> -d '+2 days': the pin has to be relative to the file
		# haikuporter compares against, never to the wall clock, which has stepped
		# backwards in these chroots.
		touch -r \"\$sp\" -d '+2 days' \"\$d/$b\"
		printf '%s pinned: ' '$b'; ls -la \"\$d/$b\" | awk '{print \$6, \$7, \$8}'
		rm -f /boot/home/$b"
done

echo "--- source proxy shim + haikuporter unpack override + patch(1)"
/opt/haiku/scripts/haiku-source-proxy install-shim "$PORT" 2>&1 | tail -5
/opt/haiku/scripts/haiku-haikuporter-patch "$PORT" 2>&1 | tail -8

# haiku-haikuporter-patch drops patch-*.hpkg into packages/ to extract bin/patch
# from it. The binary is what is wanted; the hpkg itself is a pre-fix _dirty
# package and would re-poison dependency resolution, so it goes back out.
ssh $S $G "mv -f $GP/patch-*.hpkg /boot/home/dirtypkgs/ 2>/dev/null; ls $GP/*.hpkg | wc -l"

echo "--- final packages/ contents"
ssh $S $G "ls $GP/"
echo "=== PREPGUEST $PORT DONE $(date -u) ==="
touch /opt/haiku/prepguest-$PORT.done
