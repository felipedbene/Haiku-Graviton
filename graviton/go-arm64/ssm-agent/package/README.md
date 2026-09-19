# amazon_ssm_agent hpkg — the real SSM agent, packaged for DeBeOS (M7)

This directory packages the real upstream **`amazon-ssm-agent`** (v3.3.3270.0,
Apache-2.0), cross-built for DeBeOS/Haiku arm64 (M0–M6, see `../README.md` and
`../../README.md`), as an installable Haiku `.hpkg` with a `launch_daemon` job
that starts it **EC2-native** on every boot. It is the productionization step of
issue #302: moving the fleet SSM agent from the interim custom `debeos-ssm-agent`
to the real agent.

## The native-registration gate — CLEARED (hardware-proven)

Before the real agent can be the baked fleet default it had to register
**EC2-native via the instance role over IMDS** — as an `i-…` managed node, with
**no hybrid activation code** — not the `mi-…` hybrid path M5/M6 used. Proven on
a real Graviton `c7g.large` (`i-0d26a99b645b6ab8b`, canonical
`ami-04493ac7c3fe0d304`, Haiku hrev59996):

1. Launched from canonical with the default `ssm-instance-profile`
   (AmazonSSMManagedInstanceCore). The baked `debeos-ssm-agent` came up first and
   owned the instance's `i-` node (AgentVersion 0.4.0).
2. `launch_roster disable` + `stop x-vnd.debeos-ssm-agent` freed the identity, and
   the real agent was started with **no OnPrem vault present**, so its identity
   selector logged *"Checking if agent identity type OnPrem can be assumed" →
   "Agent will take identity from EC2"* — i.e. it read instance-id/region from
   IMDS and used the instance role.
3. `aws ssm describe-instance-information` for the `i-` node then reported:

   | field | value |
   |---|---|
   | InstanceId | **i-0d26a99b645b6ab8b** (the `i-` node, not an `mi-`) |
   | PingStatus | **Online** |
   | AgentVersion | **3.3.0.0** (the real agent; debeos is 0.4.0) |
   | **ResourceType** | **EC2Instance** (an EC2-native node, not `ManagedInstance`) |
   | PlatformName / Version | Haiku / R1~beta6+development |
   | PlatformType | Linux (closed-enum, B2) |

4. `AWS-RunShellScript` against that `i-` node returned **Status Success, RC 0**
   with real stdout (`whoami=baron`, `arith=42`, `uname … arm64`), reproduced
   **n=2** (`run2-distinct-token-91`, `SSM Agent version: 3.3.0.0`).

That is the gate: **EC2-native (IMDS, instance role, `i-` node) registration and
Run Command work** with the real agent on DeBeOS/arm64.

## The package

`amazon_ssm_agent-3.3.3270.0-1-arm64.hpkg` — **zlib-compressed** (~35 MB;
`package create -z zlib`, sha256 `3871b026cdf2b2919e9970580582168ec07fdf0cde96f7e61693596725786187`
for the build below). Layout (package root maps to `/boot/system`):

```
bin/amazon-ssm-agent          bin/ssm-agent-worker     bin/ssm-document-worker
bin/ssm-session-worker        bin/ssm-session-logger   bin/ssm-cli
bin/updater                   bin/ssm-setup-cli
data/launch/amazon_ssm_agent               # launch_daemon service (static PATH)
data/licenses/Apache-2.0                    # bundled (not a Haiku system license)
.PackageInfo
```

**zlib is mandatory, not cosmetic.** The lean image's packagefs has no zstd
reader; a zstd hpkg (Haiku's `package create` default) lands in
`system/packages` but *fails to activate at first boot* ("Failed to decompress
chunk data: Operation not supported"). zlib activates in both the lean image and
the full system — the same constraint that governs `debeos_ssm_agent` and the
OpenSSH package.

The agent resolves its worker binaries relative to its own path (`os.Args[0]`),
so with the core at `/boot/system/bin/amazon-ssm-agent` and the workers beside
it, `DefaultProgramFolder` becomes `/boot/system/bin` (verified: packagefs files
are root-owned, which the agent's relative-path check requires). No
`amazon-ssm-agent.json` is needed for EC2-native — the built-in defaults apply.

### Launch job (EC2-native + PATH)

`data/launch/amazon_ssm_agent` is a `legacy service` that launches
`/boot/system/bin/amazon-ssm-agent` with a **static** `env { PATH … }` block that
puts `/boot/system/bin` on PATH. Without the shell dir on PATH the (healthy)
fork+exec of `ssm-document-worker` reaches `execve` and returns "sh not found in
$PATH" — the M6 deploy note, now baked into the job.

PATH is inlined statically rather than sourced from a script. An earlier revision
used `env { from_script data/amazon_ssm_agent/agent-env.sh }`; launch_daemon
evaluates a `from_script` source by spawning `sh -c '. <script>; export -p'` and
reading its output with an unbounded read early in boot (#376), which hangs
before the network/desktop come up — the job stayed `enabled=true` but
`launched=false` and the agent never started at boot (it only ran when started by
hand). The static block sets exactly what the removed `agent-env.sh` exported
(`PATH=/boot/system/bin:/bin:/boot/system/non-packaged/bin`), so run-time
behaviour is unchanged and the job now launches on every boot.

## Building the hpkg

`package create` runs on Haiku, not on the Linux cross-host, so the hpkg is
assembled **on a Graviton DeBeOS box**. `build-hpkg.sh` does it:

```sh
# On a DeBeOS/Haiku arm64 instance, with the 8 cross-built binaries in $BINDIR:
BINDIR=/boot/home/ssm-real/bin \
PKGDIR=<this dir> \
OUTDIR=/boot/home \
    sh build-hpkg.sh
# -> /boot/home/amazon_ssm_agent-3.3.3270.0-1-arm64.hpkg  (zlib)
```

The eight binaries are produced by the cross-build in `../README.md`
("How to reproduce"): pristine `amazon-ssm-agent` v3.3.3270.0 + the
`../amazon-ssm-agent-haiku-arm64.patch` + the korli/go fork with the
M0/M2/M4/M6 patchset, `GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 go build` of the
eight `makefile` targets.

## Baking it into the image (wiring plan — NOT done here)

Baking is a separate integration step. It mirrors exactly how `debeos_ssm_agent`
is baked today (`graviton/ssh/UserBuildConfig`, the `AddPackageFilesToHaikuImage`
pattern) — deliberately **`AddPackageFilesToHaikuImage`, not
`AddHaikuImageSystemPackages`**: the lean arm64 image has no DeBeOS repository as
a `HAIKU_REPOSITORIES` member, and `package_repo create` rejects a DeBeOS-vendored
package ("unexpected vendor 'DeBeOS'"). Dropping the hpkg straight into
`system/packages` skips the solver and the vendor check, and packagefs still
activates it and runs its bundled launch job.

Wire it by adding, next to the existing `debeos_ssm_agent` glob block in
`graviton/ssh/UserBuildConfig`:

```jam
if $(HG_POOL_DIR) {
    local ssmGlob = [ Glob $(HG_POOL_DIR) : amazon_ssm_agent-*-arm64.hpkg ] ;
    if $(ssmGlob) {
        local ssmName = $(ssmGlob[1]:G=:D=) ;
        local ssmPackage = <hg-pool>$(ssmName) ;
        SEARCH on $(ssmPackage) = $(HG_POOL_DIR) ;
        AddPackageFilesToHaikuImage system packages : $(ssmPackage) ;
    }
}
```

and add `--include "amazon_ssm_agent-*"` to the lean pool `aws s3 sync` in
`graviton/pipeline/buildspecs/cross-build.yml` (beside the existing
`--include "debeos_ssm_agent-*"`). The version-agnostic glob means a newer hpkg
needs no edit. Runtime prerequisite unchanged: the launch template must attach an
instance profile carrying AmazonSSMManagedInstanceCore (the AMI cannot supply its
own IAM role).

### CRITICAL: exactly one SSM launch job may be active (measured)

**Do not co-activate both agents.** On the gate box, after installing this hpkg
and rebooting, `launch_roster disable` did **not** persist across the reboot, so
*both* packaged launch jobs ran — `debeos_ssm_agent` (0.4.0) and
`amazon_ssm_agent` (3.3.0.0). Both then heartbeat the **same** `i-` node
(last-writer-wins on the reported AgentVersion) and contend for its MDS command
stream, which **wedges Run Command** (commands sit `Pending`). A fleet image must
ship only one active SSM launch job.

> **Correction (#376).** An earlier draft here read the post-reboot `3.3.0.0`
> Online ping as proof the package's *launch job* started the agent at boot. A
> later staging-RC boot-test disproved that: with the `env { from_script … }`
> block, `launch_roster info x-vnd.debeos-amazon-ssm-agent` showed
> `enabled=true, launched=false, running=false` on first boot and after reboot
> (even `launch_roster start` refused it), because launch_daemon's `from_script`
> evaluation hangs on an unbounded read early in boot. The agent only ran when
> started by hand, so the earlier "Online from the launch job" reading was
> confounded. The launch job now sets PATH **statically** (this revision), which
> is what makes it actually launch at boot.

So the bake must make the switch a **replacement**, not an addition:

- **Primary path (recommended):** bake `amazon_ssm_agent` as the *only* SSM launch
  job. Stop pooling/baking `debeos_ssm_agent` (drop its `--include` and its
  `UserBuildConfig` glob block) so its `data/launch/debeos_ssm_agent` job is not
  present in the image at all. One agent, one `i-` node, no contention.
- **Fallback / rollback:** keep the last-known-good `debeos_ssm_agent` hpkg in the
  DeBeOS repo (not baked-active). If a fleet `amazon_ssm_agent` bake regresses,
  roll back by re-baking with the `debeos_ssm_agent` block and dropping the
  `amazon_ssm_agent` one — or, on a live box, `pkgman install` the
  `debeos_ssm_agent` hpkg and remove `amazon_ssm_agent`, then reboot. Because both
  are plain `AddPackageFilesToHaikuImage` drops, the swap is a one-block edit in
  `UserBuildConfig` + the matching pool `--include`.
- Do **not** rely on `launch_roster disable` to keep the losing agent down: it is
  in-memory for the running `launch_daemon` and does not survive a reboot. The
  durable control is which `data/launch/*` job is present in the image.

Validate any bake candidate on a fresh `c7g.large` before promoting: exactly one
SSM package present, `describe-instance-information` shows the `i-` node Online at
AgentVersion 3.3.x with ResourceType EC2Instance, and an `AWS-RunShellScript`
returns real stdout — then promote via `haiku-canonical promote` as usual. This
task does **not** touch canonical/pipeline/green.
