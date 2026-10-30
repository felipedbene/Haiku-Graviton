# EXTRACT — pulling the JS bundle out of the Claude Code Bun binary

Scripted and repeatable per version. The Linux binary is parsed as data only; it is
executed solely on the Linux reference box (`--version`, for comparison), never on DeBeOS.

## Inputs (pinned, verified)

| What | Version | Verification |
|---|---|---|
| `@anthropic-ai/claude-code-linux-arm64` | 2.1.314 | npm `dist.integrity` `sha512-aVv8UGdpMs2AXbFPGiFKtYlF/1P0kNrSwUrPx9a/hAzAW+W0x4e0fdzVYzblsL0KaNSD8VLLFq8X9m/8Jj5HhA==` (checked by `npm pack`); `package/claude` sha256 `a66c0a8f5d0c87a439953d797985192bbcbaefbce2e0d7b2eaa8df30ef0f3ef2` (identical on macOS and Linux) |
| Reference Bun (Linux only) | 1.4.4, rev `38afee6c84fcf48c0edb2466aa4b27d287a59c30` | `SHASUMS256.txt` from the GitHub release |
| Reference Node (Linux only) | v22.23.4 | `SHASUMS256.txt` from nodejs.org |

## Method

`tools/extract-bun-bundle.py <binary> <outdir>` (Python 3 stdlib only, read-only on input):

1. Find ELF section `.bun`. Layout: `u64 len | payload | "\n---- Bun! ----\n"`.
2. Read the 32-byte `Offsets` struct at the payload tail: byte_count, module table
   pointer, entry_point_id, compile_exec_argv pointer, flags.
3. Walk the module table: 52-byte records = 6 `{u32 off, u32 len}` pointers
   (name, contents, sourcemap, bytecode, module_info, bytecode_origin_path) + 4 flag
   bytes (encoding, loader, module_format, side).
4. Write each module's `contents` to `outdir/<name minus "/$bunfs/">`, so `outdir`
   can stand in for `/$bunfs`. Write `manifest.json` (per-module sizes + sha256,
   entry point, argv, bytecode region hash).

Every size is validated; an unknown layout (new Bun version) fails loudly instead of
being guessed.

**The bytecode is deliberately not extracted.** Each module's bytecode pointer runs
from its own start to the end of a single shared region (102.9 MB). Writing it per
module produced 75 GB before I stopped it (first version of the script). That region
is JavaScriptCore bytecode for Bun 1.4.4's exact JSC build, so no other engine can run
it. Every module also ships its full source (`contents`), and the source is what we use.

## Result for 2.1.314 (Linux arm64 reference box, `tools/linux-extract-run.sh`)

```
modules=2871 entry=/$bunfs/root/cli argv='--zero-fill-buffers'
contents=54.3MB sourcemap=0.0MB module_info=0.0MB bytecode_region=102.9MB (not extracted)
=== reference: shipped binary ===
2.1.314 (Claude Code)
=== 3a: stock bun, extracted bundle ===
2.1.314 (Claude Code)
rc=0
Usage: claude [options] [command] [prompt]
...
rc=0
```

Module mix: 2,638 JS chunks, 127 zstd-compressed assets (`*.zst`: skill docs, `.mjs`
helpers), 73 `.md` and 23 `.txt` (loader "text"), 2 woff2 fonts, 2 N-API addons
(`clipboard-napi.node` 976 KB, `audio-capture.node` 459 KB, both Linux ELF).

## How the extracted bundle is run

Every module imports its siblings by absolute specifier (`import … from
"/$bunfs/root/chunk-xxxx.js"`), and assets and addons are opened by `/$bunfs/...`
path. For the Linux baseline, `ln -s <outdir> '/$bunfs'` runs it with zero bytes
modified, as `bun --zero-fill-buffers '/$bunfs/root/cli'` (the compile argv is mirrored).
**Open decision for DeBeOS:** keep a `/$bunfs` symlink (Haiku's `/` is a rootfs, so it
would have to be recreated every boot) or relocate the specifiers with a scripted
rewrite to an install prefix (recorded in PATCHES.md). Not chosen.

## Repeat for a new version

```
bash tools/linux-setup.sh                 # once per box
bash tools/linux-extract-run.sh 2.1.315   # extract + baseline
python3 tools/inventory.py /opt/cc/v2.1.315/bunfs   # RUNTIME-DEPS census
```
