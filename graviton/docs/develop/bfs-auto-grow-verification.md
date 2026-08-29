# BFS auto-grow — verification plan (fault-injection-centered)

The acceptance bar is NOT "df shows 40 GiB". It is: **the root filesystem is
consistent after a grow, AND after a power loss at every step of the grow.**
Consistency is judged by `checkfs`/`chkbfs` + the BFS `CheckVisitor` (the same
machinery the ENA fault-injection work used for its unwind proofs).

## Instruments
- `checkfs /boot` (userland) and `CheckVisitor` (kernel) — full structural check:
  bitmap vs. actual allocation, block_run validity, AG count invariant, log sanity.
- A grow **phase-abort hook** (test-only, compile-gated): abort the grow
  immediately after each phase boundary (2a-reserve, 3-evacuate, 4-log-relocate,
  5-bitmap-write, just-before-6-commit, just-after-6-commit), leaving the on-disk
  state as a real power-loss would.
- Snapshot/restore of the raw volume between trials (EBS snapshot or a raw image
  copy on the metal) so each abort starts from an identical known-good FS.

## Tiered acceptance (must pass in order)
### T0 — foundation (item 1, already merged)
In-capacity grow ≤32 MiB, mounted, quiescent: `df` widens, `checkfs` clean,
reboot-stable, second grow is a no-op. (Regression guard for the merged primitive.)

### T1 — large grow, clean (no crash)
Offline grow 19→40 GiB (item 2, offline path): after grow, mount, `checkfs`
clean, pre-existing files **byte-identical** + attributes intact (checksum a
manifest before/after), `num_ags` invariant holds, reboot-stable, idempotent.

### T2 — CRASH INJECTION (the real bar)
For EACH phase boundary above: start from the known-good snapshot, run the grow
with the abort hook firing at that boundary, then MOUNT and require ONE of:
- **rolled back**: superblock = old geometry, `checkfs` clean, all data intact; or
- **completed on replay**: recovery finished the grow, `checkfs` clean, data intact.
NEVER: a mountable-but-inconsistent FS, a `checkfs` error, or lost/dangling data.
Repeat each boundary N times. Any single corruption = design defect, do not ship.

### T3 — online path (only if online is pursued; offline is preferred)
Grow while mounted r/w under a light write load; item 3 (`block_cache_set_size`)
exercised; writes into the new tail verified readable after; `checkfs` clean.
Plus T2-style crash injection with the write load running.

### T4 — end-to-end first-boot (items 5+6 tie-in)
Canonical AMI on a 40 GiB EBS: first-boot hook fires → GPT partition grows →
BFS grows → `df /boot` ~40 GiB, `checkfs` clean, second boot a no-op.

## Gate
Merge only after T1 + **T2 (all boundaries, N repeats, zero corruption)** pass on
real Graviton. T0 is the per-change regression guard. Until then the destructive
path stays `B_NOT_SUPPORTED`.
