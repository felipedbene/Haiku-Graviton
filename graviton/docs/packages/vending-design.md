# DeBeOS package management & vending — design

How DeBeOS builds, activates, and distributes native `.hpkg` packages on arm64, what is proven on
hardware today, and the remaining client work needed to install from a remote (CDN-fronted) repo.

Everything below marked **[proven]** was verified on real Graviton hardware; **[design]** is the
target architecture; **[gap]** is a measured client limitation with a fix plan.

## 1. Native package manager — local install **[proven]**

Two defects historically made *"nothing built can be `pkgman install`ed"* on arm64. Both are fixed
and hardware-verified:

- **Non-dirty revision.** Every built `.hpkg` records `requires: haiku >= <rev>`. When the build tree
  is `-dirty` (uncommitted), `git describe --dirty` stamps a `_dirty` suffix into `<rev>`, and the
  version comparator ranks the clean image *below* the `_dirty` requirement → unsatisfiable, so the
  solver refuses every package. Fix: build the image **and** the packages from the same committed,
  non-dirty revision (pin `HAIKU_REVISION` at the bake step and inside the build chroot). With that,
  locally built packages install.
- **Local repo for install-by-name.** arm64 has no usable upstream repo, so name resolution needs a
  local index. Build one with Haiku's `package_repo` and register it as a `file://` repo:

  ```sh
  # repo.info (values largely free-form; vendor must match every package — see rule below)
  #   name <repo-name> / vendor "<Vendor>" / summary "..." / priority 1
  #   url <stable-identifier-url> / architecture arm64
  package_repo create repo.info packages/*.hpkg          # -> ./repo
  sha256sum repo | sed -E 's,([^[:space:]]*).*,\1,' > repo.sha256
  # lay out:  <dir>/repo  <dir>/repo.sha256  <dir>/packages/*.hpkg
  pkgman add-repo file:///<dir>
  pkgman refresh
  pkgman install <name>          # resolves deps + activates live (no reboot)
  ```

  Verified end-to-end: `refresh` → `search` → `install <name>` with dependency resolution and **live
  activation**, installing packages that were not on the image.

**Rule — one vendor per repo.** `package_repo create` aborts if any package's `vendor` differs from
`repo.info`'s `vendor`. Host `Haiku Project` and DeBeOS-vendor packages (e.g. `rust_bin`) in
**separate repos**, or normalise vendor at build time.

## 2. Remote vending over a CDN — architecture **[design]**

A repo is just a directory tree; publishing it is standard AWS. The bucket stays **private** and a CDN
serves it, so **no public-bucket exception is needed**.

```
Publish (on a Haiku host, which has package_repo):
   package_repo create repo.info packages/*.hpkg
   sha256sum repo | sed ... > repo.sha256
   aws s3 sync {repo, repo.sha256, packages/} -> s3://<bucket>/<repo-path>/   (private, BPA on)
   CloudFront invalidation of /repo and /repo.sha256

Serve:
   CloudFront distribution (Origin Access Control) -> private S3 origin
   base URL = https://<distribution-domain>/<repo-path>

Consume (target):
   pkgman add-repo https://<distribution-domain>/<repo-path>
   # pkgman fetches /repo, /repo.sha256, and packages from /packages/<file>
```

S3 layout under the repo path:

```
<repo-path>/
  repo            # package_repo index      (mutable  -> no-cache / invalidate on publish)
  repo.sha256     # sha256 of repo          (mutable  -> no-cache / invalidate on publish)
  repo.info       # optional; identifier/vendor/arch
  packages/*.hpkg # immutable, versioned    (long cache TTL)
```

**Verified [proven]:** the private-bucket → OAC → CloudFront path serves the full tree (`repo`,
`repo.sha256`, `packages/*.hpkg`) with HTTP `200` and a self-consistent `sha256(repo) == repo.sha256`,
over both HTTP and HTTPS. The **infrastructure is correct**; the blocker is the client (§3).

**CDN cache rule.** `*.hpkg` are immutable (versioned filenames) → cache long. `repo` and `repo.sha256`
change on every publish → serve `no-cache`/short-TTL or issue a CloudFront invalidation for those two
keys on each publish, or a stale index/checksum yields a `Bad data` / checksum-mismatch on refresh.

## 3. Remote fetch over HTTP — **[proven]**, and what the "EINTR" really was

HTTP remote install works today with **no client change**. On real hardware, against the CDN:
`pkgman add-repo http://<cdn>/<repo-path>` → `refresh` → `install <name>` resolved dependencies,
downloaded the hpkgs from `/<repo-path>/packages/`, validated checksums, and **activated live**.

The earlier "the HTTP fetch aborts with *Interrupted system call*" report was a **phantom**. The fetch
was never interrupted: `pkgman` mis-reports an **HTTP 4xx** as `B_INTERRUPTED`. When `fOptStopOnError`
is set and the status is ≥ 400, `HttpRequest.cpp` sets `fQuit = true` and returns `B_INTERRUPTED`
("Interrupted system call") — so a plain *not found* surfaces as a syscall-interrupt error. The 4xx
itself came from a **missing `repo.info` on the CDN**: only `repo` + `repo.sha256` had been uploaded,
so `<base>/repo.info` returned **403** (S3 behind OAC returns 403, not 404, for an absent key). The
fix was to **publish `repo.info`** — one file, no code, no bake.

> Lesson baked into tooling: `haiku-repo-publish` always emits and uploads `repo.info`, and uploads
> the index files *after* the packages, so a client refreshing mid-publish never sees a dangling index.

The two `EINTR`-restart commits (`BSocket::Read/Write`, `BAbstractSocket::Connect`) were chasing this
phantom. They are harmless socket hardening and were kept, but they were **not** the fix.

## 4. The one remaining gap — HTTPS on arm64 **[gap, parked]**

`pkgman add-repo https://…` still fails with **"Operation not supported"**: the `@minimum-mmc` arm64
image links `libnetapi` without SSL (`BSecureSocket` is a `B_UNSUPPORTED` stub) because the `openssl`
build feature is gated on `IsPackageAvailable openssl3_devel`, which arm64 did not advertise. This is
the parked **openssl/TLS build-feature** task (cooking via the feature-cook workflow). It is *not* on
the modular-AMI critical path — **HTTP vending is sufficient** and TLS is later hardening. When it
lands, the repo base URL flips from `http://` to `https://` (one line in
`data/settings/package-repositories/DeBeOS`).

## 5. Interim / offline: distribute via S3 as a `file://` repo **[proven]**

Where HTTP to the CDN is undesirable (air-gapped, or pre-TLS on a sensitive network), the same repo
installs locally: `aws s3 sync` (or `haiku-s3 sync-down`) the repo tree onto the target, then
`pkgman add-repo file://<local-dir>`. Same index, local fetch — proven with dependency resolution and
live activation. This is a fallback, not the default: §3 (HTTP CDN) is the modular-AMI path.

## 6. Building & publishing the repo — `haiku-repo-publish`

The `repo` index is a Haiku-tool artifact, so the repo is built on a **Haiku host** (the dev AMI or a
metal build guest) and published to the CDN from there. `graviton/scripts/haiku-repo-publish`
automates all of §2–§3:

```sh
# on a Haiku host with `package` + `package_repo`, POOL = the closure SET
haiku-repo-publish build   --pool <closure-dir> --out /tmp/debeos-repo
# then, with creds + config in the environment (no account names in the tool):
HG_REPO_S3=s3://<bucket>/<prefix>/arm64 HG_CF_DIST=<dist-id> \
    haiku-repo-publish publish --out /tmp/debeos-repo
```

It **re-stamps every hpkg to vendor `DeBeOS`** (metadata repackage, no rebuild) so the whole toolset
lives in **one** repo despite mixed upstream vendors (§1's one-vendor rule), writes `repo.info` with
`url http://packages.debene.dev/arm64`, builds the index + `repo.sha256`, uploads packages-then-index,
and invalidates the CloudFront index keys. Feed `--pool` the closure computed by
`haiku-package-closure`, not a raw pool, so the repo contains exactly what installs resolve against.

The end-to-end sequence (closure → build missing → publish → lean bake → prove → promote) is the
`graviton/docs/packages/modular-ami-buildout.md` runbook.
