#!/bin/bash
# pyfix.sh <sshport> <port> <version> <revision>
#
# Two independent fixes to a python recipe in one guest. Both were found by building it,
# and neither is a dependency problem -- every dependency resolved and both builds reached
# the compiler.
#
# FIX 1 -- drop PGO/LTO, keep -O3.
#   python3.14  cc1: error: LTO support has not been enabled in this configuration
#               The recipe sets "--enable-optimizations --with-lto" when optimizedBuild is
#               true, and the cross-built bootstrap gcc 13.3.0 on this image has no LTO
#               support compiled in. Note configure reports "checking for --with-lto... yes"
#               -- it probes whether the flag is accepted, not whether the feature exists.
#   python3.10  PGO runs the freshly linked ./python, which trips FIX 2 below.
#   Why not the recipe's own optimizedBuild=false switch: it also drops -O3 to -O0, and this
#   interpreter is about to build the whole PEP-517 ladder, meson and ninja.
#
# FIX 2 -- make RUNSHARED additive.
#   Haiku's runtime_loader REPLACES the default library search path with LIBRARY_PATH
#   rather than prepending to it. Python's generated Makefile sets
#       RUNSHARED= LIBRARY_PATH=<build dir>
#   so the freshly linked ./python can find libpython3.x.so -- but that also hides
#   libroot.so and libnetwork.so, so Makefile's pybuilddir.txt rule dies with
#       runtime_loader: Cannot open file libnetwork.so (needed by .../python)
#       generate-posix-vars failed
#   Same defect class, and the same remedy, as perl-5.42.2-library-path.patch does for
#   LDLIBPTH. Do NOT clear RUNSHARED: the build directory is genuinely needed.
set -u
PORT=${1:?usage: pyfix.sh <sshport> <port> <version> <revision>}
P=${2:?}; V=${3:?}; R=${4:?}
KEY=/home/ubuntu/.ssh/haiku-ed25519
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20"
S="-n -p $PORT $OPTS -i $KEY"
SCP="-P $PORT $OPTS -i $KEY"
G=baron@127.0.0.1
DIR=/boot/home/haikuports/input-source-packages/develop/sources/$P-$V-$R
RECIPE=$DIR/$P-$V.recipe

echo "=== pyfix $P on guest $PORT ==="
ssh $S $G "ls -la $RECIPE" || { echo "FATAL: no recipe at $RECIPE"; exit 1; }

# ---- FIX 1: blank the PGO/LTO flag, leaving the -O3 in the same branch alone.
ssh $S $G "sed -i 's|^\(\s*\)maybeEnableOptimizations=\"--enable-optimizations.*\"|\1maybeEnableOptimizations=\t# graviton: PGO/LTO cut, see pyfix.sh|' $RECIPE"

# ---- FIX 2: teach the recipe to make RUNSHARED additive after configure writes the
# Makefile. The anchor is the NOTE comment that sits immediately above `make $jobArgs`
# in both the 3.10 and 3.14 recipes, and `sed r` inserts *after* the matched line, so
# the block lands immediately before make. Shipped via base64 so that no layer of shell
# rewrites the backslashes in the inner sed.
BLOCK=$(cat <<'EOB' | base64 -w0
	# graviton: Haiku's runtime_loader REPLACES the default library search path with
	# LIBRARY_PATH rather than prepending to it. Python's Makefile sets
	# "RUNSHARED= LIBRARY_PATH=<build dir>" so the freshly linked ./python can find
	# libpython3.x.so, but that also hides libroot.so and libnetwork.so, so the
	# pybuilddir.txt rule dies with "runtime_loader: Cannot open file libnetwork.so"
	# and "generate-posix-vars failed". Make the path additive -- same fix as
	# perl-5.42.2-library-path.patch applies to LDLIBPTH. Do NOT clear RUNSHARED:
	# the build directory is genuinely needed to find the new libpython.
	sed -i -e "s|^\(RUNSHARED=[[:space:]]*LIBRARY_PATH=.*\)$|\1:/boot/system/lib:/boot/system/non-packaged/lib|" Makefile
	grep -n "^RUNSHARED" Makefile

EOB
)
if ssh $S $G "grep -q 'graviton: Haiku.s runtime_loader REPLACES' $RECIPE"; then
	echo "FIX 2 already present, skipping insert"
else
	ssh $S $G "echo '$BLOCK' | base64 -d > /boot/home/pyfix-block.txt && \
		sed -i '/# NOTE: When using \"--enable-optimizations\"/r /boot/home/pyfix-block.txt' $RECIPE && \
		rm -f /boot/home/pyfix-block.txt"
fi

# ---- verify by CONTENT, never by a sed exit code: sed returns 0 when it matches nothing.
echo "---- verification ----"
left=$(ssh $S $G "grep -c '^[[:space:]]*maybeEnableOptimizations=\"--enable-optimizations' $RECIPE")
o3=$(ssh $S $G "grep -c 'OPT+=\" -O3\"' $RECIPE")
rs=$(ssh $S $G "grep -c 'RUNSHARED=\[\[:space:\]\]\*LIBRARY_PATH' $RECIPE")
echo "live PGO/LTO assignments: $left (want 0)"
echo "-O3 retained:             $o3 (want >=1)"
echo "RUNSHARED fix present:    $rs (want 1)"
[ "$left" = "0" ] || { echo "FATAL: PGO/LTO assignment still live"; exit 1; }
[ "$o3" != "0" ]  || { echo "FATAL: -O3 disappeared; this cut is meant to keep it"; exit 1; }
[ "$rs" = "1" ]   || { echo "FATAL: RUNSHARED fix not inserted exactly once"; exit 1; }
ssh $S $G "grep -n 'maybeEnableOptimizations=\|RUNSHARED\|make \$jobArgs' $RECIPE | head -12"

# ---- pin the recipe forward of its source package, or haikuporter re-extracts and
# silently reverts the edit. Computed from the package's own mtime where possible so it
# does not depend on the clock.
ssh $S $G "srcpkg=\$(ls -1 /boot/home/haikuports/input-source-packages/${P}-${V}*.hpkg 2>/dev/null | head -1);
	if [ -n \"\$srcpkg\" ]; then
		touch -d @\$(( \$(stat -c %Y \"\$srcpkg\") + 172800 )) $RECIPE
		echo \"pinned forward of \$(basename \$srcpkg)\"
	else
		touch -d '+2 days' $RECIPE
		echo 'no source hpkg found; pinned +2 days from the clock'
	fi"
echo "=== pyfix $P done ==="
