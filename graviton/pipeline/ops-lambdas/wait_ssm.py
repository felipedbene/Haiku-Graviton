"""WaitForSSM: is the builder BOTH SSM-registered AND build-env ready?

Input : {instance_id, probe_command_id?}
Output: adds {ssm_ready: bool, probe_command_id?}. The state machine loops
        Wait -> this -> Choice(ssm_ready) until true.

Two hardening steps over a naive PingStatus check (both learned from the smoke
test, where 5 OTHER Online SSM nodes exist in the account and our node reported
Online in ~33s):

  1. INSTANCE-ID MATCH. describe_instance_information is filtered by our
     instance_id AND we verify the returned entry's InstanceId equals ours, so a
     filter hiccup can never let a different Online node (there are several) read
     as "our builder is ready".
  2. READINESS PROBE, not just Online. SSM Online means the agent is up, NOT
     that Haiku booted its build environment. So once Online+matched, we run a
     one-shot probe (`haikuporter` present AND the ports tree exists) and only
     report ssm_ready=true when THAT succeeds -- otherwise StartBuild could
     send-command into a half-booted box. The probe id is carried in the event
     across loop iterations so we send it once and then poll it.
"""
import boto3

PENDING = {"Pending", "InProgress", "Delayed"}
PROBE = ("command -v haikuporter >/dev/null 2>&1 && "
         "test -d /boot/home/haikuports && echo BUILDER_READY")


def _online_and_ours(ssm, instance_id):
    resp = ssm.describe_instance_information(
        Filters=[{"Key": "InstanceIds", "Values": [instance_id]}]
    )
    for info in resp.get("InstanceInformationList", []):
        if info.get("InstanceId") == instance_id and info.get("PingStatus") == "Online":
            return True
    return False


def handler(event, context):
    ssm = boto3.client("ssm")
    iid = event["instance_id"]
    event["ssm_ready"] = False

    if not _online_and_ours(ssm, iid):
        return event  # not registered yet -> loop

    probe_id = event.get("probe_command_id")
    if not probe_id:
        # First time Online: fire the readiness probe, keep looping.
        resp = ssm.send_command(
            InstanceIds=[iid], DocumentName="AWS-RunShellScript",
            Parameters={"commands": [PROBE], "executionTimeout": ["60"]},
        )
        event["probe_command_id"] = resp["Command"]["CommandId"]
        return event

    # Probe in flight: check it.
    try:
        inv = ssm.get_command_invocation(CommandId=probe_id, InstanceId=iid)
    except ssm.exceptions.InvocationDoesNotExist:
        return event
    status = inv["Status"]
    if status in PENDING:
        return event
    if status == "Success" and "BUILDER_READY" in (inv.get("StandardOutputContent") or ""):
        event["ssm_ready"] = True
    else:
        # Probe failed (env not up yet): re-arm so the next loop re-probes.
        event.pop("probe_command_id", None)
    return event
