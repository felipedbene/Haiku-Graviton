#!/bin/bash
# drive.sh: issue 1046 compile check (bf-P). Oven cross-build buildspec setup with
# WORK_BUCKET = read-only copy of the oven cross-tools cache + hpkg pool under
# bugfix/P/, then build dialup and every ppp add-on on trunk and on the fix branch.
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
FIX=a2f26eef27b2fca660f89d4a82a50ea623af35d4
TARGETS="dialup ppp ppp_manager ppp_frame ipcp pap pppoe modem"
L=/opt/bf/logs; mkdir -p $L
mbuild() {
	label=$1 sha=$2
	git -C "$TOP" checkout -q "$sha" && git -C "$TOP" log --oneline -1
	cd "$GEN"
	for t in $TARGETS; do
		HAIKU_REVISION="$HAIKU_REVISION" jam -q -j"$(nproc)" "$t" > $L/jam-$label-$t.log 2>&1
		rc=$?
		f=$(find "$GEN/objects/haiku/arm64" -type f -name "$t" -path "*add-ons*" | head -1)
		echo "[$label] jam -q $t rc=$rc errors=$(grep -c ' error: ' $L/jam-$label-$t.log) warnings=$(grep -c ' warning: ' $L/jam-$label-$t.log) out=${f:-none}"
		[ -n "$f" ] && [ $rc -eq 0 ] && sha256sum "$f" && rm -f "$f"
		grep -E " (error|warning): " $L/jam-$label-$t.log | sed "s/^/    /"
	done
}
{
date -u +"start setup %Y%m%dT%H%M%SZ"
apt-get update -qq && apt-get install -y -qq git python3-yaml > /dev/null
rm -rf /opt/src /opt/cb-state.sh /tmp/ct.env; mkdir -p /opt/src
git clone -q https://github.com/felipedbene/Haiku-Graviton.git "$TOP"
git -C "$TOP" fetch -q origin fix/1046-dialup-build
git -C "$TOP" checkout -q $TRUNK && git -C "$TOP" log --oneline -1
python3 -I /opt/bf/runphase.py "$TOP/graviton/pipeline/buildspecs/cross-build.yml" \
	install pre_build "build:jam pass 0"
echo "setup rc=$?"
date -u +"end setup %Y%m%dT%H%M%SZ"
} > $L/setup.log 2>&1
{
date -u +"start build %Y%m%dT%H%M%SZ"
mbuild trunk $TRUNK
mbuild fix $FIX
date -u +"end build %Y%m%dT%H%M%SZ"
} > $L/build.log 2>&1
aws s3 cp $L/ s3://$WORK_BUCKET/logs/ --recursive --only-show-errors
echo done > $L/DONE
aws s3 cp $L/DONE s3://$WORK_BUCKET/logs/DONE --only-show-errors
