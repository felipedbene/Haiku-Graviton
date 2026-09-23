"""Start a build command on the builder over SSM RunCommand.

  build.handler -> runs `haiku-nativebuild <pkg>` for event["pkg"]

haiku-nativebuild harvests the built hpkgs to s3://<bucket>/hpkg/arm64/; the wave
then publishes them incrementally in a separate CodeBuild step (see the
RepoPublish project in ops-stack.ts). Publishing is NOT done from here: the Haiku
builder cannot run the Linux `package_repo` host tool or bulk-sync the full repo
pool, both of which the incremental haiku-repo-add needs.

Input : {instance_id, bucket, region?, run_id?, pkg}
Output: adds {command_id}.

LOG HANDLING (why a wrapper, not SSM OutputS3): SSM's native OutputS3 writes the
full log UNCOMPRESSED, and get-command-invocation truncates the inline copy at
24 KB -- unreliable for a multi-MB haikuporter log. Instead a tiny POSIX wrapper
runs the command, **gzips the full log and uploads it via the on-instance
debeos-ssm-agent**, and prints only a compact one-line verdict to stdout:

    VERDICT=<OK|UNRESOLVABLE|FAIL> RC=<n> LOG=s3://<bucket>/logs/<run>/<name>.log.gz

That verdict is a few dozen bytes, so the inline StandardOutputContent poll_ssm
reads is always complete. We deliberately do NOT set OutputS3BucketName (stdout
is now tiny). boto3 send_command takes a JSON command list, so the multi-line
script is passed as-is -- none of the CLI-shorthand comma-mangling that
graviton/scripts/ssm-run works around applies here.
"""
import os
import re
import boto3

NATIVEBUILD = os.environ.get("NATIVEBUILD_PATH", "/boot/home/haiku-nativebuild")
AGENT = os.environ.get("SSM_AGENT_PATH", "/boot/system/bin/debeos-ssm-agent")
HP_TREE = os.environ.get("HP_TREE", "/boot/home/haikuports")
OVERLAY_PREFIX = os.environ.get("OVERLAY_PREFIX", "debeos-overlay")
# Version of graviton/scripts/haiku-nativebuild this deployment was built against.
# The ops stack reads NATIVEBUILD_VERSION out of the tree's driver at synth time and
# sets it here (see ops-stack.ts). Empty only in a stale/local deploy that predates
# #506 -- in which case _refresh_and_assert degrades to a no-op refresh (see there).
NATIVEBUILD_VERSION = os.environ.get("NATIVEBUILD_VERSION", "")
DRIVER_KEY = os.environ.get("NATIVEBUILD_S3_KEY", "bake-scripts/haiku-nativebuild")


def _refresh_and_assert(bucket, version):
    """#506: stage the CURRENT driver onto the builder and REFUSE to build a stale one.

    `run_ssm.build` sends a command that runs /boot/home/haiku-nativebuild -- but that
    file is whatever the (possibly months-old) builder AMI baked. A driver fix could not
    reach a wave without a full rebake, and NOTHING said so: the wave would silently
    re-run the old driver and report a confident, wrong answer (this is exactly the
    checks-that-stopped-checking failure class). So before every build we:

      1. `s3 cp` the driver from s3://<bucket>/<DRIVER_KEY> over NATIVEBUILD, and
      2. assert the staged copy carries NATIVEBUILD_VERSION="<version>" -- the version
         the deployed Lambda was built against. On mismatch we set STALE=1 (the wrapper
         turns that into VERDICT=FAIL RC=3 with a diagnostic log) instead of building,
         so the wave fails LOUDLY rather than testing stale code.

    Emits shell that writes into "$LOG" (set by _wrapper) and sets STALE. If `version`
    is empty (pre-#506 env), it still refreshes from S3 but cannot assert -- it says so
    in the log rather than silently trusting whatever is on the box."""
    src = f"s3://{bucket}/{DRIVER_KEY}"
    lines = [
        f'echo "#506 staging driver from {src}" >> "$LOG"',
        f'{AGENT} s3 cp "{src}" {NATIVEBUILD} >> "$LOG" 2>&1 || '
        f'echo "#506 WARNING: could not refresh driver from {src}" >> "$LOG"',
        f'chmod +x {NATIVEBUILD} 2>/dev/null || true',
    ]
    if version:
        lines += [
            f'if grep -q \'NATIVEBUILD_VERSION="{version}"\' {NATIVEBUILD} 2>/dev/null; then',
            f'  echo "#506 driver OK: {NATIVEBUILD} is NATIVEBUILD_VERSION={version}" >> "$LOG"',
            'else',
            '  STALE=1',
            f'  GOT=$(grep -m1 "NATIVEBUILD_VERSION=" {NATIVEBUILD} 2>/dev/null || true)',
            f'  echo "#506 STALE DRIVER: {NATIVEBUILD} is not NATIVEBUILD_VERSION={version}" >> "$LOG"',
            '  echo "  found instead: ${GOT:-<no NATIVEBUILD_VERSION marker>}" >> "$LOG"',
            '  echo "  REFUSING TO BUILD -- a wave must not silently test a stale driver." >> "$LOG"',
            f'  echo "  Republish the current driver to {src} (graviton/scripts/haiku-publish-driver)" >> "$LOG"',
            '  echo "  and redeploy the ops stack so this Lambda carries the new version." >> "$LOG"',
            'fi',
        ]
    else:
        lines += [
            f'echo "#506 WARNING: NATIVEBUILD_VERSION not set in this deployment -- '
            f'refreshed the driver but CANNOT assert it is current." >> "$LOG"',
        ]
    return "\n".join(lines) + "\n"


def _overlay_stage(pkg, bucket):
    """Emit shell that stages the DeBeOS overlay recipe(s)+patchset(s) for `pkg`
    into the builder's ports tree BEFORE building -- so a committed recipe bump
    actually reaches the build (persistence), instead of a hand-stage that dies
    with the builder. The agent's single-file `s3 cp` can't glob, so we list the
    overlay here (boto3) and emit an explicit cp per file; the tree's port dir is
    found by name (category-agnostic)."""
    s3 = boto3.client("s3")
    pat = re.compile(rf"^{re.escape(pkg)}[-_].*\.(recipe|patchset)$")
    files = []
    paginator = s3.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix=f"{OVERLAY_PREFIX}/"):
        for o in page.get("Contents", []):
            base = o["Key"].rsplit("/", 1)[-1]
            if pat.match(base):
                files.append((o["Key"], base))
    if not files:
        return ""
    lines = [f'D=$(find {HP_TREE} -maxdepth 2 -type d -name {pkg} | head -1)',
             'if [ -n "$D" ]; then mkdir -p "$D/patches"']
    for key, base in files:
        dest = '"$D/patches/"' if base.endswith(".patchset") else '"$D/"'
        lines.append(f'{AGENT} s3 cp "s3://{bucket}/{key}" {dest}{base} || true')
    lines.append('fi')
    return "\n".join(lines) + "\n"


def _wrapper(cmd, bucket, run_id, name, ok_grep, refresh="", prelude=""):
    """POSIX-sh: refresh+assert the driver (#506), optional prelude (overlay staging),
    then run cmd, classify, gzip+upload the full log, echo the compact verdict.

    The driver refresh runs FIRST and writes into "$LOG"; if it finds a stale driver it
    sets STALE=1 and we skip the build, emitting VERDICT=FAIL RC=3 -- poll_ssm records
    that as a build-error with the diagnostic log, so a stale run can never pass silently."""
    return f"""set -u
LOG=/tmp/{name}.$$.log
: > "$LOG"
STALE=0
{refresh}{prelude}if [ "$STALE" = 1 ]; then
RC=3
else
{cmd} >> "$LOG" 2>&1
RC=$?
fi
if [ "$STALE" = 1 ]; then V=FAIL
elif grep -q {ok_grep} "$LOG"; then V=OK
elif grep -q UNRESOLVABLE "$LOG"; then V=UNRESOLVABLE
else V=FAIL; fi
KEY=logs/{run_id}/{name}.log.gz
gzip -9 -c "$LOG" > "$LOG.gz" 2>/dev/null || cp "$LOG" "$LOG.gz"
{AGENT} s3 cp "$LOG.gz" "s3://{bucket}/$KEY" >/dev/null 2>&1 || true
echo "VERDICT=$V RC=$RC LOG=s3://{bucket}/$KEY"
exit $RC
"""


def _send(instance_id, script, timeout):
    ssm = boto3.client("ssm")
    resp = ssm.send_command(
        InstanceIds=[instance_id],
        DocumentName="AWS-RunShellScript",
        Parameters={"commands": [script], "executionTimeout": [str(timeout)]},
    )
    return resp["Command"]["CommandId"]


def _bucket(event):
    return event.get("bucket") or os.environ["WORK_BUCKET"]


def build(event, context):
    pkg = event["pkg"]
    run_id = event.get("run_id", context.aws_request_id)
    bucket = _bucket(event)
    # #506: refresh+assert the driver FIRST (cannot be skipped -- it lives in the only
    # path that runs the build), then stage the overlay for persistence.
    version = event.get("nativebuild_version", NATIVEBUILD_VERSION)
    refresh = _refresh_and_assert(bucket, version)
    prelude = _overlay_stage(pkg, bucket)   # persistence: stage overlay first
    script = _wrapper(f"{NATIVEBUILD} {pkg}", bucket, run_id, pkg,
                      ok_grep=f'"^{pkg}: BUILD_OK"', refresh=refresh, prelude=prelude)
    event["command_id"] = _send(event["instance_id"], script,
                                int(event.get("build_timeout", 21600)))
    return event
