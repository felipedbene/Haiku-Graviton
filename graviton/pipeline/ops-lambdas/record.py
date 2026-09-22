"""RecordOutcome: write a build result to DynamoDB, applying SOP backoff/quarantine.

Input : {table, pkg, target, ok, error_class?, log_url?}
Output: adds {recorded_state} to the event.

- ok=True  -> build_state=built, target_version=<target>, built_at=now, clears lease.
- ok=False -> attempt_count++, last_error_class, last_attempt_at, log_url.
              build_state=failed (backoff: state-sync won't re-queue until the
              target version changes), UNLESS attempt_count reaches the quarantine
              threshold (default 3) at this target -> build_state=needs_human,
              esc_reason=repeated-failure (SOP §4/§5).

Every write that sets build_state also ensures `queued_at` exists: it is the
by-build-state GSI's range key, and DynamoDB silently omits from the index any item
missing it -- which is how a `needs_human` row (`qt5`) went missing from the triage
census for months (#487). `if_not_exists` so a real queue timestamp is never
overwritten by an outcome record.
"""
import os
import time
import boto3

from pkgkey import validate_pkg_key

TABLE = os.environ.get("TABLE")
QUARANTINE_AT = int(os.environ.get("QUARANTINE_AT", "3"))

# Appended to every build_state-setting UpdateExpression. See module docstring.
_GSI_KEY = "queued_at=if_not_exists(queued_at,:now)"


def handler(event, context):
    table = boto3.resource("dynamodb").Table(event.get("table") or TABLE)
    now = int(time.time())
    pkg = validate_pkg_key(event["pkg"], where="RecordOutcome")
    target = event["target"]

    if event.get("ok"):
        table.update_item(
            Key={"pkg": pkg},
            UpdateExpression=("SET build_state=:s, target_version=:t, built_at=:now, "
                              "last_outcome=:o, " + _GSI_KEY +
                              " REMOVE lease_owner, lease_expires_at"),
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
            UpdateExpression=("SET build_state=:s, esc_reason=:r, " + _GSI_KEY +
                              " REMOVE lease_owner, lease_expires_at"),
            ExpressionAttributeValues={":s": state, ":r": reason, ":now": now},
        )
    else:
        state = "failed"
        table.update_item(
            Key={"pkg": pkg},
            UpdateExpression=("SET build_state=:s, " + _GSI_KEY +
                              " REMOVE lease_owner, lease_expires_at"),
            ExpressionAttributeValues={":s": state, ":now": now},
        )
    event["recorded_state"] = state
    return event
