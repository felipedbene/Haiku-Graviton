"""RecordOutcome: write a build result to DynamoDB, applying SOP backoff/quarantine.

Input : {table, pkg, target, ok, error_class?, log_url?}
Output: adds {recorded_state} to the event.

- ok=True  -> build_state=built, target_version=<target>, built_at=now, clears lease
              AND clears the previous failure's residue (see below).
- ok=False -> but an hpkg for this package is in the harvest pool: treated as a
              SUCCESS (SOP §7), with corrected_by/correction_note provenance.
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

A FAILURE IS CHECKED AGAINST THE HARVEST POOL BEFORE IT IS BELIEVED (#505 §C).
SOP §7 says success is the hpkg existing, but nothing on the wave path applied that
rule: run_ssm's wrapper ends `exit $RC`, poll_ssm reads `ok = (verdict == "OK" and
rc == 0)`, and so the verdict believes the exit code. Two ports of the #492 re-wave
(`sip`, `cmake_haiku`) built their hpkg, had it harvested to the pool by
haiku-nativebuild's `harvest` -- which runs unconditionally, after the per-port
loop, whatever rc the loop produced -- and were still recorded `failed`. The run's
real build count was 7, not 5. `haiku-correct-false-failure` exists for exactly
this and is a MANUAL follow-up, so it did not run, and the rows are wrong to this
day. The check now happens where the outcome is decided: before a failure is
written, ask the pool whether this package's hpkg is there. Same three guards as
the manual tool, and for the same reasons:

  * the name is anchored on the leading component (`<pkg>-...`), so `cmake_haiku`
    is not satisfied by a `cmake_haiku_tools` hpkg;
  * the object must be larger than FALSE_FAILURE_MIN_BYTES (class-9's ~800 B
    empty-package trap must never be promoted to `built`);
  * and -- new here, because the manual tool is aimed by a human at one named
    package and this runs on every failure -- the object must be YOUNGER than
    FALSE_FAILURE_MAX_AGE_SECS. HARVEST_S3 is the durable harvest prefix: it holds
    hpkgs until a publish prunes them, so without a freshness bound a genuine
    failure at a new target would be "corrected" by the hpkg its own previous
    success left behind. The driver harvests minutes before this Lambda runs, so
    the window only has to cover one package's build-and-reap.

If HARVEST_S3 is unset, or S3 errors, the failure is recorded exactly as it was
before: this path can only ever turn a `failed` into a `built` on positive
evidence, never the other way round. Note what it cannot fix: `sip` produces
`sip_python310-*.hpkg`, which no leading-component match can tie to the key `sip`
-- a recipe/key naming mismatch that is a separate defect, not this one.

Every write that sets build_state also ensures `queued_at` exists: it is the
by-build-state GSI's range key, and DynamoDB silently omits from the index any item
missing it -- which is how a `needs_human` row (`qt5`) went missing from the triage
census for months (#487). `if_not_exists` so a real queue timestamp is never
overwritten by an outcome record.
"""
import os
import re
import time
import boto3

from pkgkey import validate_pkg_key

TABLE = os.environ.get("TABLE")
QUARANTINE_AT = int(os.environ.get("QUARANTINE_AT", "3"))

# The harvest pool haiku-nativebuild pushes finished hpkgs to (s3://<bucket>/<prefix>).
# Empty = the false-failure check is skipped entirely. See the module docstring.
HARVEST_S3 = os.environ.get("HARVEST_S3", "")
FALSE_FAILURE_MIN_BYTES = int(os.environ.get("FALSE_FAILURE_MIN_BYTES", "2048"))
FALSE_FAILURE_MAX_AGE_SECS = int(os.environ.get("FALSE_FAILURE_MAX_AGE_SECS", "21600"))

# Appended to every build_state-setting UpdateExpression. See module docstring.
_GSI_KEY = "queued_at=if_not_exists(queued_at,:now)"

# Attributes that describe the LAST FAILURE and must not outlive it. REMOVE on a
# missing attribute is a no-op in DynamoDB, so this is safe on a row that has
# never failed. See the module docstring for why each one is here.
_FAILURE_RESIDUE = ("last_error_class", "esc_reason", "log_url", "attempt_count")


def _hpkg_version(key):
    """cmake_haiku-git-4-arm64.hpkg -> 'git-4' (version + revision, sans arch).

    Same parse as haiku-correct-false-failure, so a row corrected here and a row
    corrected by hand carry the same `built_version` and the census cannot tell the
    two apart. Deliberately permissive about how a version is spelled: demanding a
    leading digit is what hid `cmake_haiku-git-4` from the driver in the first place.
    """
    base = key.rsplit("/", 1)[-1][:-len(".hpkg")]
    m = re.match(r".+?-(.+)-[^-]+$", base)
    return m.group(1) if m else None


def _harvested_hpkg(pkg, now):
    """Return (s3 uri, size) of the largest FRESH hpkg for `pkg` in the harvest pool.

    Returns None when there is none, when HARVEST_S3 is unset, or when S3 cannot be
    read -- the caller then records the failure unchanged. Listing is narrowed by the
    `<pkg>-` key prefix (so this is one cheap LIST, not a pool scan) and the basename
    is re-checked against the same anchored pattern, because a key prefix would also
    accept `<pkg>-foo/bar.hpkg` from some future nested layout.
    """
    if not HARVEST_S3.startswith("s3://"):
        return None
    bucket, _, prefix = HARVEST_S3[len("s3://"):].partition("/")
    if not bucket:
        return None
    prefix = prefix.strip("/")
    key_prefix = f"{prefix}/{pkg}-" if prefix else f"{pkg}-"
    want = re.compile(r"^" + re.escape(pkg) + r"-[^/]+\.hpkg$")
    best = None
    try:
        pages = boto3.client("s3").get_paginator("list_objects_v2").paginate(
            Bucket=bucket, Prefix=key_prefix)
        for page in pages:
            for obj in page.get("Contents", []):
                if not want.match(obj["Key"].rsplit("/", 1)[-1]):
                    continue
                if obj["Size"] <= FALSE_FAILURE_MIN_BYTES:
                    continue
                if now - int(obj["LastModified"].timestamp()) > FALSE_FAILURE_MAX_AGE_SECS:
                    continue
                if best is None or obj["Size"] > best[1]:
                    best = (f"s3://{bucket}/{obj['Key']}", obj["Size"])
    except Exception as exc:      # noqa: BLE001 -- see below
        # Never let the pool check decide the outcome by failing: a throttle or a
        # missing permission must leave the recorded result exactly as it would have
        # been without this check. Printed, not swallowed, so the CloudWatch log says
        # that the check did not run (a check that stops checking silently is the
        # defect this file already carries a comment about).
        print(f"RecordOutcome: harvest-pool check skipped for {pkg}: {exc}")
        return None
    return best


def handler(event, context):
    table = boto3.resource("dynamodb").Table(event.get("table") or TABLE)
    now = int(time.time())
    pkg = validate_pkg_key(event["pkg"], where="RecordOutcome")
    target = event["target"]

    # SOP §7: success is the hpkg existing. A reported failure is therefore only a
    # failure once the harvest pool agrees there is no hpkg (#505 §C). Checked BEFORE
    # the failure path so a false failure never bumps attempt_count -- three of them
    # would otherwise escalate a package that builds fine to needs_human.
    corrected = None
    if not event.get("ok"):
        corrected = _harvested_hpkg(pkg, now)

    if event.get("ok") or corrected:
        expr = ("SET build_state=:s, target_version=:t, built_at=:now, "
                "last_outcome=:o, " + _GSI_KEY)
        vals = {":s": "built", ":t": target, ":now": now, ":o": "built"}
        if corrected:
            # Additive provenance, in the same attributes haiku-correct-false-failure
            # writes: a corrected row must be auditable as corrected, not look like a
            # build that was reported OK.
            expr += (", built_version=:ver, corrected_by=:who, corrected_at=:now, "
                     "correction_note=:note")
            vals[":ver"] = _hpkg_version(corrected[0]) or "unknown"
            vals[":who"] = "RecordOutcome"
            vals[":note"] = (f"false failure: driver rc said failed, but {corrected[0]} "
                             f"({corrected[1]} B) was harvested (SOP §7, #505 §C)")
            event["false_failure_hpkg"] = corrected[0]
            print(f"RecordOutcome: {pkg} reported failed but {corrected[0]} "
                  f"({corrected[1]} B) is in the harvest pool -> recording built")
        expr += (" REMOVE lease_owner, lease_expires_at, " + ", ".join(_FAILURE_RESIDUE))
        table.update_item(Key={"pkg": pkg}, UpdateExpression=expr,
                          ExpressionAttributeValues=vals)
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
