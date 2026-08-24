# DeBeOS — ARM-first OS (AGENTS.md)

Guidance for AI agents and human contributors working in this tree.

## What this is

**DeBeOS**, an independent operating system project descended from
[Haiku](https://www.haiku-os.org/) and BeOS (see [README.md](README.md)). Haiku and
BeOS are the **historical lineage** and the bulk of the code is still Haiku's, under
its own licence — but DeBeOS **does not track or merge from upstream Haiku**. A
read-only `haiku-upstream` remote exists for reference and lineage only, and is never
merged.

Practically, that means: **work here is DeBeOS's own implementation, not a patch
meant for Haiku.** Frame driver and kernel changes that way. Several bugs fixed in
this tree are generic Haiku bugs affecting every architecture; they are fixed here and
documented plainly, and that is the end of the obligation. If something is genuinely
upstream-worthy for Haiku itself, say so as a separate, explicit, optional path —
never as the default (Haiku's contribution policy rules out most AI-assisted patches
anyway).

DeBeOS is **ARM-first**: AWS Graviton / ARM64 EC2 is the current flagship, with
Raspberry Pi 5, Raspberry Pi 3 and RISC-V on the roadmap. The active work
concentrates in two areas:

- **arm64 kernel** — `src/system/kernel/arch/arm64/`
  - GICv3 ITS interrupt controller: `gicv3_its.cpp`, `gicv3_its.h`, `gicv3_regs.h`
  - MMU / page tables: `VMSAv8TranslationMap.cpp`
- **ENA network driver** (AWS Elastic Network Adapter) —
  `src/add-ons/kernel/drivers/network/ether/ena/` (`ena.cpp`, `ena.h`,
  `ena_plat.cpp`, and the vendored `ena-com/` common layer)
  - Fault-injection tester: `src/bin/ena_fault/`

When touching these, prefer keeping the vendored `ena-com/` layer close to its
upstream shape; Haiku-specific glue lives in `ena_plat.cpp`/`ena.cpp`.

## Repository layout

- `src/` — all source (kernel `system/`, drivers `add-ons/`, `apps/`, `kits/`,
  `servers/`, `bin/`, `libs/`, `tests/`)
- `headers/` — public (`os/`), private (`private/`), and POSIX headers
- `docs/` — developer and API docs (`docs/develop/`, `docs/user/`)
- `build/jam/` — Jam build logic and user build config docs
- `3rdparty/` — third-party integrations and helper scripts
- `configure`, `Jamfile`, `Jamrules` — top-level build entry points

Each git commit maps to one logical change; kernel-arch and driver changes are
kept in separate commits.

## Build system — Jam (not make, not Brazil)

Full details are in `ReadMe.Compiling.md`. Quick reference:

Configure a build for ARM64 into a `generated.*` directory (from a non-Haiku
host, cross-compiling with the `buildtools` checkout beside this repo):

```bash
mkdir generated.arm64 && cd generated.arm64
../configure --cross-tools-source ../../buildtools --build-cross-tools arm64
```

`configure` writes `BuildConfig` under `generated/build/`. Re-run it only if you
change `configure` itself or update the cross-tools; otherwise just re-run `jam`.

Common `jam` invocations (run from the repo root or from inside a `generated.*`
dir; `-q` = quit on first error):

```bash
jam -q @nightly-anyboot      # bootable anyboot image (haiku-nightly-anyboot.iso)
jam -q @nightly-raw          # raw disk image (haiku.image)
jam -q <Target>              # build one component, e.g. `jam -q kernel_arm64`
jam -qa <Target>             # force rebuild of a component
```

To build a single component from its source dir when your output dir isn't the
default `generated/`:

```bash
jam -q -sHAIKU_OUTPUT_DIR=<path to generated dir> <Target>
```

Building for ARM/ARM64 also needs `mkimage` installed (see `ReadMe.Compiling.md`
→ "Haiku for ARM"). See `build/jam/UserBuildConfig.ReadMe` and
`UserBuildConfig.sample` to customize a build.

> Note: builds are large and slow. Redirect `jam` output to a log and inspect
> the tail rather than streaming everything:
> `jam -q @nightly-anyboot > build.log 2>&1; tail -n 40 build.log`

## Coding style

- Indentation: **tabs**, width 4 — for `.c`, `.cpp`, `.h` (see `.editorconfig`).
- Follow the
  [Haiku Coding Guidelines](https://www.haiku-os.org/development/coding-guidelines/)
  and match the style of the surrounding file.
- Use inclusive terminology (allowlist/denylist, primary/replica, etc.).
- Keep comments at the density of the file you're editing; explain *why*, not
  *what*, for non-obvious kernel/driver logic.

## Testing & verification

- There is no fast unit-test loop for kernel/driver changes — verify by
  building the relevant target, then booting an image under an emulator
  (QEMU is the usual choice for ARM64) or on real Graviton hardware.
- The ENA driver has a fault-injection path (`ena_fault`, driven by an ioctl)
  used to exercise reset/error-unwind and race conditions — use it when
  touching device lifetime, reset, or descriptor-reclaim code.
- After any change, always build the affected target before considering the
  work done.

### AWS Graviton test target

Booting is verified on real AWS Graviton (arm64) EC2 instances in **us-west-2**.

- Each build is baked into an AMI named `haiku-<feature>-<ts>` and tagged
  `Project=haiku-graviton`. The **golden** image is whichever AMI carries the
  tag **`canonical=true`** — find it by that filter, never by newest date.
- `graviton/scripts/haiku-canonical` maintains the invariant *exactly one AMI
  is canonical*: `haiku-canonical check` guards it; `haiku-canonical promote
  <ami-id>` atomically moves the tag to a freshly baked image. Call `promote`
  as the last step of the bake loop.
- Test instances are **SSM-managed nodes** — inspect/drive them with
  `aws ssm` (e.g. `describe-instance-information`, `send-command`,
  `start-session`) rather than SSH.
- Credentials: `ada credentials update --account <id> --role Admin --provider
  isengard --once` (this Isengard account has no `ReadOnly` role). Keep to
  describe/list unless a change is intended.

## Contributing / workflow

- `origin` is DeBeOS: `github.com/felipedbene/Haiku-Graviton` (repository not yet
  renamed). `graviton` is the working branch and DeBeOS's own trunk. The `master`
  branch is a historical mirror of upstream Haiku and is **no longer updated**.
- **`haiku-upstream` is read-only and is never merged.** Historical
  `Merge branch 'haiku:master'` commits predate the split and stay as-is for lineage
  credit; there will be no more of them. `.gitreview` (haiku.git / Gerrit) has been
  removed — it was inherited config and was never the push target.
- Work lands via topic branches merged into `graviton`. **Never commit directly to
  `graviton`.**
- Follow upstream code style; the C++ style guide still applies:
  <https://www.haiku-os.org/development/coding-guidelines/>.
- Do not commit or push unless asked. When you do, keep unrelated changes in
  separate commits and write focused, imperative commit subjects (see
  `git log` for the house style, e.g. `arm64: ...`, `ena: ...`).

## Useful references

- Source browsers: <https://git.haiku-os.org/> and
  <https://grok.nikisoft.one/opengrok/>
- API docs: <https://api.haiku-os.org>
- Issue tracker: <https://dev.haiku-os.org/>
