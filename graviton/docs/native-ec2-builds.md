# Native EC2 package builds (the current path)

**Build HaikuPorts packages for arm64 by running `haikuporter` on a native Graviton
Haiku EC2 instance, driven over SSM by [haiku-mgmt-agent](https://github.com/felipedbene/haiku-mgmt-agent).**
This supersedes the metal + QEMU-guest fleet for package builds.

## Why this, not metal + QEMU guests

The QEMU guests run Haiku arm64 under KVM (near-native speed — speed was never the
problem). The problem is the guest disk images are **bootstrap-seeded**, not full Haiku
installs, so their build userland is half-wired: `python3.14` has its binary but not
`libpython3.14.so.1.0` in the build chroot, `freetype` is bootstrap-only (no `ft2build.h`),
etc. Those surface as per-package "blockers" that are really just an incomplete environment.

On a **native EC2 Haiku instance with a working `pkgman`**, the dependency closure resolves
correctly and those artifacts disappear. Validated 2026-08-30: `pkgman install python3.14`
pulled sqlite/readline/libiconv/gettext_libintl/file and `python3.14 --version` →
`Python 3.14.7`. So glib2/gobject-introspection (and the harfbuzz freetype/pygments issues)
should build here without the recipe cuts they seemed to need in the guests.

> HaikuPorts publishes **no arm64** repo (only riscv64/x86_64/x86_gcc2), so building these
> ourselves is required — there is nothing upstream to pull.

## Stand up a builder

```bash
# 1. Launch a Graviton instance from the canonical DeBeOS AMI. ALWAYS resolve the latest
#    canonical AMI from the SSM parameter — never hardcode an ami-id (any specific id, e.g.
#    ami-05a424c43ddfed717, is just whatever was canonical at the time).
AMI=$(aws ssm get-parameter --name /haiku-graviton/canonical-ami-id \
        --query Parameter.Value --output text)   # authoritative source of the canonical AMI
aws ec2 run-instances --image-id "$AMI" --instance-type c8g.2xlarge \
  --security-group-ids sg-008114891fd207df1 --key-name haiku-graviton \
  --iam-instance-profile Name=AWSSupportPatchwork-SSMRoleForInstances \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=haiku-native-builder},{Key=Project,Value=haiku-graviton}]'

# 2. The mgmt-agent registers the box as an SSM node within a few minutes. Confirm:
aws ssm describe-instance-information --filters Key=InstanceIds,Values=<iid> \
  --query 'InstanceInformationList[0].{Ping:PingStatus,Platform:PlatformName,Ver:PlatformVersion}'
#    -> Online / Haiku / hrev*.   The IAM profile MUST carry AmazonSSMManagedInstanceCore.
```

Drive it with plain `aws ssm send-command` / `graviton/scripts/ssm-run <iid>` — identical to
how the metal is driven. `PlatformType` reports `Linux` (closed SSM enum); `PlatformName`
is the real `Haiku`.

## Provision the toolchain (AMI is runtime-only)

The canonical AMI is the lean `@minimum-mmc` **runtime** image: no gcc/make/cmake/meson,
no `haikuporter`, no ports tree, no python until installed. The pre-registered **DeBeOS**
repo (`packages.debene.dev/arm64`) has them. Install with the bootstrap→DeBeOS vendor
change accepted (pkgman prints it as "solution 1"; feed `printf '1\ny\n' | pkgman install …`
for non-interactive runs):

```bash
pkgman install -y gcc make cmake meson ninja pkgconfig python3.14 ...   # build toolchain
# then set up haikuporter per the wiki: clone haikuporter + haikuports (--depth=50),
# copy haikuports-sample.conf -> /boot/home/config/settings/haikuports.conf,
# set PACKAGER + TREE_PATH, symlink haikuporter onto PATH.
# wiki: https://github.com/haikuports/haikuports/wiki  ("Setting Up HaikuPorts")
```

Big builds may need a BFS root grow (see the bfs/auto-grow work).

## Build and collect

```bash
# on the instance, via ssm-run:  haikuporter -y -G --all-dependencies <port>
# pull artifacts straight to S3 from the box:
haiku-mgmt-agent s3 cp /boot/home/haikuports/packages/<pkg>.hpkg s3://<bucket>/hpkg/arm64/
# or capture full command output to S3:
aws ssm send-command --document-name AWS-RunShellScript --output-s3-bucket-name <bucket> …
```

Publish to the repo with `graviton/scripts/haiku-repo-publish-remote` as before.

## Canonical driver

`graviton/scripts/haiku-nativebuild <port>...` encodes the sequence below as a hands-off
loop (build → auto-install whatever repo dep haikuporter reports missing → retry → harvest
to the pool). Launch a builder from `aws ssm get-parameter --name /haiku-graviton/builder-ami-id`
(tag `builder-canonical=true`) and drive it over SSM.

## Reproduced sequence — lean AMI → builder → glib2 (2026-08-30, RC=0)

This is the exact, working sequence. glib2 is the proof case: it "blocked" in the QEMU
guests on a half-wired python3.14/gobject-introspection; on native EC2 it builds clean
(produces `glib2-2.88.1-8` + `gobject_introspection-1.86.0-8` + devels).

**1. Launch** a Graviton instance from `$(aws ssm get-parameter --name
/haiku-graviton/canonical-ami-id ...)` (see above). For real builds, give it a big root via
`--block-device-mappings` (online grow is NOT seen — Haiku caches disk size at boot), then
`partition_grow` + `resizefs` at first boot.

**2. Provision the toolchain** (all from the DeBeOS repo; accept the bootstrap→DeBeOS
vendor change — pkgman "solution 1"; non-interactive: `printf '1\n…\ny\n' | pkgman install`).
Install **per-package** (one bad name aborts a batch):

```
gcc binutils make cmake meson ninja git diffutils patch gawk wget haiku_devel
python3.14 pkgconf          # pkg-config is provided by 'pkgconf', NOT 'pkgconfig'
ln -sf /boot/system/bin/python3.14 /boot/home/config/non-packaged/bin/python3   # env python3
```

**3. haikuporter** (not packaged — clone it):
```
cd /boot/home
git clone --depth=1 https://github.com/haikuports/haikuporter.git
git clone --depth=1 https://github.com/haikuports/haikuports.git
cp haikuporter/haikuports-sample.conf config/settings/haikuports.conf   # sample is in the haikuporter repo
#   set PACKAGER="DeBeOS <…>" and TREE_PATH="/boot/home/haikuports"
ln -sf /boot/home/haikuporter/haikuporter config/non-packaged/bin/haikuporter
```

**4. Build a port — install its deps from the repo, then build the leaf.** pkgman accepts
`devel:`/`cmd:` resolvables directly. Do **NOT** use `--all-dependencies` (it hits
"Port X depends on itself" on packages whose subpackage — e.g. gobject_introspection — is
also a build-require). Read the recipe's `BUILD_REQUIRES` + `BUILD_PREREQUIRES` and install:

```
# glib2 worked example (recipe pins python 3.10 for gobject-introspection):
pkgman install devel:libffi devel:libiconv devel:libintl devel:libpcre2_8 devel:libxml2 devel:libz
pkgman install bash_completion setuptools_python3.14 setuptools_python310 python3.10 \
               cmd:bison cmd:flex cmd:xgettext cmd:pkg_config
export PATH=/boot/home/config/non-packaged/bin:$PATH
cd /boot/home/haikuports && haikuporter -y glib2          # sources download over real internet
```

First `haikuporter` run scans all ~7100 recipes to build dependency-infos (one-time, slow).
Harvest with `haiku-mgmt-agent s3 cp .../packages/<pkg>.hpkg s3://…` and publish as above.

> The per-package prereq chase above is what a baked builder AMI eliminates — bake the
> toolchain + a common prereq set so builds start clean.

## Standing rule

Do **not** ship feature-capped builds (a backend/loader/subpackage disabled just to go
green). Fix the real blocker — which is exactly what a complete native environment makes
possible. Feature cuts are throwaway diagnostics only.
