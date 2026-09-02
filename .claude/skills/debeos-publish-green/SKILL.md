---
name: debeos-publish-green
description: Publish newly-built DeBeOS hpkgs to the green package pool — chunked, single-flight, verified. Use for "publish these packages", "push the wave output to green", "update the repo with the new builds". Wraps haiku-repo-publish-ephemeral with the SOP §3 safeguards.
---

# Publish to the green pool

Add built hpkgs to the green pool `s3://haiku-graviton-hpkg-668984504585/debeos-repo-green/arm64/`
so prod (packages.debene.dev, prod dist `EZBGMRSQZAMKW` origin `/debeos-repo-green`)
and beta (`E531VGN7VDNBL`) serve them. **Read `graviton/ops/SOPs.md` §3 first.**
`export AWS_PROFILE=haiku-graviton`; run tooling from `origin/graviton` (§8).

## Guardrails (all mandatory — SOP §3)
1. **Single-flight.** Confirm no other publisher is running
   (`ec2 describe-instances Name=tag:Name,Values=haiku-repo-publisher …
   running,pending`) and wait if one is. Across your own fan-out, exactly one
   agent publishes — concurrent publishers race and strand packages.
2. **Chunk to ≤18 packages per `haiku-repo-add`.** The `package` re-stamp fills
   the publisher's `/dev/shm` (~1.9 GB) and dies at ~26 pkgs regardless of EBS
   (#168); ≤18 clears it.
3. **Exclude decompression bombs (>300 MB uncompressed):** `0ad_data`,
   `openarena_data`, `ayat_recit_ghamadi`, `another_world`, `yab_ide`,
   `vvvvvv_data` → leave `needs_human` (#168).
4. **Size the publisher root for the whole pool** (the index rebuild syncs it
   all): `HG_PUBLISHER_DISK_GIB` ≥ 40–60 as the pool grows, or it ENOSPCs.
5. **Only publish verified artifacts.** The `.hpkg` must exist and be real
   (§7) — a zero exit through `| tail` is not proof.

## Run
- `haiku-repo-publish-ephemeral` with `HG_REPO_S3=…/debeos-repo-green/arm64`,
  `HG_CF_DIST=E531VGN7VDNBL`, in ≤18-package chunks (serialized).
- Deps-first ordering when a chunk unblocks many dependents.

## Verify (against S3, not the log)
Count objects / read the index in the green prefix after; confirm the count rose
by the number published and the new packages resolve. Invalidate the serving
dist's index paths. Leave blue untouched; do not flip prod OriginPath here.
