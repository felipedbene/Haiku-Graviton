#!/bin/bash
# haiku-accept-install.sh <acceptance-instance>: install the built nodejs24 + claude_code
# hpkgs on a canonical Haiku instance and run the non-interactive acceptance checks there
# (tools/ssh-run, evidence logged). Downloads go through presigned GETs (no S3 client).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); inst=$1
B=s3://haiku-graviton-$(aws sts get-caller-identity --query Account --output text)-us-west-2/claude-code-port
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
sha() { aws s3 cp --quiet --region us-west-2 "$B/$1" - | shasum -a 256 | cut -d' ' -f1; }
(cd "$here" && COPYFILE_DISABLE=1 tar --no-xattrs -czf "$T/port.tgz" tools compat 2>/dev/null)
aws s3 cp --quiet --region us-west-2 "$T/port.tgz" "$B/port-tools.tgz"
{
	echo 'set -u'
	echo 'W=/boot/home/cc-accept; rm -rf $W; mkdir -p $W; cd $W; C=/boot/system/bin/curl'
	for f in nodejs24-24.21.0-1-arm64.hpkg claude_code-2.1.314-2-arm64.hpkg vectors-2.1.314.tgz port-tools.tgz; do
		echo "\$C -sSfo $f '$("$here/tools/presign" get "$B/$f" 3600)' || exit 1"
		echo "echo '$(sha "$f")  $f' | sha256sum -c - || exit 1"
	done
	cat <<'STEP'
echo "== install"
N=$W/nodejs24-24.21.0-1-arm64.hpkg; [ -e /boot/system/packages/nodejs24-24.21.0-1-arm64.hpkg ] && N=""
pkgman uninstall -y claude_code 2>&1 | tail -1
pkgman install -y tmux $N $W/claude_code-2.1.314-2-arm64.hpkg 2>&1 | tail -3
command -v node claude; node --version; ls /boot/system/lib/claude-code | head -20
echo "== claude --version"; claude --version; echo "rc=$?"
echo "== claude doctor (no TTY)"; HOME=$W/home-doctor timeout 120 claude doctor < /dev/null 2>&1 | head -60; echo "rc=$?"
echo "== cc-teaminfo"; /boot/system/lib/claude-code/bin/cc-teaminfo $$; /boot/system/lib/claude-code/bin/cc-teaminfo -a | wc -l
echo "== compat vectors on Haiku"
mkdir -p port && tar xzf port-tools.tgz -C port && tar xzf vectors-2.1.314.tgz
bash port/tools/check-vectors.sh vec-all /boot/system/bin/node; echo "vectors rc=$?"
STEP
} > "$T/step.sh"
"$here/tools/ssh-run" "$inst" claude-accept-install "$T/step.sh" 7200
