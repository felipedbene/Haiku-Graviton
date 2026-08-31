"""Start a shell command on the builder over SSM RunCommand.

Two entry points share the send logic:
  build.handler   -> runs `haiku-nativebuild <pkg>` for event["pkg"]
  publish.handler -> runs the repo publish for the whole builder pool

Input : {instance_id, bucket, region?, run_id?, pkg? (build only)}
Output: adds {command_id} (build) or {publish_command_id} (publish).

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
import boto3

NATIVEBUILD = os.environ.get("NATIVEBUILD_PATH", "/boot/home/haiku-nativebuild")
REPO_PUBLISH = os.environ.get("REPO_PUBLISH_CMD", "haiku-repo-publish all")
AGENT = os.environ.get("SSM_AGENT_PATH", "/boot/system/bin/debeos-ssm-agent")


def _wrapper(cmd, bucket, run_id, name, ok_grep):
    """POSIX-sh: run cmd, classify, gzip+upload the full log, echo the verdict."""
    return f"""set -u
LOG=/tmp/{name}.$$.log
{cmd} > "$LOG" 2>&1
RC=$?
if grep -q {ok_grep} "$LOG"; then V=OK
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
    script = _wrapper(f"{NATIVEBUILD} {pkg}", _bucket(event), run_id, pkg,
                      ok_grep=f'"^{pkg}: BUILD_OK"')
    event["command_id"] = _send(event["instance_id"], script,
                                int(event.get("build_timeout", 21600)))
    return event


def publish(event, context):
    run_id = event.get("run_id", context.aws_request_id)
    script = _wrapper(REPO_PUBLISH, _bucket(event), run_id, "publish",
                      ok_grep='-i published')
    event["publish_command_id"] = _send(event["instance_id"], script,
                                        int(event.get("publish_timeout", 3600)))
    return event
