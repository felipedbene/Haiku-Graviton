# DeBeOS package portal (issue #596)

A single static page that answers *"is X packaged, which version, how do I
install it"* at the root of `packages.debene.dev`, without booting DeBeOS.

- **`index.html`** — the whole portal. Inline CSS, vanilla JS, no build step, no
  backend, no `packages.json`, no generator. The browser fetches `/arm64/repo`
  (the same HPKR index `pkgman` reads), parses it client-side, and searches it.
- **`test-parser-equality.mjs`** — the acceptance test (below).

There is **no publish-path change**. The page is one more static object served
by the same CloudFront distribution that already vends the repo.

## How it works

On load the page does `fetch("/arm64/repo", { cache: "no-cache" })` (revalidate,
so a fresh publish shows up immediately), parses the HPKR index in the browser,
and renders a searchable table. The parser — between the
`/*HPKR_PARSER_START*/` … `/*HPKR_PARSER_END*/` sentinels in `index.html` — is a
JS port of `graviton/scripts/hpkg_meta.py` `read_repo`: HPKR v2 header, zlib
heap chunks (via `DecompressionStream`), the shared-strings table, and the
attribute tree. Per package it extracts name, version-revision, architecture,
summary (attribute id 16, which `read_repo` does not read), and provides.

Search is case-insensitive and ranked: exact `cmd:`/`app:` provide → exact name
→ name prefix → name substring → summary substring. The query lives in the URL
as `?q=`. `_devel`, `_debuginfo` and `source` packages are hidden by default
(toggle to show all). Rows are capped (200) so a bare query never paints all
~3,173 packages at once. States cover loading, no-results (with a prefilled
new-issue link), index-unreachable (incl. 403), and unparseable/unknown HPKR
version.

## Test

```sh
cd graviton/portal
node test-parser-equality.mjs            # downloads the live index
node test-parser-equality.mjs /path/to/repo   # or use a local copy
```

The test extracts the **exact** parser block shipped in `index.html` and evals
it (so there is no second implementation to drift), then asserts its output
equals `hpkg_meta.read_repo` on the live index for every package — name,
version, revision, arch, provides, requires — and that `?q=go` ranks `golang`
first, `?q=git` ranks `git` first, and a nonsense query returns nothing.

Last run (live index, 2026-10-04 publish): **3,173 packages compared, 0
mismatches**; `go→golang`, `git→git`, nonsense → no results. `PASS`.

## Deploy (maintainer step — the only infra change)

1. Upload `index.html` to the **root of the served (green) pool** — the S3
   prefix that CloudFront serves as `packages.debene.dev/` (one level above
   `arm64/`), e.g. `aws s3 cp index.html s3://<bucket>/<prefix>/index.html`
   with `--content-type text/html`.
2. Set the CloudFront **default root object** to `index.html` so `GET /`
   returns the page instead of 403.
3. Keep the viewer protocol policy **allowing plain HTTP** — `pkgman` clients
   add the repo over `http://`, and the page's `add-repo` hint is always
   `http://`. Do not force a HTTP→HTTPS redirect.
4. Invalidate `/` and `/index.html` on update.

### Verified safe to deploy once

- **Pool GC leaves it in place.** `haiku-pool-gc` only ever targets `*.hpkg`
  keys and its `--apply` is inert (prints a plan, never deletes), so it cannot
  remove `index.html`.
- **Blue→green promotion leaves it in place.** `haiku-repo-promote-green`
  promotes by incremental `haiku-repo-add` into the green prefix; it never runs
  a prefix-wide `s3 sync --delete`, and its only `s3 rm` is a scoped sweep of
  the disposable incoming prefix. A root `index.html` is untouched.
- **Beta:** if beta shares this origin path it gets the page for free; if it is
  a separate prefix, upload `index.html` there too.
