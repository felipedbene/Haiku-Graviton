# Modular AMI build-out — runbook

How DeBeOS goes from the current **fatty** AMI (every feature baked into the image) to a **modular**
one: a lean base image plus a CDN repo that vends every feature as an installable package. The end
state is capability parity with the fatty image, reached by `pkgman install` on demand instead of at
bake time.

This is a **gated** runbook. Each phase produces an artifact you inspect before the next; nothing here
touches the canonical AMI until the final human-gated promotion (see
`graviton/audit/agent-sops/debeos-hardware-proof.sop.md`).

## The design in one paragraph

The **lean base** is `@minimum-mmc` + DeBeOS patches — it already ships pkgman, `package_daemon`,
`openssl3`, the network stack, ENA, and the boot chain, so it needs **no functional change**. The one
image addition is a pre-registered repo config (`data/settings/package-repositories/DeBeOS` →
`http://packages.debene.dev/arm64`), so a fresh boot can `pkgman refresh && pkgman install <pkg>` with
no manual `add-repo`. Everything else — WebKit, the image translators, the dev toolchain (git, cmake,
ninja, python, ruby, perl, vim), rust, netsurf, fonts, OpenSSH, editors — is a **repo package**, not
in the image. A lean bake is the existing bake *without* staging the hpkg pool (the buildspec already
has that empty-pool "SSH-only" path) plus the baked config.

**Split invariant:** the packages the base image activates are the closure resolver's *stop-set*. No
package appears in both the image and the repo.

## Tooling (all under `graviton/scripts/`)

| Tool | Role |
|------|------|
| `haiku-package-closure` | Deterministic dependency closure of a package selection over a pool, against what the base already provides. Reports the closure **and** anything MISSING. One computed set replaces the "add package → bake fails on a missing dep → add dep → fail" loop. |
| `haiku-repo-publish` | Builds the single DeBeOS-vendored repo from a closure set (re-stamps every hpkg to vendor `DeBeOS`, writes `repo` + `repo.info` + `repo.sha256`), then publishes to S3 + invalidates CloudFront. Runs on a Haiku host. |
| `haiku-s3` | Fast S3 transfers via `s5cmd` with a transparent `aws s3` fallback. Used by `haiku-repo-publish` for the upload. |
| `haiku-stage-local-packages` | (Fatty path only) stages the pool into a bake's `download/` so build features turn on. A lean bake skips this. |

## Prerequisites

- A running **dev AMI** or spun-up native Graviton builder (has `package`, `package_repo`, and a
  full hpkg pool). Native package builds run on such a Haiku host over SSM (`haiku-nativebuild`),
  not on a shared metal builder (that host has been retired).
- The CDN provisioned: private S3 bucket + CloudFront (OAC) serving `http://packages.debene.dev/arm64`.
- Admin/describe creds for the test account (describe/list unless a change is intended).

## Phase 0 — parity scope (define before building)

"Fatty parity" is a **capability checklist**, not every nightly app. The target set is the DeBeOS
README capability list: dev toolchain, rust, WebKit/WebPositive, the image translators, fonts,
OpenSSH, common editors. Write this as the `--want` list. Do **not** pad it with nightly desktop apps
(pe, wonderbrush, bepdf, vision, …) — those are not part of parity and inflate the MISSING count.

## Phase 1 — closure

On the dev AMI, compute what the base already provides, then close the `--want` set over the pool:

```sh
# base-provides = the activated packages of a lean @minimum-mmc image
pkgman list-installed | ... > base-provides.txt      # provide-tokens, one per line
haiku-package-closure --pool <hpkg-pool> --package-tool "$(command -v package)" \
    --base base-provides.txt \
    --want webkit,rust_bin,git,cmake,ninja,python,pip_python3.14,setuptools_python3.14,ruby,perl,vim,openssh,<...> \
    > closure.json
```

> Include `pip_python3.14` and `setuptools_python3.14` in `--want` whenever `python` is wanted
> (DeBeOS #552): the `python3.14` package ships neither on the import path, so `python3 -m pip`
> and `import setuptools` fail without them. `setuptools_python3.14` is already in the pool;
> `pip_python3.14` is built from the ensurepip-vendored wheel by
> `graviton/scripts/haiku-build-pip-hpkg` (HaikuPorts has no standalone pip recipe) and must be in
> the pool before this closure resolves it.

- Exit 0 → the selection is fully satisfiable from the pool; `closure_files` is the **repo set**.
- Exit 2 → `missing` lists providers nothing supplies. Those are **Phase 2**.

Inspect `closure.json` before proceeding. The real MISSING list is expected to be small (most parity
features are already in the ~399-hpkg pool).

## Phase 2 — build the MISSING packages (native, on the dev AMI)

For each MISSING provider, build it natively with haikuporter (`--all-dependencies`), then drop the
resulting hpkg into the pool and **re-run Phase 1** until the closure is clean (exit 0). Recycle build
guests by halting the OS, not by `ec2:StopInstances`.

## Phase 3 — publish the repo

Point `--pool` at the closure set (not the raw pool), build the DeBeOS-vendored repo, and publish:

```sh
mkdir -p /tmp/repo-set
# copy exactly closure_files from closure.json into /tmp/repo-set
haiku-repo-publish build --pool /tmp/repo-set --out /tmp/debeos-repo
HG_REPO_S3=s3://<bucket>/<prefix>/arm64 HG_CF_DIST=<dist-id> \
    haiku-repo-publish publish --out /tmp/debeos-repo
```

Verify from outside: `curl` `repo`, `repo.info`, `repo.sha256` return `200`, and
`sha256(repo) == repo.sha256`. (`repo.info` **must** be present — its absence is a 403 that pkgman
mis-reports as "Interrupted system call"; see `vending-design.md` §3.)

## Phase 4 — bake the lean image

Bake `@minimum-mmc` with an **empty** hpkg pool so the buildspec takes its SSH-only path (no pool
staging → build features stay off → the image stays lean), plus the baked DeBeOS repo config. This
registers a **candidate** AMI (`candidate=true`; canonical is never set here).

## Phase 5 — prove parity on hardware

Boot a disposable instance from the candidate. Confirm the repo is pre-registered, then install the
parity set and verify capability:

```sh
pkgman refresh                       # reads the pre-baked DeBeOS repo over HTTP
pkgman install <parity set>          # resolves + activates from the CDN
# then exercise: webpositive renders; a rust/cmake project builds; translators load; ssh in
```

Parity = the capability checklist passes, **not** a package-count match. Follow
`debeos-hardware-proof.sop.md` for the evidence bar.

## Phase 6 — promote

Only after Phase 5 is green: `graviton/scripts/haiku-canonical promote <candidate-ami>` moves the
`canonical=true` tag atomically. Tear down the disposable test instance.

## Notes & hazards

- **Publish order matters.** `haiku-repo-publish` uploads packages before the index so a mid-publish
  refresh never sees an index pointing at absent packages. Keep that order in any manual publish.
- **CloudFront caching.** `repo`/`repo.info`/`repo.sha256` are mutable → invalidate them each publish;
  `*.hpkg` are versioned/immutable → cache long.
- **HTTP vs HTTPS.** The base URL is `http://` until the parked openssl/TLS build feature lands, then
  it flips to `https://` in the baked config. HTTP is sufficient for the modular AMI.
- **DNS.** `packages.debene.dev` is a Cloudflare-proxied CNAME to the CloudFront distribution; set the
  Cloudflare origin **Host** header to the CloudFront domain so the dist answers without a CNAME+ACM.
