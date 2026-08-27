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

## 3. The client gap — remote fetch on arm64 **[gap]**

Measured on hardware against a known-good CDN endpoint (verified `200` + checksum-valid from outside):

- **HTTPS → "Operation not supported".** No TLS/HTTPS URL-protocol handler is present in the
  `@minimum-mmc` image, so `pkgman add-repo https://…` fails immediately. (The built-in upstream
  `https://` repos give the identical error — so that was never a dead upstream, it was the client.)
- **HTTP → "Interrupted system call" (B_INTERRUPTED).** The HTTP handler *is* present — pkgman prints
  *"Fetching repository info from http://…"* and the fetch thread runs — but a network syscall returns
  `EINTR` and is not restarted, so the fetch aborts. `curl` on the same host fetches the same URL
  fine (it retries `EINTR`), confirming the network and TLS are healthy and the defect is in the
  package-kit fetch path, not the platform.

Net: `file://` repos work; remote (`http(s)://`) repos do not. This is why DeBeOS has been
`file://`-only, and it is the one thing standing between the working CDN and true remote install.

## 4. Fix plan — enabling remote install **[design]**

Two independent client fixes; both are ordinary DeBeOS source/build changes, provable on hardware:

1. **Include the HTTPS URL-protocol handler in the image.** Find where the `http(s)` `BUrlRequest`
   handler / network-services backend is provided and ensure it (and its TLS dependency) is part of
   the `@minimum-mmc` (and default) image definition, not only the larger profiles.
2. **Restart `EINTR` in the fetch path.** Locate the socket call in the package-kit fetch
   (`FetchFileJob` → the HTTP request → `BSocket`/`BHttpRequest`) that returns `B_INTERRUPTED` without
   retrying, and wrap it in a restart loop (the standard `while (r == B_INTERRUPTED) retry`). `curl`'s
   behaviour is the reference: transient signals must not abort the transfer.

Verification: bake a candidate image with both fixes, boot a disposable target, and run
`pkgman add-repo https://<cdn>/<repo-path>` → `refresh` → `install <name>` end-to-end. Promote only
after that passes, following `graviton/audit/agent-sops/debeos-hardware-proof.sop.md`.

## 5. Interim: distribute via S3 without the client fix **[proven]**

Until §4 lands, packages can still be distributed through S3: `aws s3 sync` the repo tree from the
bucket onto the target, then `pkgman add-repo file://<local-dir>`. Same repo, local fetch — proven to
install with dependency resolution and live activation.
