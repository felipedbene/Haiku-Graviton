# Node.js / V8 on DeBeOS arm64 — feasibility scope (#93)

Scoping only — no full build was attempted this pass (see the verdict for why that
is the right call, not a punt). All sources below are public (HaikuPorts / GitHub).

## TL;DR verdict

- A **HaikuPorts `nodejs` recipe exists and declares arm64 eligible.** It is
  actively maintained (patched 2026-09) and carries a complete, *architecture-neutral*
  Haiku port of V8 in its patchset.
- The realistic blocker is **not** V8's arm64 backend (first-class everywhere) and
  **not** a missing Haiku V8 port (it already exists in-tree). It is, in order:
  **(1) build resources** — RAM at the V8 link step, and a toolchain closure with the
  recipe's *pinned* `python3.10` + ICU >= 74; and **(2) Haiku-arm64 platform-glue
  details** that only surface at runtime (JIT W^X, cache maintenance on generated
  code, signal/stack-unwind on arm64) — the same class this project already patches
  in the kernel.
- **Recommended path: build the existing recipe on a native arm64 Graviton builder**
  with a `>= 8 GB` (ideally 16 GB) RAM instance and the pinned toolchain, via the
  project's established native-`haikuporter`-over-SSM path. **Do not defer for a
  V8 port** — the port is done; this is a resourcing + validation exercise.
  Rough effort: ~0.5-1 day to a first build if the toolchain closure is ready;
  1-3 days if arm64 patch-rebasing or a runtime JIT/signal fix is needed.

## 1. The recipe

- Path: `net-libs/nodejs/nodejs20-20.15.1.recipe` (note: `net-libs`, not `dev-lang`).
- Node.js **20.15.1**, the only nodejs recipe in HaikuPorts (nodejs16 dropped 2025-05).
- `ARCHITECTURES="all !x86_gcc2"` with `SECONDARY_ARCHITECTURES="x86"`. In HaikuPorts
  semantics `all` means *every supported arch including arm64/aarch64*; only the legacy
  `x86_gcc2` ABI is excluded. So arm64 is **declared eligible, but not proven** —
  HaikuPorts publishes no arm64 package repo, and there is no public evidence the
  Haiku+arm64 nodejs combination has ever actually been built or run.
- Build shape: Node's own `./configure --dest-os=haiku` + `make` (Node's **GYP**-driven
  build — **not** standalone-V8 GN/Chromium). Links **shared** system libs:
  `--with-intl=system-icu` (ICU >= 74), shared brotli/cares/libuv/nghttp2/openssl(>=3)/
  zlib, `--without-npm`.
- `BUILD_PREREQUIRES`: `cmd:gcc`, `cmd:ld`, `cmd:make`, `cmd:ninja`, `cmd:pkg_config`,
  **`cmd:python3.10`** (hard-pinned), `cmd:find`, `cmd:which`.

## 2. The V8-on-Haiku port already exists (as a Node patchset)

There is **no standalone V8 recipe** in HaikuPorts (only lighter engines: duktape,
mujs, quickjs-ng, spidermonkey). The Haiku V8 port lives entirely in the nodejs
patchset (`patches/nodejs-20.15.1.patchset`), authored by Haiku developers. It:

- adds `deps/v8/src/base/platform/platform-haiku.cc` — the V8 platform glue
  (timezone cache, mmap/munmap, thread/stack via Haiku's `<OS.h>`). Upstream V8 ships
  no Haiku platform layer; Haiku carries it downstream on the bundled copy.
- adds `V8_OS_HAIKU`/`V8_OS_POSIX` in `v8config.h`;
- in `BUILD.gn`, **disables V8 pointer compression on Haiku** for both arm64 and x64
  (`(v8_current_cpu == "arm64" || v8_current_cpu == "x64") && !is_haiku`) — the key
  Haiku mitigation, and it is **architecture-independent**;
- disables the V8 snapshot on Haiku (`v8_use_snapshot: 0`), forces the build to use
  **gcc, not clang** (`clang%: 0`), links `-lroot -lbsd -lnetwork`, and works around a
  Haiku `sigaction` quirk in `src/node.cc`.
- The only *architecture-conditional* patch (`patches/nodejs_x86-20.15.1.patchset`,
  an ia32 WASM-liftoff fixup) is **x86-only and is skipped on arm64**.

The classic "V8 on a new OS" blockers (W^X/JIT mmap, pointer compression, snapshot,
execinfo/backtrace, pthread/semaphore) are therefore **already addressed in-tree**.
The residual risk is that these patches rebase cleanly and behave on the arm64
codegen path specifically.

## 3. V8 arm64 status

V8's arm64/aarch64 backend is first-class (Android, macOS Apple Silicon, Linux arm64).
The Haiku patch selects the platform by OS (`__HAIKU__`), not CPU, so arm64 codegen +
Haiku glue *should* compose. Treat the combination as **unbuilt/unproven**; the likely
failure mode is Haiku-arm64 platform-glue detail (JIT W^X, i-cache maintenance on
generated code, signal-context/stack-unwind on arm64), not the V8 backend.

## 4. Build cost

V8 is the heavy part (thousands of TUs; a memory-hungry final link).

- **RAM:** ~2 GB per parallel compile job, and the V8/node **link step alone wants
  ~2-4 GB**. A 2-vCPU / 4 GB builder is marginal and will likely OOM at link unless
  jobs are throttled and swap is present. **8 GB+ is the practical floor; 16 GB
  comfortable.** This matches the project's WebKit/rust experience where the gate was
  guest RAM/disk, not codegen.
- **Time:** ~2-4 h on 2 vCPU (throttled); ~25-45 min on a many-vCPU native Graviton
  builder. Disk: budget ~5-8 GB free for the source+build tree.
- **Toolchain:** gcc (recipe forces `clang%:0`), **python3.10** (hard-pinned — same
  pinned-interpreter class of issue this project has hit before, so the builder must
  actually provide `cmd:python3.10`), ninja, make, pkg-config; system ICU >= 74,
  openssl >= 3, libuv, libcares, libnghttp2, brotli as `devel:` packages for arm64.
  Node uses its bundled GYP generator — **no GN needed** (GN is only for
  standalone-V8/Chromium).

## 5. Lighter alternatives (if the goal is "a JS runtime", not Node specifically)

Already in HaikuPorts and far cheaper to build than V8: `quickjs-ng` (small, ES2023,
trivial arm64 build), `duktape`, `mujs`; `spidermonkey` is a real JIT alternative but
similarly heavy. **Node against a system V8 is not viable** — there is no standalone V8
in HaikuPorts and Node upstream does not build against an external V8; Node must build
its bundled V8. If a quick JS-runtime win is wanted while the Node build is set up,
`quickjs-ng` is a near-free arm64 build to prove the category.

## Sources

- Recipe: <https://github.com/haikuports/haikuports/blob/master/net-libs/nodejs/nodejs20-20.15.1.recipe>
- Haiku V8 patchset (platform-haiku.cc, v8config.h, BUILD.gn pointer-compression disable):
  <https://github.com/haikuports/haikuports/blob/master/net-libs/nodejs/patches/nodejs-20.15.1.patchset>
- x86-only patchset (not used on arm64):
  <https://github.com/haikuports/haikuports/blob/master/net-libs/nodejs/patches/nodejs_x86-20.15.1.patchset>
