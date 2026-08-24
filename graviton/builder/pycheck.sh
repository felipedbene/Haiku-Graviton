#!/bin/bash
# pycheck.sh <sshport> <port-version-revision>  e.g. pycheck.sh 2229 python3.10-3.10.20-3
#
# Capability test for a freshly built python. NOT an existence test, and deliberately
# paranoid about *which* interpreter it is testing.
#
# The reason python3.10 is on the critical path is Blocker 3's residual: haikuporter runs
# under a python with no zlib/_bz2/_lzma, so it cannot unpack a compressed tarball without
# the metal's help. A built hpkg does not settle that -- those are separate extension
# builds that fail quietly and are merely reported "missing" by configure.
#
# Two traps this script exists to avoid, both of which produced a false negative first:
#
#  1. The guest already has python3.10_bootstrap, and BOTH packages provide cmd:python3.10.
#     The first run of this check reported every module missing -- it was measuring the
#     bootstrap. The tell was the version: the bootstrap is 3.10.21, the port we build is
#     3.10.20. So this script pins the expected version and REFUSES to report module
#     results until `python3.10 -V` matches. Blocker 6's habit 3 ("provides is not
#     function") one step further: resolving a cmd: says nothing about which binary ran.
#  2. `pkgman install <python.hpkg>` alone fails with "nothing provides cmd:file", because
#     file-5.43 is built but not activated. Pass every local hpkg it needs in one go.
set -u
PORT=${1:?usage: pycheck.sh <sshport> <name-version-revision>}
FULL=${2:?}
PY=${FULL%%-*}
WANT=$(echo "$FULL" | sed -E 's/^[^-]+-([^-]+)-.*/\1/')
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20"
S="-n -p $PORT $O -i $KEY"
G=baron@127.0.0.1
GP=/boot/home/haikuports/packages
q() { ssh $S $G "$1" 2>&1 | grep -v 'Permanently added'; }

echo "=== pycheck $PY (want version $WANT) on guest $PORT ==="
q "ls -la $GP/${FULL}-arm64.hpkg"

echo "---- install the port plus the local packages it needs ----"
q "pkgman install -y $GP/${FULL}-arm64.hpkg $GP/file-5.43-2-arm64.hpkg 2>&1 | tail -8"

got=$(q "$PY -V 2>&1" | tail -1)
echo "---- interpreter on PATH: $got (want Python $WANT) ----"
if ! printf '%s' "$got" | grep -q "$WANT"; then
	echo "PATH still resolves to the wrong interpreter; quarantining the bootstrap"
	q "mkdir -p /boot/home/quarantine && mv -f $GP/${PY}-*_bootstrap*.hpkg /boot/home/quarantine/ 2>/dev/null; ls /boot/home/quarantine/"
	q "pkgman install -y $GP/${FULL}-arm64.hpkg $GP/file-5.43-2-arm64.hpkg 2>&1 | tail -6"
	got=$(q "$PY -V 2>&1" | tail -1)
	echo "after quarantine: $got"
fi
if ! printf '%s' "$got" | grep -q "$WANT"; then
	echo "INCONCLUSIVE: could not get $WANT onto PATH, so module results below would"
	echo "describe the wrong binary. Reporting nothing rather than a false negative."
	exit 2
fi
echo "confirmed: testing the port we built, not the bootstrap"

echo "---- extension modules (the point of the exercise) ----"
for m in zlib bz2 lzma sqlite3 readline ssl ctypes _decimal; do
	r=$(q "$PY -c 'import $m' 2>&1" | tail -1)
	if [ -z "$r" ]; then printf '  OK      %s\n' "$m"
	else printf '  MISSING %-9s %s\n' "$m" "$r"; fi
done

echo "---- functional, not merely importable: round-trip each codec ----"
q "$PY -c '
import zlib, bz2, lzma
d = b\"graviton\" * 512
for n, mod in ((\"zlib\", zlib), (\"bz2\", bz2), (\"lzma\", lzma)):
    c = mod.compress(d)
    assert mod.decompress(c) == d, n
    print(\"  OK      %-5s %d -> %d bytes, round-trip exact\" % (n, len(d), len(c)))
'"

echo "---- Blocker 3 residual 1: stdlib tarfile opens compressed tars unaided ----"
q "cd /tmp && rm -rf pyck && mkdir pyck && cd pyck && echo hello > a.txt &&
	for z in 'czf t.tar.gz' 'cjf t.tar.bz2' 'cJf t.tar.xz'; do tar \$z a.txt; done &&
	$PY -c '
import tarfile
for f in (\"t.tar.gz\", \"t.tar.bz2\", \"t.tar.xz\"):
    print(\"  is_tarfile(%-11s) = %s\" % (f, tarfile.is_tarfile(f)))
'"
echo "=== pycheck $PY done ==="
