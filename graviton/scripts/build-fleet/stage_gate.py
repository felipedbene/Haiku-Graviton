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

# Same env-override convention as graviton/scripts/ssm-run.
BUCKET = os.environ.get("HAIKU_GRAVITON_BUCKET", "haiku-graviton-668984504585-us-west-2")
PREFIX = "hpkg/arm64-gate/"
SRC = "/opt/haiku/fleet/gate-out"

s3 = boto3.client("s3", region_name="us-west-2")

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
