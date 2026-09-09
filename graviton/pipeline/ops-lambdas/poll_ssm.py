"""Poll an SSM RunCommand invocation; classify from the wrapper's verdict line.

Input : {instance_id, command_id}
Output: adds {done: bool, ok: bool, error_class: str|None, ssm_status: str,
              log_url: str|None}. The state machine loops Wait -> this ->
              Choice(done) until done, then branches on ok.

The wrapper (run_ssm._wrapper) prints exactly one line:
    VERDICT=<OK|UNRESOLVABLE|FAIL> RC=<n> LOG=s3://.../<name>.log.gz
so we classify from that (reliable, tiny) rather than scraping a truncated log.
Falls back to SSM's own status if the verdict line is somehow absent (e.g. the
instance died before printing it -> treat as timeout/build-error).
"""
import re
import boto3

PENDING = {"Pending", "InProgress", "Delayed"}
_VERDICT = re.compile(r"VERDICT=(\S+)\s+RC=(-?\d+)\s+LOG=(\S+)")


def handler(event, context):
    ssm = boto3.client("ssm")
    try:
        inv = ssm.get_command_invocation(CommandId=event["command_id"],
                                         InstanceId=event["instance_id"])
    except ssm.exceptions.InvocationDoesNotExist:
        event.update(done=False, ok=False, error_class=None, ssm_status="Pending")
        return event

    status = inv["Status"]
    if status in PENDING:
        event.update(done=False, ok=False, error_class=None, ssm_status=status)
        return event

    out = inv.get("StandardOutputContent") or ""
    m = _VERDICT.search(out)
    ok = False
    err = None
    log_url = None
    if m:
        verdict, rc, log_url = m.group(1), int(m.group(2)), m.group(3)
        ok = (verdict == "OK" and rc == 0)
        if not ok:
            err = "unresolvable" if verdict == "UNRESOLVABLE" else "build-error"
    else:
        # No verdict: the wrapper never finished (host died, SSM timeout, etc.).
        err = "timeout" if status in ("TimedOut", "DeliveryTimedOut",
                                      "ExecutionTimedOut") else "build-error"

    event.update(done=True, ok=ok, error_class=err, ssm_status=status,
                 log_url=log_url)
    return event
