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
#    auto-stop=no is NOT optional -- see "The janitor will stop your builder" below.
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=haiku-native-builder},{Key=Project,Value=haiku-graviton},{Key=auto-stop,Value=no}]'

# 2. The mgmt-agent registers the box as an SSM node within a few minutes. Confirm:
aws ssm describe-instance-information --filters Key=InstanceIds,Values=<iid> \
  --query 'InstanceInformationList[0].{Ping:PingStatus,Platform:PlatformName,Ver:PlatformVersion}'
#    -> Online / Haiku / hrev*.   The IAM profile MUST carry AmazonSSMManagedInstanceCore.
```

Drive it with plain `aws ssm send-command` / `graviton/scripts/ssm-run <iid>`.
`PlatformType` reports `Linux` (closed SSM enum); `PlatformName` is the real `Haiku`.

### The janitor will stop your builder

The account runs the SpringClean janitor. Once a day, at the end of the owner's
configured shift, it calls `StopInstances` on **every instance that does not carry
`auto-stop=no`**. It reads no liveness signal — not CPU, not network, not SSM — so a
box three hours into a silent `ninja` or `haikuporter` run is indistinguishable to it
from a box nobody has touched.

On 2026-10-16T20:06:24Z it stopped all three native arm64 builders in one sweep: one
75 minutes into an haikuporter packaging phase, one 95% of the way through a 3-hour
Ladybird build (issue 761). Both then looked like dead hardware — SSH answering
`Connection timed out during banner exchange` / `kex_exchange_identification: read:
Connection reset by peer` — which is the trap: the first thing to check when a builder
goes unreachable mid-build is `describe-instances`, not `sshd`.

Per SpringClean's own documentation the exemption tags are:

| tag | effect |
|---|---|
| `auto-stop=no` | never stopped at shift end |
| `auto-delete=no` / `never` | never deleted |
| `auto-delete=<YYYY-MM-DD>` | not deleted until that date |
| `auto-start=yes` | started before shift **and** exempt from deletion |

Every in-tree launcher now sets `auto-stop=no` (`haiku-launch`, `haiku-bake-builder`,
`haiku-perf-gate`, `haiku-repo-publish-ephemeral`, the wave's `launch_builder.py`);
`graviton/scripts/haiku-launch-selftest` is the ratchet that keeps a newly added
launcher from dropping it. `haiku-launch --reapable` opts back out for a box
short-lived enough that a nightly stop is harmless.

`auto-delete` is deliberately **not** set anywhere. These are ephemeral boxes and
having the janitor reclaim a forgotten one is wanted — it is also what bounds the cost
of `auto-stop=no`, since an instance left running is still deleted at the account's
delete age, with a warning email first. Terminate your own builders anyway.

## Provision the toolchain (AMI is runtime-only)

The canonical AMI is the lean `@minimum-mmc` **runtime** image: no gcc/make/cmake/meson,
no `haikuporter`, no ports tree, no python until installed. The pre-registered **DeBeOS**
repo (`packages.debene.dev/arm64`) has them. Install with the bootstrap→DeBeOS vendor
change accepted (pkgman prints it as "solution 1"; feed `printf '1\ny\n' | pkgman install …`
for non-interactive runs):

```bash
pkgman install -y gcc make cmake meson ninja pkgconf python3.14 ...   # build toolchain (pkg-config is provided by 'pkgconf', NOT 'pkgconfig')
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

Publish to the repo with `graviton/scripts/haiku-repo-publish` on a Haiku host (the
dev AMI, or this native builder itself).

## Canonical driver

`graviton/scripts/haiku-nativebuild <port>...` encodes the sequence below as a hands-off
loop (build → auto-install whatever repo dep haikuporter reports missing → retry → harvest
to the pool). Launch a builder from `aws ssm get-parameter --name /haiku-graviton/builder-ami-id`
(tag `builder-canonical=true`) and drive it over SSM.

**One driver per builder.** The driver takes a mutex (`$HP/.nativebuild.lock`) for the
whole run and refuses to start while another holds it, because haikuporter is not safe to
run twice against one ports tree: the second run's chroot-setup deletes the links in
`boot/system/packages`, then aborts on `unmount: unmounting failed: Device/File/Resource
busy` before it can remount packagefs — so the *first* run's build packages are gone and
its `make install` fails hours later with `mkdir: cannot create directory
'/packages/<port>-<version>-<revision>': Read-only file system`, which looks nothing like
the cause (issue 702). The two runs also share `/boot/home/nb-<port>.log`, which the
driver opens with `>`, so the surviving run's log is truncated too. A lock whose owner
pid is gone is reclaimed automatically; `NB_LOCK_WAIT_SECS` sets how long a second driver
waits (default 3600) before refusing.

## Reproduced sequence — lean AMI → builder → glib2 (2026-08-30, RC=0)

This is the exact, working sequence. glib2 is the proof case: it "blocked" in the QEMU
guests on a half-wired python3.14/gobject-introspection; on native EC2 it builds clean
(produces `glib2-2.88.1-8` + `gobject_introspection-1.86.0-8` + devels).

**1. Launch** a Graviton instance from `$(aws ssm get-parameter --name
/haiku-graviton/canonical-ami-id ...)` (see above) with the **DEFAULT root size**.

> **Do not pass a `--block-device-mappings` / `--disk` size override to a builder you
> drive over SSM** (#254, `../ops/SOPs.md` §13.6 — the authority). Haiku caches disk
> size at boot, so the override needs a first-boot `partition_grow` + `resizefs`, and
> that userland job races the network-gated SSM and sshd launch jobs: the box boots
> healthy and is then unreachable by any channel. This run predates that finding.
>
> The fix that would lift this restriction has landed but is **not boot-proven**:
> the partition grow moved into the EFI loader
> (`EfiDevice::GrowBootGptPartition()`), which runs before the kernel mounts the
> boot volume and so removes the launch-daemon ordering dependency entirely — see
> `develop/bfs-auto-grow-design.md`. It is compile-proven only; the one-boot
> hardware confirmation (verification plan T4) is still owed. **Until T4 lands,
> follow §13.6 and launch at the default size.**

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
