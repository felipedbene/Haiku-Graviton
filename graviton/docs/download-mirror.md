# DeBeOS HaikuPorts source mirror (issue #172)

## The problem

haikuporter appends a fallback SOURCE_URI built from its `DOWNLOAD_MIRROR`
config whenever a recipe's primary upstream is tried:

```
$DOWNLOAD_MIRROR/<recipeDir>/<sourceBasename>[#fragment]
```

(`HaikuPorter/Source.py` — `recipeDir` is `basename(port.baseDir)`, i.e. the
port's directory name; `sourceBasename` is the basename of the recipe's first
`SOURCE_URI`, minus any `#…` fragment.)

haikuporter's compiled-in default is `https://ports-mirror.haiku-os.org`, which
is now **permanently NXDOMAIN**. So for any port whose primary upstream URL has
rotated, the fallback resolves nothing and the source fetch dies — a recurring
class of wave failures. The per-tarball S3 srccache (issue #162,
`haiku-srccache-fetch`) is a stopgap that helps only when the exact fetch URL was
seen before; it does not give haikuporter a working fallback host.

## The mirror

A DeBeOS-owned mirror, laid out to match haikuporter's fallback URL exactly:

| Piece | Value |
|---|---|
| Host | `https://sources.debene.dev` |
| Origin bucket / prefix | `s3://haiku-graviton-668984504585-us-west-2/download-mirror/<recipeDir>/<file>` |
| CloudFront distribution | `E1CR5FIE6NETHG` (domain `d1a6zp21bblqer.cloudfront.net`), OriginPath `/download-mirror`, OAC `debeos-sources-mirror-oac` |
| ACM cert (us-east-1) | `sources.debene.dev`, DNS-validated |
| DNS | Cloudflare zone `debene.dev`, `sources` CNAME → the dist, **proxied OFF** (DNS-only), mirroring `packages.debene.dev` |
| Bucket access | private bucket (full public-access-block ON); a prefix+dist-scoped `s3:GetObject` grant to `cloudfront.amazonaws.com` on `download-mirror/*` with `AWS:SourceArn` = the dist |

`haiku-provision-native-builder` writes `DOWNLOAD_MIRROR="https://sources.debene.dev"`
into every builder's `haikuports.conf` (the value is passed by
`haiku-bake-builder` at bake time, not baked into the tracked provisioner file).
This takes effect on the next builder-AMI bake.

## How it gets populated (never with an unverified file)

Two complementary, checksum-safe paths — nothing reaches the mirror unless its
bytes match a pinned recipe `CHECKSUM_SHA256`:

1. **Always-on, per build** — `haiku-nativebuild`'s `mirror_sources` step. After
   a port builds OK (`.hpkg` produced), haikuporter has already fetched **and**
   checksum-validated every source in that port's `download/` dir; those files
   are promoted to `download-mirror/<recipeDir>/<file>`. A port that did not
   build is never mirrored.

2. **Batch backfill / periodic sweep** — `graviton/scripts/haiku-mirror-sync`.
   Content-addressed: it indexes every `CHECKSUM_SHA256` in the ports tree, then
   for each object in the #162 srccache downloads the bytes, computes the sha256,
   and promotes it **only if** that sha256 equals a checksum some recipe pins.
   The mirror key's basename is the srccache object's own basename (the fetch-URL
   basename == the URI basename haikuporter requests). A rotated/truncated/poisoned
   cache entry matches no pinned checksum and is never uploaded. Idempotent
   (skips objects already present at the same size); safe to re-run after each
   wave to sweep newly-cached sources into the mirror layout.

   ```
   haiku-mirror-sync \
       --tree /path/to/haikuports \
       --srccache s3://haiku-graviton-668984504585-us-west-2/srccache \
       --mirror   s3://haiku-graviton-668984504585-us-west-2/download-mirror
   ```

haikuporter re-verifies whatever it fetches **from** the mirror against the
recipe checksum anyway, so this is belt-and-braces.

## Deferred

The ~77 ports whose upstream is dead **and** which have never been fetched into
the srccache are not recoverable by this mirror (there is nothing verified to
serve). Recovering those needs source archaeology and is tracked separately —
out of scope for #172.
