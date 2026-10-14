"""RecordOutcome: write a build result to DynamoDB, applying SOP backoff/quarantine.

Input : {table, pkg, target, ok, error_class?, log_url?}
Output: adds {recorded_state} to the event.

- ok=True  -> build_state=built, target_version=<target>, built_at=now, clears lease
              AND clears the previous failure's residue (see below).
- ok=False -> attempt_count++, last_error_class, last_attempt_at, log_url.
              build_state=failed (backoff: state-sync won't re-queue until the
              target version changes), UNLESS attempt_count reaches the quarantine
              threshold (default 3) at this target -> build_state=needs_human,
              esc_reason=repeated-failure (SOP §4/§5).

A SUCCESS CLEARS THE FAILURE RESIDUE. The success path used to set build_state
and nothing else, leaving `last_error_class`, `esc_reason`, `log_url` and
`attempt_count` from the last failure sitting on a row that is now `built`. That
is not cosmetic: the triage census reads `last_error_class` as the row's CURRENT
error class (haiku-triage-failures maps it straight onto `error_class`), so every
such row was counted as a live failure. Measured on the live table before this
fix: 331 of 811 `built` rows still carried `last_error_class`, 163 of them
`unresolvable` -- enough to inflate the "unresolvable" population from 323 to 486,
about 1.5x, and 20 `built` rows still looked escalated via `esc_reason`.

Clearing `attempt_count` on success also makes the quarantine threshold behave
the way the docstring above describes. The counter is a LIFETIME count, not a
per-target one, so without a reset a package that failed twice, succeeded, and
then failed once more at a new target would escalate to needs_human on that
single new failure. No row in the live table had hit that yet (0 of 177
`repeated-failure` escalations had `built_at` set), so this is a latent hazard
being closed, not an observed one being fixed. Note it is only closed for the
success path: consecutive failures at two DIFFERENT targets with no success
between them still accumulate, which a strictly per-target counter would not do.

`last_attempt_at` is deliberately NOT cleared -- claim.py sets it when a lease is
taken, so it means "last time we tried", which a success does not invalidate.

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

# Attributes that describe the LAST FAILURE and must not outlive it. REMOVE on a
# missing attribute is a no-op in DynamoDB, so this is safe on a row that has
# never failed. See the module docstring for why each one is here.
_FAILURE_RESIDUE = ("last_error_class", "esc_reason", "log_url", "attempt_count")


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
                              " REMOVE lease_owner, lease_expires_at, "
                              + ", ".join(_FAILURE_RESIDUE)),
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
