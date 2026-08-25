#!/usr/bin/env python3
"""Push the verified gate artifacts off builder3 to S3.

They go to a SEPARATE prefix, hpkg/arm64-gate/, deliberately. haiku*.hpkg must
never land in the guests' seed directory (hpkg/arm64/): haikuporter activates its
own packagefs from packages/haiku.hpkg, so a haiku*.hpkg swept into the seed set
becomes the chroot's build INPUT and makes itself immortal across every newly
cloned guest. That is how a stale pre-fix libroot survived days of rebuilds.
These two files are outputs for final image assembly, not build inputs.

boto3 rather than the aws CLI because builder3 has no CLI installed; credentials
come from the instance profile via IMDS.
"""
import hashlib
import os
import sys

import boto3

REGION = os.environ.get("AWS_REGION") or "us-west-2"


def default_bucket() -> str:
    """The package-repository bucket, haiku-graviton-<account>-<region>.

    The account id is not written down anywhere in the tree -- this is a public
    repository -- so it is resolved once, here, from whatever credentials this
    process already has. Same convention as graviton/scripts/ssm-run;
    HAIKU_GRAVITON_BUCKET names a different bucket outright and skips the call.

    STS via boto3 rather than a shell-out to `aws`, because the host this runs on
    has no CLI installed (see the module docstring). A failed lookup is fatal
    rather than falling back, because interpolating an empty account id composes
    "haiku-graviton--us-west-2": a valid bucket name that belongs to nobody, so
    the upload would fail later with an access error naming the wrong problem.
    """
    try:
        account = boto3.client("sts", region_name=REGION).get_caller_identity()["Account"]
    except Exception as exc:  # no credentials, no IMDS, no network
        raise SystemExit(
            f"stage_gate: cannot resolve the AWS account id from STS ({exc}); "
            "set HAIKU_GRAVITON_BUCKET to the package-repository bucket"
        ) from exc
    if not account.isdigit():
        raise SystemExit(f"stage_gate: implausible account id from STS: {account!r}")
    return f"haiku-graviton-{account}-{REGION}"


BUCKET = os.environ.get("HAIKU_GRAVITON_BUCKET") or default_bucket()
PREFIX = "hpkg/arm64-gate/"
SRC = "/opt/haiku/fleet/gate-out"

s3 = boto3.client("s3", region_name=REGION)

for name in ("haiku.hpkg", "haiku_datatranslators.hpkg"):
    path = os.path.join(SRC, name)
    if not os.path.isfile(path):
        print(f"MISSING {path}")
        continue
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    key = PREFIX + name
    try:
        s3.upload_file(
            path, BUCKET, key,
            ExtraArgs={"Metadata": {
                "sha256": digest,
                "built-by": "builder3-gate-build",
                "swap-fix": "verified-by-disassembly",
            }},
        )
        print(f"uploaded s3://{BUCKET}/{key}  bytes={os.path.getsize(path)}  sha256={digest}")
    except Exception as exc:                                   # noqa: BLE001
        print(f"UPLOAD FAILED for {name}: {exc}")
        sys.exit(1)

print("--- listing ---")
resp = s3.list_objects_v2(Bucket=BUCKET, Prefix=PREFIX)
for obj in resp.get("Contents", []):
    print(f"  {obj['Key']}  {obj['Size']}  {obj['LastModified']}")
