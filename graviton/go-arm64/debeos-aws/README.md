# debeos-aws — a native AWS client for DeBeOS on Haiku arm64

`debeos-aws` is a minimal, CGO-free AWS client that runs natively on Haiku
arm64 (AWS Graviton). It exists to retire the ephemeral Linux publish peer
(DeBeOS #120): the package-repo publish path needs **bulk S3** plus a
**CloudFront invalidation**, and until now a headless Haiku box could not do
either — the baked `debeos-ssm-agent` only does single-file `s3 cp`, and there
is no native `aws` CLI.

This tool covers exactly the verbs the publish flow calls (`graviton/scripts/haiku-repo-add`):

| publish-path call                                  | debeos-aws                          |
| -------------------------------------------------- | ----------------------------------- |
| `aws s3 sync <repo>/packages/ <local>/` (~1900 objs) | `debeos-aws s3 sync <s3> <dir>`     |
| `aws s3 sync <local>/packages/ <repo>/ --delete`   | `debeos-aws s3 sync <dir> <s3> --delete` |
| `aws s3 cp <local>/repo <repo>/repo`               | `debeos-aws s3 cp <local> <s3>`     |
| `aws s3 rm <incoming>/<pkg>`                        | `debeos-aws s3 rm <s3>`             |
| `aws cloudfront create-invalidation ...`           | `debeos-aws cloudfront create-invalidation ...` |

`package_repo` (the other half of the publish path) is already a Haiku host
tool, so once bulk S3 is native the ephemeral Linux publish peer is no longer
required.

## Verbs

```
debeos-aws s3 ls   [s3://bucket[/prefix]]
debeos-aws s3 cp   <src> <dst>            # local<->s3 or s3<->s3, one object
debeos-aws s3 sync <src> <dst> [--delete] [--exclude GLOB] [--include GLOB]
debeos-aws s3 rm   <s3://bucket/key>
debeos-aws cloudfront create-invalidation --distribution-id ID --paths /a /b ...
debeos-aws version
```

Credentials come from the default AWS chain — on a headless Graviton box that
is the **EC2 instance-role provider over IMDS**. Region: `--region`,
`AWS_REGION`/`AWS_DEFAULT_REGION`, or IMDS placement (defaults to `us-west-2`).

`sync` uses the awscli default comparator (copy if the destination is missing,
the size differs, or the source is newer) and supports ordered
`--include`/`--exclude` glob rules (last match wins). Only `local<->s3` sync is
implemented, which is all the publish path uses.

## Why Go, not Python (aws-cli v2)

aws-cli v2 is Python with C extensions (`cryptography` → OpenSSL/Rust). A native
Python + C-extension stack on Haiku arm64 is a large, uncertain port. The Go
path is far more tractable and reuses proven ground:

- **The Go toolchain already works on Haiku arm64** (the korli-go fork extended
  to arm64; see `graviton/docs/go-arm64-bringup-scope.md`). HTTPS + the
  concurrent netpoller are hardware-proven on Graviton (M2).
- **aws-sdk-go-v2 is pure-Go and CGO_ENABLED=0-friendly**, with pure-Go
  `crypto/tls` — the exact properties that made the SSM-agent bring-up work.
- **An S3 client needs no `fork`+`exec`**, so it dodges the one open toolchain
  blocker (B1) that stalls the full amazon-ssm-agent port.

## Build

Cross-built from an amd64 Linux host with the korli-go toolchain (the toolchain
binary is amd64-hosted; the module proxy must be reachable, so this is done on a
throwaway builder EC2, not the corp workstation whose proxy is DNS-sinkholed):

```
GOROOT_HAIKU=/opt/korli-go ./build.sh
# -> debeos-aws: ELF 64-bit AArch64 Haiku, interpreter /system/runtime_loader, CGO-free
```

`go.mod`/`go.sum` pin aws-sdk-go-v2 (S3 + CloudFront + s3/manager + config).

## Proven (2026-09-18, Graviton c7g.large, canonical ami-04493ac7c3fe0d304)

Running natively on Haiku (hrev59996), authenticating **only** via the EC2
instance role over IMDS — no static keys:

```
### s3 ls (instance role via IMDS)
2026-09-18 21:45:41    9896204 awscli120/debeos-aws
2026-09-18 21:42:57       5879 awscli120/debeos-aws-src.tgz
...
### s3 cp DOWN
download: s3://.../go.sum -> /boot/home/gosum.txt (5296 bytes)
### s3 cp UP
upload: /boot/home/proof.txt -> s3://.../proof/proof.txt
### sync UP round-trip
sync up: 2 copied, 0 deleted
### sync DOWN into fresh dir
sync down: 2 copied, 0 deleted    (content verified)
```

The seed step (getting this binary onto the box) reused the existing native
single-file GET: `debeos-ssm-agent s3 cp s3://.../debeos-aws /boot/home/debeos-aws`.

## Status / not-yet-covered

- **`s3 rm` / `sync --delete`**: implemented and correct — surfaced a clean
  `AccessDenied` when the test box's role lacked `s3:DeleteObject` on the scratch
  bucket. The real publish target (the repo/hpkg bucket) grants delete, and the
  build-fleet role already carries `GetObject`/`PutObject`/`ListBucket`/
  `DeleteObject` + `cloudfront:CreateInvalidation` there. The `DeleteObjects`
  batch path checks the per-key `Errors` array (a false "deleted" was fixed).
- **`cloudfront create-invalidation`**: implemented and linked, but **not fired
  against the live CDN distribution** in this pass (that would touch the green
  pool). It should be validated as the last step of a real publish.
- **`s3<->s3` sync** and multi-source globbing beyond the publish path are not
  implemented (unneeded).

## Peer retired (#68)

`haiku-repo-add` now routes its `aws s3`/`aws cloudfront` calls through
`graviton/scripts/haiku-aws`, which prefers `debeos-aws` on a Haiku box (and
falls back to the stock `aws` on a Linux builder). The whole publish therefore
runs natively on a Haiku Graviton box -- `package`/`package_repo` (native host
tools) build the index, `debeos-aws` (via the EC2 instance role over IMDS) does
the bulk S3 + CloudFront -- driven by `graviton/scripts/haiku-repo-publish-native`.
No Ubuntu peer.

Proven natively (Graviton c7g.large, Haiku hrev59996, instance role only): a
from-empty publish of two hpkgs to a scratch prefix -- index rebuilt with
`package_repo create`, packages + `repo`/`repo.info`/`repo.sha256` uploaded via
`debeos-aws s3 sync`/`cp`, incoming pruned with `s3 rm`, and a
`cloudfront create-invalidation` -- all on the Haiku box, then verified against
S3. The legacy `haiku-repo-publish-ephemeral` (Ubuntu peer) is kept only as the
Linux fallback.
