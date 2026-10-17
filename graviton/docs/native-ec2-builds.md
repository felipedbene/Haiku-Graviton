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

### Run it DETACHED, with its output on the builder (issue 779)

As of `NATIVEBUILD_VERSION=2026-10-17.1` the driver keeps its own run log on the
builder at `$HP/nativebuild-run.log` (`NB_RUN_LOG` to override), so the verdicts
survive whoever is watching going away. Read it when a run's stdout is gone:

```sh
haiku-ssh -p <port> baron@127.0.0.1 'tail -40 /boot/home/haikuports/nativebuild-run.log'
```

Still run it detached. The run log saves the *record* on every image; it saves
*liveness* only where `tee` supports `--output-error=warn-nopipe` (the base image's
`/boot/system/bin/tee` does, but the driver probes rather than assumes). Detaching
makes the question moot:

```sh
# stage the driver on the builder, then start it detached -- two separate calls
haiku-ssh -p <port> baron@127.0.0.1 'cat > /boot/home/nativebuild.sh' \
    < graviton/scripts/haiku-nativebuild
haiku-ssh -p <port> baron@127.0.0.1 \
    'nohup sh /boot/home/nativebuild.sh <port>... > /boot/home/nb-driver.log 2>&1 &'
# then poll, from any later connection
haiku-ssh -p <port> baron@127.0.0.1 'tail -20 /boot/home/nb-driver.log'
```

The obvious one-liner is the one that fails:

```sh
# DON'T -- the run dies with the connection and leaves no verdict behind
haiku-ssh -p <port> baron@127.0.0.1 'sh -s <port>...' < graviton/scripts/haiku-nativebuild
```

Measured on `i-076018fb6c01ef378`, building `app-text/texlive`. An expired token killed
the `ec2-instance-connect open-tunnel`:

```
aws: [ERROR]: An error occurred (ExpiredTokenException) when calling the GetSecretValue operation: ...
kex_exchange_identification: read: Connection reset by peer
```

On a fresh tunnel both processes were still alive and reparented to init, so the drop
had not killed them outright — but eight minutes later both were gone, with
`/boot/home/nb-texlive.log` frozen on haikuporter's attempt-1 verdict
(`build-requires "cmd:luametatex" ... could not be resolved`), `luametatex` **not**
installed, the lock released, and no `BUILD_OK`/`FAILED`/`attempt N installing:` line
anywhere on the builder. `build_one` writes that line to stdout immediately before
calling `install_dep`, which is exactly the gap the run died in.

Two things make this worse than a lost terminal. The drop is **scheduled** — credentials
expire, so every port whose build outlasts the credential lifetime is affected, which is
the same set that most needs a driver (texlive, llvm, rust, ladybird). And the leftover
state is **misleading**: a frozen per-port log ending in a dependency-resolution error
reads as "this port cannot resolve its dependencies", when the driver had diagnosed the
dependency correctly and was one statement from installing it. Same shape as #769 and
#776 — the visible symptom names something other than the cause.

The detached form above is what got the build going again, and it survives the drop.

## Reproduced sequence — lean AMI → builder → glib2 (2026-08-30, RC=0)

This is the exact, working sequence. glib2 is the proof case: it "blocked" in the QEMU
guests on a half-wired python3.14/gobject-introspection; on native EC2 it builds clean
(produces `glib2-2.88.1-8` + `gobject_introspection-1.86.0-8` + devels).

**1. Launch** a Graviton instance from `$(aws ssm get-parameter --name
/haiku-graviton/canonical-ami-id ...)` (see above). This run used the default root
size; a bigger one is now fine — see below.

> **`--disk` is allowed again, as of 2026-10-17.** The old prohibition (#254,
> `../ops/SOPs.md` §13.6) was real: Haiku caches disk size at boot, the override
> needed a first-boot userland `partition_grow`, and that job raced the
> network-gated SSM and sshd launch jobs, so the box booted healthy and was then
> unreachable by any channel.
>
> The partition grow has since moved into the EFI loader
> (`EfiDevice::GrowBootGptPartition()`), which runs *before* the kernel mounts the
> boot volume, removing the launch-daemon ordering dependency entirely. That is now
> boot-proven: verification plan **T4** passed on two instances launched with
> `haiku-launch --disk 100` and `--disk 60`, both of which reported the full size
> from `df -k /boot` on **boot one**, both reachable on that same boot, `checkfs`
> showing no allocation or structural errors, and the second boot a no-op. The
> measurement is in `develop/bfs-auto-grow-design.md`.
>
> So: `haiku-launch --disk <GiB>` for a build that needs room, and do **not** reboot
> to converge it — there is nothing left to converge.

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

## Recovering a builder whose boot volume filled up

**Issue 770, open.** A builder whose 20 GiB BFS root reaches 0 free stops answering
*every* channel: SSM goes `ConnectionLost` in the same minute, `sshd` accepts the TCP
connection and never sends a banner, and EC2's instance reachability check flips to
`impaired` while the system check stays `ok`. The serial console shows no panic — the
kernel is alive and still allocating. There is **no in-place recovery**: the one thing
you would do (log in and delete something) is the thing that stops working.

So the procedure is about the *volume*, not the instance.

**1. Do not terminate it.** The hpkgs the run already built are on that volume and
nowhere else — harvest runs at the END of a run, and on the current builder image it is
a fatal no-op anyway (issue 728). Terminating with the default `DeleteOnTermination=true`
destroys them.

**2. Stop, then start.** `stop-instances` is a hard power-off from the guest's point of
view, but the EBS root survives it, and a fresh boot gets you a running system with a
(still full) volume. This is a **modify** on a running instance, so it needs explicit
operator sign-off — it is not something automation should do on its own.

> After any stop/start, come back over **SSH**, not SSM. The SSM registration does not
> survive a reboot on this image, and `sshd` is ed25519-only — see
> `haiku-launch`/`haiku-ssh` and the access notes. Have the tunnel open before you need
> it: `aws ec2-instance-connect open-tunnel --instance-id <iid> --remote-port 22
> --local-port 22NN`, then `graviton/scripts/haiku-ssh -p 22NN baron@127.0.0.1`.

**3. Delete with a budget, not with `rm -rf`.** The tree to remove is usually
`/boot/home/haikuports/work-*` or a single port's work directory, and on BFS a 7 GiB
delete takes minutes. Do **not** wrap it in a short `timeout` — a `timeout 110` has
already killed one of these mid-delete, which leaves the tree half-removed and the
volume still full. Run it under `nohup`, poll `df -k /boot` separately.

**4. If it does not come back from the stop/start**, the volume is still readable: detach
it and attach it to a NEW Haiku builder as a secondary device (`/dev/sdf` →
`/dev/disk/nvme/1/...`), mount it read-only, and copy the hpkgs out. BFS is not readable
from a Linux host, so a Linux rescue instance is no help here.

**5. Prevention is the driver's job now.** `haiku-nativebuild` refuses to start a port
with less than `NB_DISK_FLOOR_MIB` (default 3072 MiB) free, aborts the run on an ENOSPC
verdict rather than walking the remaining ports into the same wall, and the wave records
that as `capacity`, not as a port failure (issue 769, playbook Class 41). That makes this
state much harder to reach — it does not make it impossible, because a single `haikuporter`
invocation can still consume the floor in one step.

### What is established about the cause, and what is not

**Established.** BFS keeps **no free-space reserve for root or for an interactive user**.
`BlockAllocator::InitializeAndClearBitmap` reserves only metadata — the boot block, the log area and
the block bitmap (`src/add-ons/kernel/file_systems/bfs/BlockAllocator.cpp:621-635`) — and
the allocation paths just return `B_DEVICE_FULL` when the bitmap is exhausted
(`:1020`, `:1148`, `:1163`). There is no uid carve-out anywhere in the allocator, so the
usual ext4/UFS defence (a few percent only the superuser can dip into) does not exist
here. That is a design gap, not a bug being hit.

**Established: BFS allocates eagerly, so `ENOSPC` is always delivered synchronously.**
`Inode::WriteAt` grows the data stream *before* it touches the page cache — `SetFileSize`
→ `_GrowStream` runs at `Inode.cpp:1621-1628` and its failure returns `B_DEVICE_FULL`
straight to the caller with `*_length = 0`; only once that has succeeded does it call
`file_cache_write` (`:1653`). There is no delayed allocation anywhere in the write path.
This is the load-bearing fact for everything below: a dirty file-backed page in this
filesystem **always already has its blocks**, so writeback cannot discover that the
volume is full.

**Ruled out (1): the swap file.** The obvious theory — paging dies because the swap file
cannot grow — does not hold. BFS has no sparse files, and the kernel sizes swap once at
boot and materialises it with `B_STAT_SIZE`, so its blocks are **already allocated**
before the volume fills (`src/system/kernel/vm/VMAnonymousCache.cpp:1763-1791`; the size
is min(RAM, 25% of free) for automatic swap, `:1689-1697` and `:1771-1773`). Writing a
page into an already-allocated range needs no new allocation, so a full volume does not
stop paging. Worth recording as a dead end so the next person does not spend the same
hour on it.

**Ruled out (2): dirty pages that cannot be flushed starving the free-page pool.** This
one is worth stating because it fits the symptoms so well and is still wrong. The shape
is real: when a page write fails, `PageWriteWrapper::Done` sets `fPage->modified = true`
and puts a non-temporary (file-backed) page **straight back on the modified queue**
(`src/system/kernel/vm/vm_page_writer.cpp:261-311`), and the writer then retries forever,
pausing 500 ms whenever a whole queue pass wrote nothing (`:632-637`). Pages stuck there
are never freed, and `vm_page_reserve_pages` has no failure path at all — it is `void`,
it calls `reserve_pages(..., dontWait = false)` (`vm_page.cpp:2675-2687`), and that ends
in a bare `thread_block()` with no timeout (`:2405-2417`) which only
`wake_up_page_reservation_waiters` can release, and only once `reserve_some_pages` has
actually succeeded (`:1426-1451`). So *if* the pool were exhausted this way, every page
reservation — including the one `fork()` needs for an incoming sshd session — would wait
forever, which is exactly the observed "accepts TCP, never sends a banner".

It does not happen, because of the eager allocation above: the failure arm of
`PageWriteWrapper::Done` is not reachable for `B_DEVICE_FULL` on BFS. The blocks behind
every modified page were allocated at `write()` time, so writeback has nothing left to
allocate and cannot fail for lack of space. The unbounded wait is still there and is
still worth knowing about — it is just not reached by a full volume.

**Ruled out (3): the BFS journal running out of log space.** The log area is a fixed
region allocated at `mkbfs` time, so it does not compete with file data — and when a
transaction will not fit after a sync, `Journal::_WriteTransactionToLog` **panics** rather
than blocking (`src/add-ons/kernel/file_systems/bfs/Journal.cpp:1198-1204`). The serial
console showed no panic, so this path was not taken. That is the one place where the
*absence* of console output is itself evidence.

**Ruled out (4): `syslog()` blocking on a full daemon port.** `send_syslog_message` writes
to the syslog daemon's port with **no timeout**, retrying forever on `B_INTERRUPTED`
(`src/system/libroot/posix/syslog.cpp:172-177`, under the comment "make sure the message
gets send"), and sshd logs on the pre-auth path. If the daemon ever stopped draining, every
`syslog()` caller in the system would block there permanently. It keeps draining: a log
write that fails just breaks out of the message handler and the read loop continues
(`src/servers/syslog_daemon/syslog_output.cpp:201-205`, `SyslogDaemon.cpp:141`). Another
unbounded primitive that is real but is not this bug.

**Not established: what `sshd` blocks on.** This is issue 770's open half, and the four
eliminations above narrow it rather than answer it. What they rule out collectively is the
whole class of *deferred* failures — there is no path on this filesystem where a full
volume is discovered late, by a kernel thread, after the `write()` that caused it already
returned success. So whatever sshd waits on is reached while `ENOSPC` is being reported
correctly, which means a free-space reserve in the allocator would very likely fix the
symptom even without naming the wait.

Closing it properly still needs a thread backtrace from a box in that state. That means
reproducing the condition deliberately: fill a disposable builder's volume, then enter KDL
over the EC2 serial console (account access is already enabled) and read `threads` plus a
`bt` for the sshd team. `graviton/scripts/haiku-qemu-virt` is the cheaper arm of the same
test and boots a DeBeOS arm64 image with the serial console on stdio, but it gives no
userland shell on that console, so filling the volume from inside the guest needs an image
with the fill scripted at boot.

## Standing rule

Do **not** ship feature-capped builds (a backend/loader/subpackage disabled just to go
green). Fix the real blocker — which is exactly what a complete native environment makes
possible. Feature cuts are throwaway diagnostics only.
