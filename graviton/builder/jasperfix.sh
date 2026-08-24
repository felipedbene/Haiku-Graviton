#!/bin/bash
# jasperfix.sh <sshport>  -- drop jasper's jiv viewer so netpbm/groff can proceed.
#
# THE CUT IS NEEDED, BUT NOT FOR THE REASON THE STATUS DOC GAVE.
#
# The old account: jasper needs devel:libGL, which means mesa-25.3.6, which needs
# libLLVM + libvulkan + cmd:glslangValidator + cmd:git, so cut -DJAS_ENABLE_OPENGL=OFF
# to avoid a multi-hour wall.
#
# What actually happens, measured: devel:libGL is provided by libglvnd-1.7.0 and
# devel:libglu by glu-9.0.0, both of which build here in about seventy seconds combined.
# jasper's cmake then reports
#
#   -- Found OpenGL: /boot/system/develop/lib/libGL.so
#   OpenGL libraries: /boot/system/develop/lib/libGL.so;/boot/system/develop/lib/libGLU.so
#
# so OpenGL is fine and mesa was never involved. It fails one line later on something
# else entirely:
#
#   CMake Error ... Could NOT find GLUT (missing: GLUT_glut_LIBRARY)
#   ... build/cmake/modules/JasOpenGL.cmake:16 (find_package)
#
# GLUT. And there is **no GLUT recipe anywhere in the haikuports tree** -- no glut, no
# freeglut, and nothing provides devel:libglut, cmd:glut or devel:libfreeglut. That is
# the real blocker, and unlike mesa it cannot be built at all.
#
# So the conclusion survives and the reasoning does not. Recorded because "the cut was
# needed" would otherwise be taken as confirmation of an explanation that is wrong, and
# the wrong explanation is what made this look like a multi-hour job instead of a
# two-line one.
#
# WHAT THE CUT COSTS, stated as a capability and not as a flag: GLUT is used only by
# jasper's `jiv` image *viewer*. netpbm wants devel:libjasper, the JPEG-2000 codec
# library, which is untouched. Per the rule Blocker 6 paid for -- when a cut removes a
# library, test the capability that library provided -- the check after this is that the
# built jasper still provides devel:libjasper and that netpbm links against it, which is
# verified by netpbm building rather than by jasper's exit code.
set -u
PORT=${1:?usage: jasperfix.sh <sshport>}
V=2.0.33; R=2
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20"
S="-n -p $PORT $O -i $KEY"
G=baron@127.0.0.1
RECIPE=/boot/home/haikuports/input-source-packages/develop/sources/jasper-$V-$R/jasper-$V.recipe
q() { ssh $S $G "$1" 2>&1 | grep -v 'Permanently added'; }

echo "=== jasperfix on guest $PORT ==="
q "ls -la $RECIPE" || { echo "FATAL: no recipe at $RECIPE"; exit 1; }

echo "---- before ----"
q "grep -n 'JAS_ENABLE_OPENGL\|cmd:jiv\|lib:libGL\|lib:libglu' $RECIPE"

# 1. turn the viewer off. 2. stop advertising cmd:jiv, which will no longer be built --
#    a package that claims a command it does not ship is worse than one that ships less.
# 3. drop the two runtime lib:libGL/libglu requires that only jiv needed.
# devel:libGL/devel:libglu in BUILD_REQUIRES are deliberately LEFT ALONE: they resolve
# now that libglvnd and glu are built, and removing a dependency that is satisfied would
# be an unnecessary divergence from upstream.
q "sed -i \
	-e 's|-DJAS_ENABLE_OPENGL=ON|-DJAS_ENABLE_OPENGL=OFF\t# graviton: no GLUT in the tree|' \
	-e '/cmd:jiv\$secondaryArchSuffix/d' \
	-e '/^\t\tlib:libGL\$secondaryArchSuffix\$/d' \
	-e '/^\t\tlib:libglu\$secondaryArchSuffix\$/d' \
	$RECIPE"

echo "---- after ----"
q "grep -n 'JAS_ENABLE_OPENGL\|cmd:jiv\|lib:libGL\|lib:libglu\|devel:libGL\|devel:libglu' $RECIPE"

# Verify by content; sed exits 0 on no match so its status proves nothing.
off=$(q "grep -c 'JAS_ENABLE_OPENGL=OFF' $RECIPE")
jiv=$(q "grep -c 'cmd:jiv' $RECIPE")
dev=$(q "grep -c 'devel:libGL\|devel:libglu' $RECIPE")
echo "OPENGL=OFF present:       $off (want 1)"
echo "cmd:jiv still advertised: $jiv (want 0)"
echo "devel:libGL/glu retained: $dev (want 2)"
[ "$off" = "1" ] || { echo "FATAL: the OpenGL flag was not flipped"; exit 1; }
[ "$jiv" = "0" ] || { echo "FATAL: cmd:jiv is still advertised"; exit 1; }
[ "$dev" = "2" ] || { echo "FATAL: BUILD_REQUIRES was damaged (they should survive)"; exit 1; }
echo "verified"

q "srcpkg=\$(ls -1 /boot/home/haikuports/input-source-packages/jasper-${V}*.hpkg 2>/dev/null | head -1);
	if [ -n \"\$srcpkg\" ]; then
		touch -d @\$(( \$(stat -c %Y \"\$srcpkg\") + 172800 )) $RECIPE; echo 'pinned forward of source package'
	else
		touch -d '+2 days' $RECIPE; echo 'no source hpkg; pinned +2 days'
	fi"
echo "=== jasperfix done ==="
