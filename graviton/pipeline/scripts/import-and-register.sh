#!/usr/bin/env bash
#
# import-and-register.sh -- upload the raw Haiku arm64 disk image to S3, run
# `aws ec2 import-snapshot`, wait for it, then `aws ec2 register-image` with the
# EC2 boot properties the Graviton port needs, and tag the result as a
# *candidate* (canonical is deliberately NOT set here -- that is a separate,
# human-gated promotion step; see graviton/scripts/haiku-canonical).
#
# This is the cloud-native replacement for the hand-run
#   aws ec2 import-snapshot (raw) -> aws ec2 register-image --architecture arm64
#     --boot-mode uefi --ena-support --virtualization-type hvm
#     --root-device-name /dev/xvda
# from docs/ssh-arm64-desktop-bake.md.
#
# Required env (set by the CodeBuild buildspec):
#   RAW_IMAGE          path to haiku-ec2.raw
#   WORK_BUCKET        S3 bucket authorized for the vmimport role (import/ prefix)
#   IMPORT_PREFIX      key prefix within the bucket (e.g. "import")
#   AWS_DEFAULT_REGION target region
#   AMI_NAME_PREFIX    AMI name prefix; a timestamp is appended
#   ROOT_VOLUME_BYTES  root EBS size in bytes
# Optional:
#   HAIKU_REVISION     stamped into a tag
#
# Writes the registered AMI id to $AMI_ID_OUT (default /tmp/ami_id) so the
# buildspec can export it as a pipeline variable.
set -euo pipefail

: "${RAW_IMAGE:?path to haiku-ec2.raw}"
: "${WORK_BUCKET:?S3 work bucket}"
: "${AWS_DEFAULT_REGION:?region}"
IMPORT_PREFIX="${IMPORT_PREFIX:-import}"
AMI_NAME_PREFIX="${AMI_NAME_PREFIX:-haiku-graviton}"
ROOT_VOLUME_BYTES="${ROOT_VOLUME_BYTES:-2147483648}"
AMI_ID_OUT="${AMI_ID_OUT:-/tmp/ami_id}"
STAMP="$(date -u +%s)"
KEY="${IMPORT_PREFIX}/haiku-ec2-${STAMP}.raw"
NAME="${AMI_NAME_PREFIX}-${STAMP}"
ROOT_GIB=$(( (ROOT_VOLUME_BYTES + 1073741823) / 1073741824 ))

echo "==> uploading $RAW_IMAGE -> s3://$WORK_BUCKET/$KEY"
aws s3 cp "$RAW_IMAGE" "s3://$WORK_BUCKET/$KEY" --region "$AWS_DEFAULT_REGION"

echo "==> ec2 import-snapshot (Format=raw)"
TASK_ID=$(aws ec2 import-snapshot --region "$AWS_DEFAULT_REGION" \
  --description "haiku-graviton arm64 raw ${STAMP}" \
  --disk-container "Format=raw,UserBucket={S3Bucket=${WORK_BUCKET},S3Key=${KEY}}" \
  --query 'ImportTaskId' --output text)
echo "    import task: $TASK_ID"

echo "==> waiting for import to complete"
SNAP_ID=""
for _ in $(seq 1 120); do   # up to ~60 min at 30s intervals
  read -r STATUS SNAP_ID MSG < <(aws ec2 describe-import-snapshot-tasks \
    --region "$AWS_DEFAULT_REGION" --import-task-ids "$TASK_ID" \
    --query 'ImportSnapshotTasks[0].SnapshotTaskDetail.[Status,SnapshotId,StatusMessage]' \
    --output text)
  echo "    status=$STATUS snapshot=${SNAP_ID:-<none>} ${MSG:-}"
  case "$STATUS" in
    completed) break ;;
    deleted|deleting|error) echo "import failed: $MSG" >&2; exit 1 ;;
  esac
  sleep 30
done
[ -n "$SNAP_ID" ] && [ "$SNAP_ID" != "None" ] || { echo "import did not produce a snapshot" >&2; exit 1; }
echo "    snapshot: $SNAP_ID"

echo "==> register-image (arm64/uefi/ena/hvm, root /dev/xvda)"
AMI_ID=$(aws ec2 register-image --region "$AWS_DEFAULT_REGION" \
  --name "$NAME" \
  --description "Haiku on Graviton (arm64) -- @minimum-mmc + OpenSSH, baked by CDK pipeline" \
  --architecture arm64 \
  --boot-mode uefi \
  --ena-support \
  --virtualization-type hvm \
  --root-device-name /dev/xvda \
  --block-device-mappings "DeviceName=/dev/xvda,Ebs={SnapshotId=${SNAP_ID},VolumeSize=${ROOT_GIB},VolumeType=gp3,DeleteOnTermination=true}" \
  --query 'ImageId' --output text)
echo "    AMI: $AMI_ID"

echo "==> tagging as candidate (canonical NOT set here)"
aws ec2 create-tags --region "$AWS_DEFAULT_REGION" --resources "$AMI_ID" "$SNAP_ID" \
  --tags \
    "Key=Name,Value=${NAME}" \
    "Key=project,Value=haiku-graviton" \
    "Key=baked-by,Value=cdk-pipeline" \
    "Key=candidate,Value=true" \
    "Key=haiku-revision,Value=${HAIKU_REVISION:-unknown}"

echo "$AMI_ID" > "$AMI_ID_OUT"
echo "==> wrote AMI id to $AMI_ID_OUT"
