#!/bin/bash
# groffcheck.sh <sshport>  -- capability test for the freshly built groff.
#
# groff exists to *render*, so `--version` is not evidence and neither is RC=0. The reason
# groff was on the critical path is that gettext wants `MAN2HTML = groff -mandoc -Thtml`,
# so the test is: feed it a man page and check that real bytes, containing a known marker,
# come back on each device it will be asked for.
#
# Deliberately does NOT use pkgman. Installing groff pulls a transitive runtime closure
# (netpbm -> lib:libjpeg, psutils -> puremagic/pypdf/typing_extensions/libpaper2, jasper
# -> ...) and naming every hpkg by hand is both tedious and beside the point: an earlier
# run of this check reported 0 bytes on all four devices purely because the install had
# failed, which is an activation result masquerading as a capability result. `package
# extract` sidesteps the solver entirely and tests the artifact we actually built -- the
# move this project already documents for putting a tool on PATH without activating it.
set -u
PORT=${1:?usage: groffcheck.sh <sshport>}
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20"
S="-n -p $PORT $O -i $KEY"
G=baron@127.0.0.1
GP=/boot/home/haikuports/packages
q() { ssh $S $G "$1" 2>&1 | grep -v 'Permanently added'; }

echo "=== groffcheck on guest $PORT ==="
q "ls -la $GP/groff-1.23.0-2-arm64.hpkg"

echo "---- extract groff and netpbm/psutils into a private prefix ----"
q "rm -rf /tmp/gx && mkdir -p /tmp/gx && cd /tmp/gx &&
	for p in groff-1.23.0-2-arm64 netpbm-10.86.42-3-arm64 psutils-3.3.11-1-any; do
		[ -e $GP/\$p.hpkg ] && package extract $GP/\$p.hpkg >/dev/null 2>&1 && echo \"extracted \$p\"
	done
	echo '--- what came out ---'
	ls /tmp/gx
	ls /tmp/gx/bin 2>/dev/null | head -20"

echo "---- version ----"
q "cd /tmp/gx && GROFF_FONT_PATH=/tmp/gx/share/groff/1.23.0/font \
	GROFF_TMAC_PATH=/tmp/gx/share/groff/1.23.0/tmac \
	PATH=/tmp/gx/bin:\$PATH ./bin/groff --version 2>&1 | head -2"

echo "---- RENDER: a man page through each device, checking bytes AND a marker ----"
q "cd /tmp/gx && cat > t.1 <<'EOM'
.TH GRAVITON 1 \"2026-08-24\" \"graviton\" \"Test\"
.SH NAME
graviton \\- a page that must actually render
.SH DESCRIPTION
UniqueMarkerGravitonRendered if this appears, groff formatted a real document.
EOM
export GROFF_FONT_PATH=/tmp/gx/share/groff/1.23.0/font
export GROFF_TMAC_PATH=/tmp/gx/share/groff/1.23.0/tmac
export PATH=/tmp/gx/bin:\$PATH
for dev in ascii utf8 ps html; do
	n=\$(groff -mandoc -T\$dev t.1 2>/dev/null | wc -c)
	m=\$(groff -mandoc -T\$dev t.1 2>/dev/null | grep -c UniqueMarkerGravitonRendered)
	printf '  -T%-6s %8s bytes   marker: %s\n' \"\$dev\" \"\$n\" \"\$m\"
done"

echo "---- the four commands that made groff hard to reach ----"
q "for c in pnmcrop pnmtopng pnmtops psselect; do
	if [ -x /tmp/gx/bin/\$c ]; then printf '  %-10s present\n' \$c; else printf '  %-10s MISSING\n' \$c; fi
done"

echo "---- MAN2HTML: the one thing gettext wanted from groff ----"
q "cd /tmp/gx && export GROFF_FONT_PATH=/tmp/gx/share/groff/1.23.0/font \
	GROFF_TMAC_PATH=/tmp/gx/share/groff/1.23.0/tmac PATH=/tmp/gx/bin:\$PATH &&
	groff -mandoc -Thtml t.1 2>/dev/null | grep -ciE '<html|<body|<p>' | sed 's/^/  html tags: /'"
echo "=== groffcheck done ==="
