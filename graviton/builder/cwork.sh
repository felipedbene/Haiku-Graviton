#!/bin/bash
# cwork.sh <sshport> <port>...  -- build ports sequentially in one guest.
# Like gworker.sh but always passes -G (patch(1) instead of git, required for
# every port whose sources are downloaded) and records the recipe md5 both
# before and after the build, so a silently-reverted recipe edit is visible.
PORT=$1; shift
KEY=/home/ubuntu/.ssh/haiku-ed25519
S="-n -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -o ServerAliveInterval=30 -i $KEY"
SCP="-P $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"
LOG=/opt/haiku/cwork-$PORT.log
exec >> $LOG 2>&1
# Package-repository bucket, resolved from the caller's own credentials so that
# no AWS account id is written down in a public tree. HAIKU_GRAVITON_BUCKET
# overrides it. Same convention as graviton/scripts/ssm-run, which explains why
# the lookup is validated rather than interpolated blind: an empty account id
# composes "haiku-graviton--us-west-2", a valid bucket name that is nobody's.
BUCKET="${HAIKU_GRAVITON_BUCKET:-}"
if [ -z "$BUCKET" ]; then
  ACCT=$(aws sts get-caller-identity --query Account --output text) || ACCT=""
  case "$ACCT" in ""|*[!0-9]*)
    echo "$(basename "$0"): cannot resolve the AWS account id; refresh credentials or set HAIKU_GRAVITON_BUCKET" >&2
    exit 1 ;;
  esac
  BUCKET="haiku-graviton-$ACCT-${AWS_REGION:-us-west-2}"
fi
S3="s3://$BUCKET/hpkg/arm64/"
echo "=== CWORK $PORT START $(date -u) : $* ==="
for p in "$@"; do
  echo "########## $p start $(date -u)"
  ssh $S baron@127.0.0.1 "md5sum /boot/home/haikuports/input-source-packages/develop/sources/$p-*/$p-*.recipe 2>/dev/null || echo 'no ISP recipe'"
  ssh $S baron@127.0.0.1 "haikuporter -y -G $p > /boot/home/c-$p.log 2>&1; echo RC=\$?"
  ssh $S baron@127.0.0.1 "md5sum /boot/home/haikuports/input-source-packages/develop/sources/$p-*/$p-*.recipe 2>/dev/null || echo 'no ISP recipe'"
  scp $SCP "baron@127.0.0.1:/boot/home/c-$p.log" /opt/haiku/c-$p-$PORT.log 2>/dev/null
  echo "---- hpkg check (ls, not exit code) ----"
  ssh $S baron@127.0.0.1 "ls /boot/home/haikuports/packages/ | grep -E \"^${p}[-_]\" || echo NO_PKG_$p"
  echo "---- log size: $(wc -l < /opt/haiku/c-$p-$PORT.log 2>/dev/null) lines ----"
  tail -25 /opt/haiku/c-$p-$PORT.log
  # Harvest via a staging dir and DO NOT bring back haiku*.hpkg. Those are the
  # chroot's build *inputs*, not outputs: haikuporter activates its own packagefs
  # from /boot/home/haikuports/packages/haiku.hpkg, so sweeping them into the
  # guest seed directory made a stale copy immortal. One dated 20 Aug carried the
  # pre-fix system_time() (CNTPCT_EL0 plus an overflowing multiply) and reinstalled
  # itself into every new guest for days, giving every chroot an 18h-skewed clock
  # that silently reverted recipe edits. Rebuilding the image never fixed it,
  # because the loop re-imported the artifact afterwards.
  _stage=$(mktemp -d)
  scp $SCP "baron@127.0.0.1:/boot/home/haikuports/packages/*.hpkg" "$_stage/" >/dev/null 2>&1
  rm -f "$_stage"/haiku.hpkg "$_stage"/haiku_*.hpkg "$_stage"/haiku-*.hpkg
  cp -p "$_stage"/*.hpkg /opt/haiku/hpkg-out/arm64/ 2>/dev/null
  rm -rf "$_stage"
  aws s3 sync /opt/haiku/hpkg-out/arm64/ "$S3" \
      --exclude "*" --include "*.hpkg" >/dev/null 2>&1
  echo "########## $p done $(date -u)"
done
echo "=== CWORK $PORT DONE $(date -u) ==="
touch /opt/haiku/cwork-$PORT.done
