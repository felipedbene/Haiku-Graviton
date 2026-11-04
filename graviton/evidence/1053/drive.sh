#!/bin/bash
# drive.sh: issues 1053/1054/1055 compile check (bf-P). Oven cross-build buildspec
# setup with WORK_BUCKET = read-only copy of the oven cross-tools cache + hpkg pool
# under bugfix/P/, then release builds of dialup + every ppp add-on on trunk and on
# each fix branch, and DEBUG=1 builds of modem/ipcp/pppoe.
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive
export AWS_DEFAULT_REGION=us-west-2 HAIKU_REVISION=hrev59996 HG_SSH_DIR=/opt/haiku/ssh
export WORK_BUCKET=haiku-graviton-<ACCOUNT_ID>-us-west-2/bugfix/P
export IMPORT_PREFIX=import CACHE_PREFIX=cache
export BUILDTOOLS_REPO=https://github.com/felipedbene/buildtools.git BUILDTOOLS_BRANCH=master
export GEN_SUBDIR=generated.arm64
export CODEBUILD_SRC_DIR=/opt/src/haiku
TOP=$CODEBUILD_SRC_DIR GEN=$TOP/$GEN_SUBDIR
TRUNK=a9954f424adad9f0a9ebbf64a4f8493add9bd620
B1053=88bd67c0f1452bca77ba8fa4c8794de848ad16f2
B1054=4baaae5db6fd6d5368bd9e2f4de5c455db146cda
B1055=2550c4a7dc887f9d4011e91a4b005d5decb2b252
REL="dialup ppp ppp_manager ppp_frame ipcp pap pppoe modem"
DBG="modem ipcp pppoe"
L=/opt/bf/logs; mkdir -p $L
# build <label> <sha> <targets...>; jam without -q so every object is attempted
build() {
	label=$1 sha=$2; shift 2
	git -C "$TOP" checkout -q "$sha" && git -C "$TOP" log --oneline -1
	cd "$GEN"
	for t in "$@"; do
		HAIKU_REVISION="$HAIKU_REVISION" jam -j"$(nproc)" "$t" > $L/jam-$label-$t.log 2>&1
		rc=$?
		f=$(find "$GEN/objects/haiku/arm64" -type f -name "$t" -path "*add-ons*" -newer $L/stamp | head -1)
		echo "[$label] jam $t rc=$rc errors=$(grep -c ' error: ' $L/jam-$label-$t.log) out=${f:-none}"
		[ -n "$f" ] && [ $rc -eq 0 ] && sha256sum "$f"
		grep -E "network/(ppp|devices/dialup)/.* (error|warning): " $L/jam-$label-$t.log | sed "s/^/    /"
		grep -E "^\.\.\.failed" $L/jam-$label-$t.log | sed "s/^/    /"
	done
}
{
date -u +"start setup %Y%m%dT%H%M%SZ"
apt-get update -qq && apt-get install -y -qq git python3-yaml > /dev/null
rm -rf /opt/src /opt/cb-state.sh /tmp/ct.env; mkdir -p /opt/src
git clone -q https://github.com/felipedbene/Haiku-Graviton.git "$TOP"
for b in fix/1053-ppp-unused-var fix/1054-pppoe-build fix/1055-ppp-dump-packet; do git -C "$TOP" fetch -q origin $b; done
git -C "$TOP" checkout -q $TRUNK && git -C "$TOP" log --oneline -1
python3 -I /opt/bf/runphase.py "$TOP/graviton/pipeline/buildspecs/cross-build.yml" \
	install pre_build "build:jam pass 0"
echo "setup rc=$?"
# combined 1054+1055 tree, so the DEBUG pppoe module can link past the #1054 errors
git -C "$TOP" checkout -q -b combo $B1055 && git -C "$TOP" -c user.name=bf-P -c user.email=bf-P@invalid merge -q --no-edit $B1054 && echo "combo=$(git -C "$TOP" rev-parse HEAD)"
git -C "$TOP" checkout -q $TRUNK
date -u +"end setup %Y%m%dT%H%M%SZ"
} > $L/setup.log 2>&1
COMBO=$(git -C "$TOP" rev-parse combo)
{
date -u +"start build %Y%m%dT%H%M%SZ"
echo "=== release builds (DEBUG off) ==="
for ref in trunk:$TRUNK 1053:$B1053 1054:$B1054 1055:$B1055; do
	touch $L/stamp; build rel-${ref%%:*} ${ref#*:} $REL
done
echo "=== DEBUG=1 builds of modem/ipcp/pppoe ==="
cat > "$GEN/UserBuildConfig" <<'UBC'
SetConfigVar DEBUG : HAIKU_TOP src add-ons kernel network ppp modem : 1 : local ;
SetConfigVar DEBUG : HAIKU_TOP src add-ons kernel network ppp ipcp : 1 : local ;
SetConfigVar DEBUG : HAIKU_TOP src add-ons kernel network ppp pppoe : 1 : local ;
UBC
for ref in trunk:$TRUNK 1055:$B1055 combo1054+1055:$COMBO; do
	touch $L/stamp; build dbg-${ref%%:*} ${ref#*:} $DBG
done
echo "--- DEBUG objects: -DDEBUG=1 present on the compile lines ---"
grep -lE "DDEBUG=1" $L/jam-dbg-*.log | sed 's#.*/##'
rm -f "$GEN/UserBuildConfig"
date -u +"end build %Y%m%dT%H%M%SZ"
} > $L/build.log 2>&1
aws s3 cp $L/ s3://$WORK_BUCKET/logs/ --recursive --only-show-errors
echo done > $L/DONE
aws s3 cp $L/DONE s3://$WORK_BUCKET/logs/DONE --only-show-errors
