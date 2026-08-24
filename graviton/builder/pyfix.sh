#!/bin/bash
# pyfix.sh <sshport> <port> <version> <revision>
#
# One fix to a python recipe in one guest. It was found by building it, and it is not a
# dependency problem -- every dependency resolved and the build reached the compiler.
#
# This script used to carry a SECOND fix, which made Python's RUNSHARED additive because
# Haiku's runtime_loader REPLACED the default library search path with LIBRARY_PATH instead
# of prepending to it. That was a platform defect, and it is now fixed in the OS
# (`runtime_loader: make LIBRARY_PATH and ADDON_PATH additive`), so the workaround is gone
# from here rather than merely disabled: python3.10-3.10.20 builds on a post-fix host with
# the RUNSHARED sed absent, and the build log shows the previously fatal
#     LIBRARY_PATH=/sources/Python-3.10.20 ./python -E -S -m sysconfig --generate-posix-vars
# running unmodified -- no ":/boot/system/lib" appended -- and succeeding.
#
# THE HOST MATTERS. On a guest booted from a pre-fix image the loader still replaces, and
# that step still dies with "Cannot open file libnetwork.so" and "generate-posix-vars
# failed". Check before building, with a positive control:
#     /bin/echo control-ok                   # must print
#     LIBRARY_PATH=/tmp/empty /bin/echo ok    # fixed loader prints; broken one exits 3, silent
#
# FIX 1 -- drop PGO/LTO, keep -O3. This one STAYS: it is a toolchain gap, unrelated to the
# loader, and no amount of loader fixing touches it.
#   python3.14  cc1: error: LTO support has not been enabled in this configuration
#               The recipe sets "--enable-optimizations --with-lto" when optimizedBuild is
#               true, and the cross-built bootstrap gcc 13.3.0 on this image has no LTO
#               support compiled in. Note configure reports "checking for --with-lto... yes"
#               -- it probes whether the flag is accepted, not whether the feature exists.
#   python3.10  PGO runs the freshly linked ./python. On a pre-fix host that tripped the
#               loader defect described above; on a post-fix host it is no longer a reason
#               to cut PGO for 3.10, but the cut is kept uniform across both interpreters
#               until 3.10 has been rebuilt with PGO live and verified.
#   Why not the recipe's own optimizedBuild=false switch: it also drops -O3 to -O0, and this
#   interpreter is about to build the whole PEP-517 ladder, meson and ninja.
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

# ---- The RUNSHARED workaround that used to be inserted here is GONE, because the platform
# defect it worked around is fixed. It is removed rather than commented out so that nobody
# can reintroduce it by flipping a flag; the write-up lives in the header and in
# graviton/haikuports-patches/README.md.
#
# It is replaced by a REFUSAL: this script will not prepare a python recipe on a host whose
# loader still replaces the search path, because the build would fail at generate-posix-vars
# and the failure looks like a compiler problem rather than an environment one. Same
# principle as pycheck.sh exiting 2 rather than reporting results for an unconfirmed binary.
echo "---- host loader check (the RUNSHARED workaround is gone; the host must not need it) ----"
loader=$(ssh $S $G "sha256sum /boot/system/runtime_loader | cut -d' ' -f1")
echo "host runtime_loader: $loader"
# Negative control first: a plain run must work, or the probe below proves nothing.
ctl=$(ssh $S $G "/bin/echo control-ok 2>&1")
[ "$ctl" = "control-ok" ] || { echo "FATAL: negative control failed ($ctl) -- the probe is not measuring the loader"; exit 1; }
add=$(ssh $S $G "mkdir -p /tmp/pyfix-empty; LIBRARY_PATH=/tmp/pyfix-empty /bin/echo additive-ok 2>&1")
if [ "$add" != "additive-ok" ]; then
	echo "FATAL: this host's loader REPLACES the library search path (probe said: '${add:-<silence, exit 3>}')."
	echo "       python will die at generate-posix-vars. Boot a guest from an image containing"
	echo "       'runtime_loader: make LIBRARY_PATH and ADDON_PATH additive', or swap that one"
	echo "       file into the guest's system package -- see graviton/docs/package-chain-status.md."
	exit 2
fi
echo "loader is additive: no RUNSHARED workaround needed"

# ---- verify by CONTENT, never by a sed exit code: sed returns 0 when it matches nothing.
echo "---- verification ----"
left=$(ssh $S $G "grep -c '^[[:space:]]*maybeEnableOptimizations=\"--enable-optimizations' $RECIPE")
o3=$(ssh $S $G "grep -c 'OPT+=\" -O3\"' $RECIPE")
echo "live PGO/LTO assignments: $left (want 0)"
echo "-O3 retained:             $o3 (want >=1)"
[ "$left" = "0" ] || { echo "FATAL: PGO/LTO assignment still live"; exit 1; }
[ "$o3" != "0" ]  || { echo "FATAL: -O3 disappeared; this cut is meant to keep it"; exit 1; }
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
