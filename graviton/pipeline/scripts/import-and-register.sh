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

# This is the single biggest transfer in the whole bake: a ~20 GiB raw image.
# `aws s3 cp` drives one multipart stream and measured ~130 MiB/s here, i.e. about
# 2.5 minutes per GiB of image and the dominant cost of the Register stage. Route
# it through graviton/scripts/haiku-s3, which prefers s5cmd (parallel multipart)
# and falls back to the identical `aws s3 cp` when s5cmd is absent -- so this is
# strictly faster-or-the-same and cannot break the bake if the install step is
# skipped. Resolved relative to the source artifact, which is where the pipeline
# checks out the tree.
HG_S3="${CODEBUILD_SRC_DIR:-}/graviton/scripts/haiku-s3"
echo "==> uploading $RAW_IMAGE -> s3://$WORK_BUCKET/$KEY"
if [ -x "$HG_S3" ]; then
	command -v s5cmd >/dev/null 2>&1 \
		&& echo "    (haiku-s3 / s5cmd, parallel multipart)" \
		|| echo "    (haiku-s3, but s5cmd absent -- falling back to aws s3 cp)"
	AWS_DEFAULT_REGION="$AWS_DEFAULT_REGION" "$HG_S3" put "$RAW_IMAGE" "s3://$WORK_BUCKET/$KEY"
else
	echo "    (haiku-s3 not found at $HG_S3 -- using aws s3 cp)"
	aws s3 cp "$RAW_IMAGE" "s3://$WORK_BUCKET/$KEY" --region "$AWS_DEFAULT_REGION"
fi

echo "==> ec2 import-snapshot (Format=raw)"
TASK_ID=$(aws ec2 import-snapshot --region "$AWS_DEFAULT_REGION" \
  --description "DeBeOS arm64 raw ${STAMP}" \
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
  --description "DeBeOS (arm64) -- ARM-first OS descended from Haiku/BeOS; @minimum-mmc + OpenSSH, baked by CDK pipeline" \
  --architecture arm64 \
  --boot-mode uefi \
  --ena-support \
  --virtualization-type hvm \
  --root-device-name /dev/xvda \
  --block-device-mappings "DeviceName=/dev/xvda,Ebs={SnapshotId=${SNAP_ID},VolumeSize=${ROOT_GIB},VolumeType=gp3,DeleteOnTermination=true}" \
  --query 'ImageId' --output text)
echo "    AMI: $AMI_ID"

# Provenance. Without this, "what is actually in canonical?" is answerable only
# from CodePipeline execution history, which ages out -- and the AMI that was
# promoted on 2026-08-24 had no source-commit at all, so the question could not be
# answered from the image itself.
#
# Derived defensively on purpose: this script runs under `set -euo pipefail`, and
# a missing tag must never be the reason a bake fails. Every branch here ends in a
# value, never a non-zero exit.
SRC_COMMIT="${CODEBUILD_RESOLVED_SOURCE_VERSION:-}"
if [ -z "$SRC_COMMIT" ]; then
  SRC_COMMIT="$(git -C "${CODEBUILD_SRC_DIR:-.}" rev-parse HEAD 2>/dev/null || true)"
fi
[ -n "$SRC_COMMIT" ] || SRC_COMMIT="unknown"

SRC_BRANCH="${HAIKU_SOURCE_BRANCH:-}"
if [ -z "$SRC_BRANCH" ]; then
  SRC_BRANCH="$(git -C "${CODEBUILD_SRC_DIR:-.}" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
fi
# A CODEBUILD_CLONE_REF checkout can be detached, in which case --abbrev-ref
# reports "HEAD", which is worse than admitting we do not know.
if [ -z "$SRC_BRANCH" ] || [ "$SRC_BRANCH" = "HEAD" ]; then
  SRC_BRANCH="unknown"
fi

# DeBeOS-native revision (issue #201). `haiku-revision` below stays pinned to
# HAIKU_REVISION on purpose: it is the r1~beta6_<revision> package version line
# that pkgman compares against the published DeBeOS pool, and haiku-devel-publish
# resolves the devel hpkg by this exact tag (#82). So the DeBeOS revision that
# actually tracks HEAD is recorded in a SEPARATE, additive tag rather than by
# mutating haiku-revision -- nothing that reads haiku-revision changes. The
# scheme mirrors build/scripts/determine_haiku_revision: debeos-r<count>-g<sha>,
# a monotonic ordinal plus the exact commit. Derived defensively (set -euo
# pipefail): the CodeBuild clone can be shallow, so a missing count degrades to
# debeos-g<sha>, and every branch ends in a value.
DEBEOS_REVISION=""
if [ "$SRC_COMMIT" != "unknown" ]; then
  SHORT_SHA="$(git -C "${CODEBUILD_SRC_DIR:-.}" rev-parse --short HEAD 2>/dev/null || true)"
  [ -n "$SHORT_SHA" ] || SHORT_SHA="${SRC_COMMIT:0:9}"
  COMMIT_COUNT="$(git -C "${CODEBUILD_SRC_DIR:-.}" rev-list --count HEAD 2>/dev/null || true)"
  if [ -n "$COMMIT_COUNT" ]; then
    DEBEOS_REVISION="debeos-r${COMMIT_COUNT}-g${SHORT_SHA}"
  else
    DEBEOS_REVISION="debeos-g${SHORT_SHA}"
  fi
fi
[ -n "$DEBEOS_REVISION" ] || DEBEOS_REVISION="unknown"

echo "==> tagging as candidate (canonical NOT set here)"
echo "    haiku-revision:  ${HAIKU_REVISION:-unknown} (pinned; package-version line)"
echo "    debeos-revision: $DEBEOS_REVISION (HEAD-tracking)"
echo "    source-commit: $SRC_COMMIT"
echo "    source-branch: $SRC_BRANCH"
aws ec2 create-tags --region "$AWS_DEFAULT_REGION" --resources "$AMI_ID" "$SNAP_ID" \
  --tags \
    "Key=Name,Value=${NAME}" \
    "Key=project,Value=haiku-graviton" \
    "Key=baked-by,Value=cdk-pipeline" \
    "Key=candidate,Value=true" \
    "Key=haiku-revision,Value=${HAIKU_REVISION:-unknown}" \
    "Key=debeos-revision,Value=${DEBEOS_REVISION}" \
    "Key=source-commit,Value=${SRC_COMMIT}" \
    "Key=source-branch,Value=${SRC_BRANCH}"

echo "$AMI_ID" > "$AMI_ID_OUT"
echo "==> wrote AMI id to $AMI_ID_OUT"
