# Arm MCP server — evaluation for the Graviton/ARM64 work

An investigation into whether Arm's published **Model Context Protocol server** is
worth wiring into this project's tooling. Short answer: **park it for the
kernel/driver work (it cannot help there), and keep it in reserve for one narrow
slice of the HaikuPorts porting round** — an x86→arm64 SIMD inventory-and-rewrite
aid for `ffmpeg`/`mesa`, and nothing else on our list.

Everything here is from Arm's own public sources (the `learn.arm.com` learning
path and its sub-pages, Arm's public `arm-learning-paths` install guides, the
`armlimited/arm-mcp` Docker Hub page, and Arm's `arm/arm-mcp-gemini` wrapper repo),
fetched 2026-08-25. Where a source is silent it is marked so; nothing here was
run against a live server.

## What it actually is

An **x86→arm64 application / container migration assistant** exposed over MCP
(stdio transport). It does **not** contain a deterministic SIMD translator, and it
does **not** know anything about kernel-level architecture. Its own contribution is
to *scan*, *search a knowledge base*, and *score* — the actual code rewriting is
done by whatever LLM front-end you attach to it (Claude Code is a listed client).

Read the marketing with care: the learning-path overview page is AI-generated and
has a documented habit of over-listing capabilities (it separately lists an "EFI
Framebuffer for Nitro" item that, checked, is only a Linux kernel-packaging change,
not a Graviton graphics feature — see `graviton-has-no-video-device` in the project
memory). The install guides are the reliable source; the tool inventory below is
consistent across them.

### Tool inventory (six tools, namespace `arm-mcp/…`)

Names are verbatim from Arm's sources. **No formal input/output JSON schema is
published** for any of them, so the shapes below are as-described, not
schema-verified — a live `tools/list` would be needed to pin them down.

| Tool | Input (as described) | Output (as described) |
|---|---|---|
| `knowledge_base_search` | natural-language query | ranked Arm resources — learning docs, **intrinsic mappings**, software/version arm64-compatibility — with URLs and snippets |
| `check_image` | Docker image `name:tag` | supported-architecture report |
| `skopeo` | container image reference | remote manifest inspection for arm64 support, no image pull |
| `migrate_ease_scan` | workspace path or Git repo + scanner (`cpp` `python` `go` `js` `java`) | inventory of x86-specific code/deps/build-flags/intrinsics needing change; wraps the open-source `migrate-ease` scanner |
| `mca` | assembly | llvm-mca-style per-core perf prediction (IPC, timing, bottlenecks) for a Neoverse core |
| `sysreport_instructions` | (none stated) | *instructions* for running `sysreport` on a target host — not a live report |

A "Performix" SSH-to-target profiling capability is referenced but is **not** a
distinct named tool in any source found.

### How it runs

- **Transport: stdio.** Local only; no HTTP/SSE endpoint.
- **Delivered solely as a Docker image** `armlimited/arm-mcp:latest` (multi-arch,
  ~945 MB, `--pull=always` so it self-updates). Runs via Docker or any drop-in
  (Podman/Finch/Colima/Rancher).
- Canonical launch:
  ```
  docker run --rm -i --pull=always -v "<workspace>:/workspace" \
    -v "<ssh_key>:/run/keys/ssh-key.pem:ro" \
    -v "<known_hosts>:/run/keys/known_hosts:ro" \
    armlimited/arm-mcp:latest
  ```
  The two SSH mounts are **optional** — only for Performix profiling of a remote
  target. Omit them for pure local analysis.

### Auth, dependencies, phone-home

- **No Arm account or API key** documented for the tools.
- Needs Docker running and network access (for registry inspection and
  `--pull=always`).
- Your workspace is bind-mounted into the **local** container; **no documented
  upload of source to Arm.** Registry and knowledge-base calls go out over the
  network. The image is closed-source, so "no source phone-home" is *documented,
  not independently verified* — a network trace would be needed to prove it.

### Config for Claude Code

Arm's own command (writes a project-scoped `.mcp.json`):
```
claude mcp add --scope project --transport stdio arm-mcp -- \
  docker run --rm -i --pull=always -v "$(pwd):/workspace" \
  -v "/path/to/ssh/private_key:/run/keys/ssh-key.pem:ro" \
  -v "/path/to/ssh/known_hosts:/run/keys/known_hosts:ro" \
  armlimited/arm-mcp:latest
```
Equivalent `.mcp.json` block, SSH mounts dropped for local-only use (add them back
for Performix):
```json
{
  "mcpServers": {
    "arm-mcp": {
      "command": "docker",
      "args": [
        "run", "--rm", "-i", "--pull=always",
        "-v", "${workspaceFolder}:/workspace",
        "armlimited/arm-mcp:latest"
      ]
    }
  }
}
```
Use an absolute path if your Claude Code build doesn't expand `${workspaceFolder}`.
**Enable it per-workspace, never globally** (see cost, below).

### Maturity / caveats

Real, Arm-published (`armlimited`, verified Docker publisher), 10K+ pulls, image
rebuilt ~daily. But **closed-source, only a `latest` tag (no pinned versions —
a reproducibility/supply-chain caveat given `--pull=always`), no published license,
and no published tool schemas.** Not merely a learning artifact, but not a
hardened, versioned dependency either.

## Fit against this project's work

### arm64 kernel internals — no help

GICv3 ITS, VMSAv8 page tables, LSE atomics, ESR/exception decode, EFI handoff are
none of the things this tool models. Its knowledge base is scoped to *userland
intrinsics and package compatibility*; the authoritative source for
register/instruction semantics is the Arm Architecture Reference Manual, which our
loop already uses, followed by measurement on real Graviton hardware. `mca` scores
userland assembly against a generic Neoverse model, but the kernel arch code is not
SIMD-bottlenecked and our discipline is to measure on the actual core. **Net
overhead once its six tool schemas are loaded into a session.**

### ENA driver — not relevant

The hard problems on record are concurrency, DMA, locking, and receive hand-off
latency — not SIMD or container migration. The vendored `ena-com/` layer is already
portable, arm64-clean C, so a migration scan would flag little of value.

### HaikuPorts SIMD ports — the only real case, and narrower than it looks

This is where it is on-topic, with three qualifiers:

1. **Only two of the five `jam all` blockers are actually SIMD-heavy.** `mesa` and
   `ffmpeg` carry large SSE/AVX bodies — *and both already ship extensive upstream
   aarch64/Neon paths*, so their blocker is often arm64 **build-system detection**,
   which this tool's docs admit it does **not** handle (it only strips
   architecture-specific build flags; no autoconf/CMake migration). `giflib`,
   `glu`, and `fluidlite` are **not** meaningfully SIMD — their blockers are almost
   certainly config/recipe issues, outside the tool's scope entirely.
2. **Most of its tools don't apply to our build.** We build **natively on Graviton
   with Jam + haikuporter, on Haiku, not Linux/Docker.** That eliminates
   `check_image`, `skopeo`, the Docker-on-Arm-runner validation loop, and
   `sysreport`. What survives is `migrate_ease_scan` and `knowledge_base_search` —
   two of six.
3. **The edge over lighter references is modest.** For SSE, the drop-in
   `sse2neon.h` header compile-time-translates most intrinsics — often a one-line
   `#include` beats an AI rewrite. AVX coverage is thinner, which is where a
   scan-plus-Arm-KB workflow earns its keep. The scanner's real advantage over
   `grep -r _mm_` is *completeness of inventory* across a large tree.

### Lighter alternatives already cover most of it

- Kernel: the Arm ARM + hardware readback dominate; the MCP adds nothing to beat.
- SIMD ports: the standalone open-source `migrate-ease` scanner (which the MCP
  wraps) runs **without adopting the MCP at all**; `sse2neon.h` and Arm's public
  Neon porting guides cover most SSE work. The MCP's marginal value is wiring those
  together plus semantic KB search.
- Cost: an MCP server loads ~6 tool schemas into every session it's enabled in and
  adds a maintenance/trust surface (closed-source, unpinned `latest`). For a project
  that is ~90% kernel/driver internals where the tool is inert, that is persistent
  dead weight — mitigated only by enabling it strictly per-workspace.

## Verdict

**Park for kernel + ENA. Trial, narrowly, for the `mesa`/`ffmpeg` SIMD slice
only — never enabled in kernel/driver sessions.**

**First task to point it at**, if/when the HaikuPorts round resumes: run
`migrate_ease_scan` against a checked-out **`ffmpeg`** tree (the clearest x86
intrinsic bodies), produce an SSE/AVX inventory, then attempt an AVX2→Neon/SVE
rewrite of one isolated module.

- *Proves its value* if the scan surfaces intrinsic sites more completely than
  `grep -r _mm_`, and the KB-guided rewrite compiles clean under haikuporter and
  passes ffmpeg's tests **with less effort than dropping in `sse2neon.h`**.
- *Disproves it* if the real blocker turns out to be autoconf/arm64 detection or a
  Jam-recipe issue (which it does not handle), or if ffmpeg's existing upstream
  aarch64 paths already cover the hot code — making the whole SIMD-rewrite workflow
  moot.

## What a hands-on trial would still need to establish

- Exact input/output schema of each tool (not published — inspect via live
  `tools/list`).
- Whether the knowledge base is bundled in-image or queried remotely.
- Confirming no source-code phone-home (network trace against the running image).
- The server's implementation language, version, and any embedded license.
