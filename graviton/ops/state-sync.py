#!/usr/bin/env python3
"""
state-sync.py -- Phase 1 of the DeBeOS staleness->rebuild pipeline.

Reads the staleness report (out/matched.tsv) produced by repology-staleness.sh
and upserts one DynamoDB item per matched package into the `debeos-package-state`
table. This is the detector->state bridge: it records the current staleness
snapshot and applies the SOP triage rules to set build_state, WITHOUT clobbering
state owned by the DevOps agent or a human.

Ownership contract (what this script will and won't touch):
  - ALWAYS refreshes the staleness snapshot: repology_status, hp_version,
    local_version, newest_upstream, drift, last_seen_at (+ first_flagged_at once).
  - Sets build_state=queued (target_version=newest_upstream) ONLY when the item
    is a candidate (outdated/vulnerable), not suppressed, and its current
    build_state is none/unset OR a terminal state (built/failed) whose recorded
    version != the new target (i.e. a newer upstream appeared -> re-queue).
  - NEVER overrides: suppressed=true (human/agent), build_state in
    {building, needs_human}, or a built/failed state whose version still matches
    the current target (that's backoff / already-done).

Idempotent: re-running with the same report produces no churn.

Usage:
  TABLE=debeos-package-state AWS_PROFILE=... python3 state-sync.py [out/matched.tsv]
  --dry-run   print intended writes, touch nothing
"""
import argparse
import os
import sys
import time

MATCHED_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "matched.tsv")
TERMINAL = {"built", "failed"}
PROTECTED_STATES = {"building", "needs_human"}


def parse_version(v):
    """Loose version tuple for comparison. Splits on non-alnum, pads numerics.

    Good enough to answer "is local >= newest" for the local-ahead suppression
    heuristic; NOT a full RPM/Haiku vercmp. Ties/ambiguity fall through to
    NOT-suppressed (safer: we'd rather flag than silently drop)."""
    import re
    parts = re.split(r"[^0-9A-Za-z]+", v.strip())
    out = []
    for p in parts:
        if not p:
            continue
        if p.isdigit():
            out.append((1, int(p), ""))
        else:
            # split leading digits from suffix (e.g. 9c -> 9,'c')
            m = re.match(r"(\d+)([A-Za-z].*)?", p)
            if m and m.group(1):
                out.append((1, int(m.group(1)), m.group(2) or ""))
            else:
                out.append((0, 0, p))
    return out


def ge(a, b):
    """a >= b under the loose scheme; returns None if incomparable."""
    try:
        return parse_version(a) >= parse_version(b)
    except Exception:
        return None


def triage(row):
    """Given a report row, return (is_candidate, suppress_reason_or_None)."""
    status = row["status"]
    candidate = ("outdated" in status) or ("vulnerable" in status)
    if not candidate:
        return False, None
    # SOP: local already at/ahead of the fix target -> likely already-fixed
    # false positive (e.g. gcc/binutils stale bootstrap entry, or libvpx built
    # ahead of haikuports_master). Suppress with an explicit reason.
    cmp = ge(row["local_version"], row["newest_upstream"])
    if cmp is True and row["local_version"] != row["newest_upstream"]:
        return True, "local-ahead-of-target"
    return True, None


def read_report(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            f = line.split("\t")
            # matched.tsv: name status hp_ver local_ver newest_upstream drift
            if len(f) < 6:
                continue
            rows.append(dict(name=f[0], status=f[1], hp_version=f[2],
                             local_version=f[3], newest_upstream=f[4], drift=f[5]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report", nargs="?", default=MATCHED_DEFAULT)
    ap.add_argument("--table", default=os.environ.get("TABLE", "debeos-package-state"))
    ap.add_argument("--region", default=os.environ.get("AWS_REGION", "us-west-2"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    rows = read_report(args.report)
    if not rows:
        print(f"state-sync: no rows in {args.report}", file=sys.stderr)
        return 1

    now = int(time.time())
    stats = dict(queued=0, requeued=0, already_queued=0, suppressed=0,
                 skipped_terminal=0, skipped_protected=0, snapshot_only=0)

    table = None
    if not args.dry_run:
        import boto3
        table = boto3.resource("dynamodb", region_name=args.region).Table(args.table)

    for row in rows:
        pkg = row["name"]
        candidate, suppress_reason = triage(row)

        cur = {}
        if table is not None:
            resp = table.get_item(Key={"pkg": pkg})
            cur = resp.get("Item", {})

        cur_state = cur.get("build_state", "none")
        target = row["newest_upstream"]

        # Decide the build_state transition (state-sync's narrow authority).
        new_state = None
        new_target = None
        set_suppress = None

        if cur.get("suppressed"):
            action = "skip:suppressed"
            stats["snapshot_only"] += 1
        elif cur_state in PROTECTED_STATES:
            action = f"skip:{cur_state}"
            stats["skipped_protected"] += 1
        elif not candidate:
            action = "snapshot-only:newest"
            stats["snapshot_only"] += 1
        elif suppress_reason:
            set_suppress = suppress_reason
            action = f"suppress:{suppress_reason}"
            stats["suppressed"] += 1
        elif cur_state in TERMINAL:
            if cur.get("target_version") != target:
                new_state, new_target = "queued", target
                action = "requeue:new-target"
                stats["requeued"] += 1
            else:
                action = f"skip:{cur_state}@{target}"  # backoff / already built
                stats["skipped_terminal"] += 1
        elif cur_state == "queued":
            # Already queued: only rewrite (and reset queued_at) if the target
            # version moved. Otherwise leave it untouched -> idempotent, no GSI
            # churn on the weekly re-run.
            if cur.get("target_version") == target:
                action = "skip:already-queued"
                stats["already_queued"] += 1
            else:
                new_state, new_target = "queued", target
                action = "requeue:new-target"
                stats["requeued"] += 1
        else:  # none / unset
            new_state, new_target = "queued", target
            action = "queue"
            stats["queued"] += 1

        print(f"  {pkg:24s} {row['status']:20s} local={row['local_version']:14s} "
              f"target={target:14s} -> {action}")

        if args.dry_run:
            continue

        # Build the update: always the snapshot, conditionally the state fields.
        expr = ["repology_status=:rs", "hp_version=:hv", "local_version=:lv",
                "newest_upstream=:nu", "drift=:dr", "last_seen_at=:now",
                "first_flagged_at=if_not_exists(first_flagged_at,:now)"]
        vals = {":rs": row["status"], ":hv": row["hp_version"],
                ":lv": row["local_version"], ":nu": target, ":dr": row["drift"],
                ":now": now}
        if new_state is not None:
            expr += ["build_state=:bs", "target_version=:tv", "queued_at=:now"]
            vals[":bs"] = new_state
            vals[":tv"] = new_target
        elif "build_state" not in cur:
            expr.append("build_state=if_not_exists(build_state,:none)")
            vals[":none"] = "none"
        if set_suppress is not None:
            expr += ["suppressed=:sup", "suppress_reason=:sr"]
            vals[":sup"] = True
            vals[":sr"] = set_suppress

        table.update_item(
            Key={"pkg": pkg},
            UpdateExpression="SET " + ", ".join(expr),
            ExpressionAttributeValues=vals,
        )

    print("\nstate-sync summary:", ", ".join(f"{k}={v}" for k, v in stats.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
