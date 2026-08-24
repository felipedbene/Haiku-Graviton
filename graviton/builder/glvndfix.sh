#!/bin/bash
# glvndfix.sh <sshport>  -- fix libglvnd's Haiku GL backend so it compiles.
#
# libglvnd-1.7.0 is the port that provides devel:libgl, and it is what makes the whole
# jasper -> netpbm -> groff chain reachable without the mesa/LLVM wall the status doc
# used to describe. It fails to compile, and the failure is a two-line naming mismatch:
#
#   ../src/HGL/GLView.cpp:76:29: error: 'PTHREAD_RECURSIVE_MUTEX_INITIALIZER' was not
#       declared in this scope; did you mean 'PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP'?
#
# Haiku's <pthread.h> defines ONLY the non-portable spelling -- verified by reading the
# header, which has PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP at line 81 and no plain form.
# The two uses are in src/HGL/, libglvnd's *Haiku* backend, so this is a Haiku-side
# portability bug in the port, not an arm64 one: the aarch64 sources in the same build
# (entry_aarch64_tsd.c) compile cleanly, and 77 of 102 objects were already built when
# this stopped it.
#
# This is a real portability fix, NOT a cut: nothing is being dropped or disabled, and
# the two spellings are the same macro. Upstream-worthy as-is.
set -u
PORT=${1:?usage: glvndfix.sh <sshport>}
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20"
S="-n -p $PORT $O -i $KEY"
G=baron@127.0.0.1
# libglvnd has no input source package, so the ports-tree recipe IS the live one.
R=/boot/home/haikuports/sys-libs/libglvnd/libglvnd-1.7.0.recipe
q() { ssh $S $G "$1" 2>&1 | grep -v 'Permanently added'; }

echo "=== glvndfix on guest $PORT ==="
q "ls -la $R" || { echo "FATAL: no recipe at $R"; exit 1; }

if q "grep -q 'graviton: PTHREAD_RECURSIVE_MUTEX_INITIALIZER' $R"; then
	echo "already patched"
else
	# Insert the rewrite at the top of BUILD(), before meson configures anything.
	# \b after INITIALIZER cannot match before "_NP" (underscore is a word character),
	# so this is idempotent and cannot double-append.
	BLOCK=$(cat <<'EOB' | base64 -w0
	# graviton: PTHREAD_RECURSIVE_MUTEX_INITIALIZER is not a Haiku spelling -- <pthread.h>
	# defines only PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP. The two uses are in libglvnd's
	# own Haiku backend (src/HGL/GLView.cpp:76,111), so the port has never compiled here.
	# Same macro, portable name; nothing is disabled by this. The \b cannot match before
	# "_NP" because underscore is a word character, so re-running is harmless.
	sed -i -e 's/PTHREAD_RECURSIVE_MUTEX_INITIALIZER\b/PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP/g' \
		src/HGL/GLView.cpp
	grep -c PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP src/HGL/GLView.cpp

EOB
)
	q "echo '$BLOCK' | base64 -d > /boot/home/glvnd-block.txt && \
		sed -i '/^BUILD()$/{n;r /boot/home/glvnd-block.txt
		}' $R && rm -f /boot/home/glvnd-block.txt"
fi

echo "---- verification (by content; sed exits 0 on no match) ----"
q "grep -n 'graviton: PTHREAD\|sed -i -e .s/PTHREAD\|^BUILD()\|meson --buildtype' $R"
n=$(q "grep -c 'PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP/g' $R")
echo "rewrite lines present: $n (want 1)"
[ "$n" = "1" ] || { echo "FATAL: rewrite not inserted exactly once"; exit 1; }

# The work tree already holds an unpacked, unpatched copy from the failed run. rebuild.sh
# clears work dirs for input-source-package ports only, and libglvnd is not one, so clear
# it here or haikuporter prints "Skipping unpack" and rebuilds the same broken source.
echo "---- clearing the stale work tree from the failed run ----"
q "w=/boot/home/haikuports/sys-libs/libglvnd/work-1.7.0;
	if [ -e \"\$w\" ]; then rm -rf \"\$w\" 2>/dev/null || mv \"\$w\" \"\$w.wedged-\$\$\"; fi;
	ls -d /boot/home/haikuports/sys-libs/libglvnd/work-* 2>/dev/null || echo 'work tree gone'"
echo "=== glvndfix done ==="
