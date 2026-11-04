#!/bin/bash
# drive2.sh: re-run of the #1055 checks after 7f4e2672c5 (ipcp TRACE format), on the
# same builder and generated dir as drive.sh.
set -uo pipefail
export AWS_DEFAULT_REGION=us-west-2 HAIKU_REVISION=hrev59996
TOP=/opt/src/haiku GEN=$TOP/generated.arm64 L=/opt/bf/logs2; mkdir -p $L
B1054=4baaae5db6fd6d5368bd9e2f4de5c455db146cda
B1055=7f4e2672c502f347b9ffc6e6e78e05c58759966b
build() {
	label=$1 sha=$2; shift 2
	git -C "$TOP" checkout -q "$sha" && git -C "$TOP" log --oneline -1
	cd "$GEN"
	for t in "$@"; do
		touch $L/stamp
		HAIKU_REVISION="$HAIKU_REVISION" jam -j"$(nproc)" "$t" > $L/jam-$label-$t.log 2>&1
		rc=$?
		f=$(find "$GEN/objects/haiku/arm64" -type f -name "$t" -path "*add-ons*" -newer $L/stamp | head -1)
		echo "[$label] jam $t rc=$rc errors=$(grep -c ' error: ' $L/jam-$label-$t.log) out=${f:-none}"
		[ -n "$f" ] && [ $rc -eq 0 ] && sha256sum "$f"
		grep -E "network/ppp/.* (error|warning): " $L/jam-$label-$t.log | sed "s/^/    /"
		grep -E "^\.\.\.(failed|updated)" $L/jam-$label-$t.log | sed "s/^/    /"
	done
}
{
date -u +"start %Y%m%dT%H%M%SZ"
git -C "$TOP" fetch -q origin fix/1055-ppp-dump-packet
git -C "$TOP" checkout -q --detach $B1055 && git -C "$TOP" branch -f combo2 $B1055 && git -C "$TOP" checkout -q combo2 && git -C "$TOP" -c user.name=bf-P -c user.email=bf-P@invalid merge -q --no-edit $B1054
COMBO=$(git -C "$TOP" rev-parse HEAD); echo "combo2=$COMBO"
echo "=== release (DEBUG off) ==="
rm -f "$GEN/UserBuildConfig"
build rel-1055v2 $B1055 modem ipcp pppoe
echo "=== DEBUG=1 ==="
cat > "$GEN/UserBuildConfig" <<'UBC'
SetConfigVar DEBUG : HAIKU_TOP src add-ons kernel network ppp modem : 1 : local ;
SetConfigVar DEBUG : HAIKU_TOP src add-ons kernel network ppp ipcp : 1 : local ;
SetConfigVar DEBUG : HAIKU_TOP src add-ons kernel network ppp pppoe : 1 : local ;
UBC
build dbg-1055v2 $B1055 modem ipcp pppoe
build dbg-combo2 $COMBO modem ipcp pppoe
rm -f "$GEN/UserBuildConfig"
date -u +"end %Y%m%dT%H%M%SZ"
} > $L/build2.log 2>&1
aws s3 cp $L/ s3://haiku-graviton-<ACCOUNT_ID>-us-west-2/bugfix/P/logs2/ --recursive --only-show-errors
echo done > $L/DONE; aws s3 cp $L/DONE s3://haiku-graviton-<ACCOUNT_ID>-us-west-2/bugfix/P/logs2/DONE --only-show-errors
