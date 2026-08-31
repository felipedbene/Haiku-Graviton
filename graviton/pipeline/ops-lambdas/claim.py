"""ClaimBatch: atomically lease each pkg in the chain (queued -> building).

Input : {table, chain:[pkg,...], target:{pkg:ver}, exec_id, lease_secs?}
Output: adds {claimed:[...], skipped:[...]} to the event.

Uses a conditional update so two concurrent executions can never build the same
package: claim succeeds only if the item is currently `queued` (or a stale
`building` whose lease expired) and not suppressed.
"""
import os
import time
import boto3

TABLE = os.environ.get("TABLE")


def handler(event, context):
    table = boto3.resource("dynamodb").Table(event.get("table") or TABLE)
    now = int(time.time())
    lease = int(event.get("lease_secs", 6 * 3600))
    exec_id = event.get("exec_id", context.aws_request_id)
    claimed, skipped = [], []
    for pkg in event["chain"]:
        try:
            table.update_item(
                Key={"pkg": pkg},
                UpdateExpression=(
                    "SET build_state=:b, lease_owner=:o, lease_expires_at=:le, "
                    "last_attempt_at=:now"
                ),
                ConditionExpression=(
                    "(attribute_not_exists(suppressed) OR suppressed = :false) AND "
                    "(build_state = :queued OR "
                    " (build_state = :building AND lease_expires_at < :now))"
                ),
                ExpressionAttributeValues={
                    ":b": "building", ":o": exec_id, ":le": now + lease,
                    ":now": now, ":false": False,
                    ":queued": "queued", ":building": "building",
                },
            )
            claimed.append(pkg)
        except table.meta.client.exceptions.ConditionalCheckFailedException:
            skipped.append(pkg)
    event["claimed"] = claimed
    event["skipped"] = skipped
    # Per-item objects for the Step Functions Map iterator (each carries its own
    # target so the iterator needn't index the target map by a dynamic key).
    event["claimed_items"] = [{"pkg": p, "target": event["target"][p]} for p in claimed]
    return event
