"""ReleaseLeasesOnFailure: hand this execution's unbuilt leases back to the queue.

Input : {table, claimed:[pkg,...], lease_owner}   (ClaimBatch's output, carried
        through every later state -- Catch and the Map both keep the parent input)
Output: adds {released:[...], release_kept:[...]} to the event.

Runs first on the shared failure path, before ReapOnFailure (issue 839). ClaimBatch
leases every chain port queued -> building for 6h; before this, the failure path only
terminated the instance. An execution that died before BuildChain -- a spot ICE in
LaunchBuilder, SSM never registering -- left every claimed port `building` with no
builder behind it. The wave driver reads `queued`, so those ports were invisible to it
until the lease expired: wd-10191705-w1-c05-1792429505 stranded 12 ports that way.

The revert is CONDITIONAL on `lease_owner = <ours> AND build_state = building`, which
is what makes it safe to run blind over the whole claimed list:

  * a port BuildChain already recorded (built, failed, needs_human, or the capacity
    re-queue) had its lease REMOVEd by record.py, so the owner check fails and the
    recorded outcome stands;
  * a port another execution re-claimed after our lease expired carries THEIR
    lease_owner, so we cannot clobber their claim (owners are per-invocation Lambda
    request ids, so they never collide).

NO attempt is counted and none of the failure attributes are written: no build ran
(or none finished and was recorded), so this is not a failure of the port. queued_at
is the by-build-state GSI's range key and is preserved -- `if_not_exists` only fills
it in on a row that somehow lacks it, never moves a real queue timestamp (#487).

Best effort, never raising: this sits in front of the reaper, and an exception here
must not cost us the instance termination. A row that could not be released keeps its
lease and simply expires as before; it is listed in `release_kept`.
"""
import os
import time
import boto3

TABLE = os.environ.get("TABLE")


def handler(event, context):
    claimed = event.get("claimed") or []
    owner = event.get("lease_owner")
    released, kept = [], []
    if not claimed or not owner:
        # Failed before ClaimBatch returned (or nothing was claimed): no lease is ours.
        event["released"], event["release_kept"] = released, list(claimed)
        return event
    table = boto3.resource("dynamodb").Table(event.get("table") or TABLE)
    now = int(time.time())
    for pkg in claimed:
        try:
            table.update_item(
                Key={"pkg": pkg},
                UpdateExpression=("SET build_state=:q, queued_at=if_not_exists(queued_at,:now) "
                                  "REMOVE lease_owner, lease_expires_at"),
                ConditionExpression="lease_owner = :o AND build_state = :b",
                ExpressionAttributeValues={":q": "queued", ":b": "building",
                                           ":o": owner, ":now": now},
            )
            released.append(pkg)
        except Exception as exc:  # noqa: BLE001 -- see docstring: never block the reap
            # ConditionalCheckFailed is the expected, correct outcome for a row that was
            # recorded or re-claimed; anything else is printed so the log says why.
            if "ConditionalCheckFailed" not in type(exc).__name__ + str(exc):
                print(f"ReleaseLeasesOnFailure: could not release {pkg}: {exc}")
            kept.append(pkg)
    print(f"ReleaseLeasesOnFailure: owner {owner} released {len(released)} "
          f"lease(s) {released}, kept {kept}")
    event["released"], event["release_kept"] = released, kept
    return event
