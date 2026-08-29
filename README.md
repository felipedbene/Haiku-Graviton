<img src="data/artwork/debeos/debeos-logo-256.png" alt="DeBeOS" width="180" align="right">

# DeBeOS

**DeBeOS is an independent operating system project, descended from
[Haiku](https://www.haiku-os.org/) and BeOS.** It is **ARM-first**: AWS Graviton on
EC2 is the current flagship target, with Raspberry Pi 5, Raspberry Pi 3 and RISC-V on
the [roadmap](ROADMAP.md).

Haiku and BeOS are DeBeOS's **historical lineage**, and the debt is real — this tree
began as a Haiku checkout and the great majority of the code is Haiku's, under its
own licence. But DeBeOS is no longer a fork tracking an upstream. It is its own
project with its own priorities, and it does not sync from Haiku. Work here is
DeBeOS's own implementation, not a patch queued for somebody else.

> **Built and maintained with heavy [Claude Code](https://claude.com/claude-code)
> assistance**, disclosed deliberately rather than quietly. Most of the changes in
> this tree were written, measured and verified in collaboration with an AI agent.
> Every performance claim below was taken on real hardware, and where a conclusion
> later turned out to be wrong it is corrected in the docs rather than removed —
> the `graviton/docs/` directory keeps the disproven hypotheses on the record next
> to the ones that survived.

---

## Why ARM-first

Haiku's arm64 support existed but had not been driven hard on real server hardware.
Doing that surfaced a run of defects that were invisible under emulation and, in
several cases, not architecture-specific at all — a page-writer that never flushed
file data, a 64-bit overflow that stepped the clock backwards every 4 h 53 m, a
socket buffer that capped a single TCP stream at 1.6 Gbit/s. Those are the kinds of
bug you only find by booting on the metal and measuring.

ARM is also where the interesting hardware is: Graviton in the cloud, Raspberry Pi on
the desk, RISC-V next.

## What works today, measured

Everything here was measured on real AWS Graviton hardware, not emulated.

| | |
|---|---|
| Boots on EC2 Graviton | `c7g`, `t4g`, `c8g`, and `c7g.metal` — GICv3 redistributor discovery and per-bridge ECAM are fixed, so the bare-metal instance reaches userland |
| AWS ENA networking | jumbo frames at MTU 9001, multi-descriptor RX/TX |
| Throughput | **4.95 Gbit/s receive, ~4.4 Gbit/s transmit**, single queue |
| Jumbo frames vs MTU 1500 | receive **+403%**, transmit **+184%**, at well under half the CPU per byte |
| Clean shutdown | ACPI power button received over the PL061 GPIO; an EC2 stop completes in ~33 s instead of being force-killed |
| Survives stop/start | verified, with the host key intact |
| Root filesystem auto-grow | launch onto a larger EBS volume and the BFS root grows to fill the disk on first boot — a first-boot GPT partition grow, then an in-place BFS grow at mount; crash-safe (checkfs-clean across power-loss injection at every phase boundary), no rebake needed |
| Serial console | full boot log via `get-console-output --latest` |
| Web browser | WebPositive on HaikuWebKit renders real pages — JavaScript, CSS grid/flexbox/gradients/transforms |
| Data translators | JPEG, PNG, TIFF, GIF, WebP and JPEG2000 decode |
| Native Rust | `rustc` and `cargo` run on-device and build real crates fetched live from crates.io — ripgrep builds end-to-end natively |
| Native package builds | hundreds of ports built natively — a full dev toolchain (git, cmake, ninja, python, ruby, perl, vim) alongside the Rust toolchain |
| On-demand packages | a lean base image installs WebPositive, the Rust and dev toolchains, and hundreds more packages on demand from the DeBeOS package repository (S3 + CloudFront), so the image ships small and grows to fit the task |
| Fleet management | an independent, unofficial SSM-compatible agent baked into the image auto-registers each instance as a managed node on first boot — remote command execution and interactive sessions, with a durable command-history record, and no reliance on an SSH keepalive |

## Roadmap

DeBeOS is ARM-first and, after bring-up, oriented around **self-sufficiency** — building
real software *on the machine* rather than only cross-compiling it. The full staged plan,
with honest status and named open items, is in **[ROADMAP.md](ROADMAP.md)**. In short:
Stage 0 (bring-up) is done; Stage 1 (self-sufficiency — native Rust and `cargo` against
crates.io are proven, and the root filesystem now auto-grows on first boot and stays
crash-safe under power loss; toolchain reliability under heavy native builds remains the
active focus) is in progress; the package ecosystem, platform expansion (Raspberry Pi,
RISC-V) and robustness stages follow.

## Repository layout

DeBeOS's own work lives in two places; everything else is the inherited tree.

| Path | |
|---|---|
| `graviton/` | DeBeOS-specific: docs, build/bake pipeline (CDK), builder-host scripts, OpenSSH packaging, haikuports patches |
| `src/system/kernel/arch/arm64/` | arm64 kernel work — GICv3 ITS, MMU, timers, PMU |
| `src/add-ons/kernel/drivers/network/ether/ena/` | the AWS ENA network driver |
| `src/add-ons/kernel/drivers/power/pl061_acpi_event/` | ACPI GPIO-signalled event driver (the platform power button) |
| `src/bin/nettput`, `src/bin/netprof` | in-image throughput and per-thread CPU measurement tools |
| `src/`, `headers/`, `build/`, `docs/` | inherited from Haiku |

`graviton/docs/` is worth reading before changing anything in the networking or
timekeeping paths — it records what was measured, what was concluded, and which
conclusions were later overturned. Package management and distribution (the native
package manager, and the S3 + CloudFront vending design) are written up under
[`graviton/docs/packages/`](graviton/docs/packages/).

## Building

See [`ReadMe.Compiling.md`](ReadMe.Compiling.md) (inherited, still accurate) for the
general Jam build, and [`AGENTS.md`](AGENTS.md) for how this tree is actually worked
on day to day. Short version, cross-compiling for arm64:

```bash
mkdir generated.arm64 && cd generated.arm64
../configure --cross-tools-source ../../buildtools --build-cross-tools arm64
jam -q @minimum-mmc
```

Bootable EC2 images are produced by the CDK pipeline in
[`graviton/pipeline/`](graviton/pipeline/), which cross-builds, imports the disk
image, registers an AMI, **boots it on real hardware and measures it**, and only then
offers it for promotion. A candidate that will not boot, will not negotiate MTU 9001,
loses throughput, or does not survive a power cycle fails the gate.

## Relationship to Haiku

- **Licence and copyright are unchanged.** Haiku's code remains Haiku's, under the
  MIT licence, with its authors' copyright intact.
- **DeBeOS does not merge from Haiku.** A read-only `haiku-upstream` remote exists for
  reference and lineage only.
- **Fixes here are not submitted upstream by default.** Haiku's contribution policy
  effectively rules out AI-assisted patches, and DeBeOS is not organised around
  feeding them. If something is genuinely valuable to Haiku itself, that is a
  separate, explicit, optional decision — never the default assumption.

Several bugs fixed here are generic Haiku bugs affecting every architecture. They are
fixed *here*, and described plainly in `graviton/docs/`, for anyone who wants them.
