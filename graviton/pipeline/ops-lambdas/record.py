"""RecordOutcome: write a build result to DynamoDB, applying SOP backoff/quarantine.

Input : {table, pkg, target, ok, error_class?, log_url?}
Output: adds {recorded_state} to the event.

- ok=True  -> build_state=built, target_version=<target>, built_at=now, clears lease.
- ok=False -> attempt_count++, last_error_class, last_attempt_at, log_url.
              build_state=failed (backoff: state-sync won't re-queue until the
              target version changes), UNLESS attempt_count reaches the quarantine
              threshold (default 3) at this target -> build_state=needs_human,
              esc_reason=repeated-failure (SOP §4/§5).
"""
import os
import time
import boto3

TABLE = os.environ.get("TABLE")
QUARANTINE_AT = int(os.environ.get("QUARANTINE_AT", "3"))


def handler(event, context):
    table = boto3.resource("dynamodb").Table(event.get("table") or TABLE)
    now = int(time.time())
    pkg = event["pkg"]
    target = event["target"]

    if event.get("ok"):
        table.update_item(
            Key={"pkg": pkg},
            UpdateExpression=("SET build_state=:s, target_version=:t, built_at=:now, "
                              "last_outcome=:o REMOVE lease_owner, lease_expires_at"),
            ExpressionAttributeValues={":s": "built", ":t": target, ":now": now,
                                       ":o": "built"},
        )
        event["recorded_state"] = "built"
        return event

    # Failure path: increment attempts, decide failed vs needs_human.
    resp = table.update_item(
        Key={"pkg": pkg},
        UpdateExpression=("SET attempt_count=if_not_exists(attempt_count,:z)+:one, "
                          "last_error_class=:e, last_attempt_at=:now, "
                          "last_outcome=:o, log_url=:lu, target_version=:t"),
        ExpressionAttributeValues={":z": 0, ":one": 1, ":e": event.get("error_class") or "build-error",
                                   ":now": now, ":o": "failed", ":t": target,
                                   ":lu": event.get("log_url") or "n/a"},
        ReturnValues="UPDATED_NEW",
    )
    attempts = int(resp["Attributes"].get("attempt_count", 1))

    if attempts >= QUARANTINE_AT:
        state, reason = "needs_human", "repeated-failure"
        table.update_item(
            Key={"pkg": pkg},
            UpdateExpression=("SET build_state=:s, esc_reason=:r "
                              "REMOVE lease_owner, lease_expires_at"),
            ExpressionAttributeValues={":s": state, ":r": reason},
        )
    else:
        state = "failed"
        table.update_item(
            Key={"pkg": pkg},
            UpdateExpression="SET build_state=:s REMOVE lease_owner, lease_expires_at",
            ExpressionAttributeValues={":s": state},
        )
    event["recorded_state"] = state
    return event
