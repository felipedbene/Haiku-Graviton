#!/bin/bash
# haiku-recipe-build.sh <builder-instance>: stage the claude_code recipe, build it with
# haiku-nativebuild on a native Haiku builder (over tools/ssh-run, evidence logged), and
# upload the hpkg to s3://<bucket>/claude-code-port/ via a presigned PUT.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); inst=$1
B=s3://haiku-graviton-$(aws sts get-caller-identity --query Account --output text)-us-west-2/claude-code-port
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$here/tools/stage-recipe.sh" "$T" > /dev/null
(cd "$T" && COPYFILE_DISABLE=1 tar --no-xattrs -czf recipe.tgz dev-util)
S=$(shasum -a 256 "$T/recipe.tgz" | cut -d' ' -f1)
aws s3 cp --quiet --region us-west-2 "$T/recipe.tgz" "$B/claude_code-recipe.tgz"
G=$("$here/tools/presign" get "$B/claude_code-recipe.tgz" 3600)
P=$("$here/tools/presign" put "$B/claude_code-2.1.314-1-arm64.hpkg" 7200)
# /boot/system/bin/curl explicitly: on a builder, `curl` on PATH is the source-cache shim.
cat > "$T/step.sh" <<STEP
set -u
C=/boot/system/bin/curl
cd /boot/home && \$C -sSfo claude_code-recipe.tgz '$G' || exit 1
echo '$S  claude_code-recipe.tgz' | sha256sum -c - || exit 1
rm -rf /boot/home/haikuports/dev-util/claude_code /boot/home/haikuports/packages/claude_code-*.hpkg
tar xzf claude_code-recipe.tgz -C /boot/home/haikuports
node --version; python3 --version
sh /boot/home/haiku-nativebuild claude_code > /boot/home/cc-nb.log 2>&1; echo "nativebuild rc=\\\$?"
grep -E "BUILD_OK|FAILED" /boot/home/cc-nb.log | tail -3
echo "== deterministic pass"
grep -E "contents=|R4 applied|parsed [0-9]+ modules|P[0-9]+ applied|build-tree:" /boot/home/nb-claude_code.log
cd /boot/home/haikuports/packages && ls -l claude_code-*.hpkg && sha256sum claude_code-*.hpkg || exit 1
\$C -sSf -X PUT --upload-file claude_code-2.1.314-1-arm64.hpkg '$P' && echo upload-ok
STEP
"$here/tools/ssh-run" "$inst" claude_code-recipe-build "$T/step.sh" 5400
