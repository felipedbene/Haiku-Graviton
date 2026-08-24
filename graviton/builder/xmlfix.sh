#!/bin/bash
# xmlfix.sh <sshport>  -- retire the libxml2 stage-1 cut on one guest.
#
# libxml2-2.15.3 was built with `pythonModuleEnabled=false` forced on, which drops only
# the libxml2_python3.14 subpackage. That was filed as a cosmetic cleanup ("delete one
# line once cmd:python3.14 exists"). It is not cosmetic -- it is on the critical path to
# a browser:
#
#   libxml2_python3.14  <- itstool  <- gtk_doc  <- cmd:gtkdocize  <- libidn2  <- libpsl
#
# and devel:libpsl is one of only two hard edges in front of haikuwebkit. Retiring this
# cut is what makes curl's --without-libpsl cut unnecessary for WebKit too, so it is
# cheaper than it looks and it removes debt rather than adding it.
#
# Preconditions: cmd:python3.14 (built) and setuptools_python314 (from the setuptools
# port). Both are checked below rather than assumed -- the recipe turns the Python
# binding on for arm64 and then needs both.
set -u
PORT=${1:?usage: xmlfix.sh <sshport>}
V=2.15.3; R=1
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20"
S="-n -p $PORT $O -i $KEY"
G=baron@127.0.0.1
GP=/boot/home/haikuports/packages
RECIPE=/boot/home/haikuports/input-source-packages/develop/sources/libxml2-$V-$R/libxml2-$V.recipe
q() { ssh $S $G "$1" 2>&1 | grep -v 'Permanently added'; }

echo "=== xmlfix: retire the libxml2 python cut on guest $PORT ==="

echo "---- preconditions ----"
py=$(q "ls $GP/ | grep -c '^python3.14-'")
st=$(q "ls $GP/ | grep -c '^setuptools_python314-\|^setuptools-'")
echo "python3.14 present in packages/: $py (want >=1)"
echo "setuptools present in packages/: $st (want >=1)"
[ "$py" != "0" ] || { echo "ABORT: python3.14 not on this guest; push the pool first"; exit 1; }
[ "$st" != "0" ] || { echo "ABORT: setuptools not on this guest; build it first"; exit 1; }

q "ls -la $RECIPE" || { echo "FATAL: no recipe at $RECIPE"; exit 1; }

echo "---- before ----"
q "grep -n 'pythonModuleEnabled=' $RECIPE"

# Remove ONLY the graviton override -- the trailing assignment that follows the
# STAGE-1 CUT comment. The two legitimate `pythonModuleEnabled=true` lines inside the
# architecture block, and the initial `=false` default at the top, must survive: the
# default is upstream's, and deleting it would leave the variable unset.
if q "grep -q 'STAGE-1 CUT (graviton)' $RECIPE"; then
	# delete the comment block and the single assignment that closes it
	q "sed -i '/# STAGE-1 CUT (graviton)/,/^pythonModuleEnabled=false$/d' $RECIPE"
else
	echo "no graviton cut marker found -- already retired, or a different recipe"
fi

echo "---- after ----"
q "grep -n 'pythonModuleEnabled=\|STAGE-1 CUT' $RECIPE"

# Verify by CONTENT. Two things must hold: the marker is gone, and the two
# architecture-block `=true` assignments are still there. sed exits 0 on no match, so
# the exit code proves nothing.
marker=$(q "grep -c 'STAGE-1 CUT (graviton)' $RECIPE")
trues=$(q "grep -c 'pythonModuleEnabled=true' $RECIPE")
echo "graviton cut markers left: $marker (want 0)"
echo "pythonModuleEnabled=true:  $trues (want 2)"
[ "$marker" = "0" ] || { echo "FATAL: cut still present"; exit 1; }
[ "$trues" = "2" ]  || { echo "FATAL: the architecture block was damaged (want 2 true assignments)"; exit 1; }
echo "verified: the override is gone and the upstream logic is intact"

# Pin forward of the source package or haikuporter silently re-extracts and reverts.
q "srcpkg=\$(ls -1 /boot/home/haikuports/input-source-packages/libxml2-${V}*.hpkg 2>/dev/null | head -1);
	if [ -n \"\$srcpkg\" ]; then
		touch -d @\$(( \$(stat -c %Y \"\$srcpkg\") + 172800 )) $RECIPE; echo \"pinned forward of \$(basename \$srcpkg)\"
	else
		touch -d '+2 days' $RECIPE; echo 'no source hpkg; pinned +2 days'
	fi"
echo "=== xmlfix done -- now rebuild libxml2 and check for the _python3.14 subpackage ==="
