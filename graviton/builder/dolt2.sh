#!/bin/bash
exec >> /opt/haiku/libtool-fix2.log 2>&1
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
scp -P 2227 $O /tmp/ltfix2.py baron@127.0.0.1:/boot/home/ltfix2.py
R=/boot/home/haikuports/input-source-packages/develop/sources/libtool-2.5.4-1/libtool-2.5.4.recipe
# start from the pristine recipe so the failed touch attempt is not compounded
ssh -n -p 2227 $O baron@127.0.0.1 "cp $R.bak-graviton $R 2>/dev/null; python3 /boot/home/ltfix2.py $R; grep -A3 'BUILD_PREREQUIRES=' $R | head -8"
ssh -n -p 2227 $O baron@127.0.0.1 "H=\$(ls /boot/home/haikuports/input-source-packages/libtool_source_rigged-*.hpkg | head -1); touch -r \"\$H\" -d '+1 day' $R"
ssh -n -p 2227 $O baron@127.0.0.1 'haikuporter -y libtool 2>&1' | tail -12
ssh -n -p 2227 $O baron@127.0.0.1 'ls /boot/home/haikuports/packages/ | grep -i libtool || echo NO_LIBTOOL'
scp -P 2227 $O "baron@127.0.0.1:/boot/home/haikuports/packages/*.hpkg" /opt/haiku/hpkg-out/arm64/ 2>&1 | tail -1
aws s3 sync /opt/haiku/hpkg-out/arm64/ "$S3" --exclude "*" --include "*.hpkg" 2>&1 | tail -2
echo "=== DONE $(date) ==="
touch /opt/haiku/libtool-fix2.done
