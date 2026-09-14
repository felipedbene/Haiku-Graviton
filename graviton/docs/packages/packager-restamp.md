# Packager metadata re-stamp (pool hygiene, issue #41)

DeBeOS vends a single-vendor package pool: every pooled `.hpkg` is re-stamped to
`vendor "DeBeOS"` (`haiku-repo-publish revendor`). The **`packager`** field was
never normalised the same way, so packages built across the bootstrap/native
history carry a spread of inherited packager strings. That misattributes the
packager of DeBeOS-vended packages.

This is a pure **metadata re-stamp** — no rebuild. Contents, version,
dependencies, and the `vendor` field are untouched; only the `packager`
attribute changes, via the same `package extract` → edit `.PackageInfo` →
`package create` repack that `revendor` uses for the vendor field.

## Tools

- **`graviton/scripts/hpkg_meta.py`** — pure-Python HPKG metadata reader (name,
  version, vendor, packager). Parses the package-attributes section straight out
  of the file per `docs/develop/packages/FileFormat.rst`; needs **no Haiku host
  tool**, and can read via ranged callbacks so a remote pool is scanned from just
  each object's header + tail (a few hundred MB, not the whole ~6 GB pool).
- **`graviton/scripts/haiku-repo-packager-audit`** — read-only auditor. Scans an
  S3 `packages/` prefix (ranged GETs) or a local dir and reports the packager
  distribution and the re-stamp candidates. Produces the dry-run evidence.
- **`graviton/scripts/haiku-repo-restamp-packager`** — the re-stamp. `--dry-run`
  (default) reports; `--apply` rewrites each `.hpkg` in a local `--dir` in place
  whose packager != target, using the Haiku `package` tool. Carries the same
  decompression-bomb guard as `haiku-repo-add` (#168).

## Dry-run result (full pool, 2026-09)

Scanning `s3://<pool>/debeos-repo/arm64/packages/` (1885 objects, all parsed):

| count | current `packager` |
|------:|--------------------|
| 1442 | `DeBeOS <graviton@haiku-os.org>` |
|  249 | `DeBeOS <debeos@example.invalid>` |
|  177 | `Haiku Graviton <graviton@haiku-os.org>` |
|    5 | `The Haiku build system <build-system@haiku-os.org>` |
|    5 | `DeBeOS <debeos@debene.dev>` |
|    4 | `DeBeOS Haiku-Graviton` |
|    2 | `DeBeOS <https://github.com/felipedbene/haiku-mgmt-agent>` |
|    1 | `The Haiku build system` |

The whole pool (all 1885) carries a non-canonical packager, in **eight** distinct
variants — not just the "Haiku Graviton" set called out in the issue (177 exact
matches today; the issue's 363 was an earlier snapshot). The fix normalises all
of them to one string: **`DeBeOS <packages@debene.dev>`** (override with
`--packager` / `HG_PACKAGER`).

Reproduce:

```sh
export AWS_PROFILE=haiku-graviton
graviton/scripts/haiku-repo-packager-audit \
    --s3 s3://<bucket>/debeos-repo/arm64/packages --json /tmp/packager-audit.json
```

## Applying it (human-gated)

Re-stamping the LIVE pool is a single-flight publish operation (SOP §3) and is
**human-gated** — this tooling never uploads or deletes in S3. The procedure:

1. Confirm no other publisher is running (SOP §3).
2. Pull the pool to a work dir (`aws s3 sync .../packages/ pool/`).
3. `haiku-repo-restamp-packager --dir pool --apply` on a Haiku host (or a Linux
   host with the banked `package` tool via `--package-tool`). Decompression
   bombs (`0ad_data`, `yab_ide`, …) are skipped and reported.
4. Re-publish the index over the whole set and invalidate the CDN with the
   existing publish tooling.
5. Re-run the auditor against S3 to confirm a single packager string remains.

A durable follow-up is to also set the packager in `haiku-repo-publish`'s
`revendor` pass so future publishes stay canonical without a separate sweep.
