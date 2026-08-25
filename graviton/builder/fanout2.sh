#!/bin/bash
exec >> /opt/haiku/fanout2.log 2>&1
set -x
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
KEY=/home/ubuntu/.ssh/haiku-ed25519
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 -i $KEY"
echo "=== FANOUT2 $(date) ==="
# 1. harvest what guest 2227 just built (diffutils + automake) and make it durable
scp -P 2227 $O "baron@127.0.0.1:/boot/home/haikuports/packages/*.hpkg" /opt/haiku/hpkg-out/arm64/ 2>&1 | tail -1
ls /opt/haiku/hpkg-out/arm64/ | grep -iE "diffutils|automake"
aws s3 sync /opt/haiku/hpkg-out/arm64/ "$S3" --exclude "*" --include "*.hpkg" 2>&1 | tail -2
# 2. push the full package set to the other two guests so they can resolve deps
for p in 2229 2230; do
  scp -P $p $O /opt/haiku/hpkg-out/arm64/*.hpkg baron@127.0.0.1:/boot/home/haikuports/packages/ 2>&1 | tail -1
  ssh -n -p $p $O baron@127.0.0.1 'ls /boot/home/haikuports/packages/*.hpkg | wc -l'
done
echo "=== dispatching ==="
# spine on 2227; independent branches on 2229/2230, all now dependency-satisfied
rm -f /opt/haiku/worker-2227.done /opt/haiku/worker-2229.done /opt/haiku/worker-2230.done
rm -f /opt/haiku/worker-2227.log /opt/haiku/worker-2229.log /opt/haiku/worker-2230.log
setsid nohup /opt/haiku/worker.sh 2227 libtool tar >/dev/null 2>&1 < /dev/null &
sleep 2
setsid nohup /opt/haiku/worker.sh 2229 help2man m4 pkgconf >/dev/null 2>&1 < /dev/null &
sleep 2
setsid nohup /opt/haiku/worker.sh 2230 bzip2 >/dev/null 2>&1 < /dev/null &
sleep 2
echo "=== DISPATCHED $(date) ==="
touch /opt/haiku/fanout2.done
