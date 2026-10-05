# Ladybird on DeBeOS arm64

The [Ladybird](https://github.com/LadybirdBrowser/ladybird) browser engine
(independent; not WebKit) built natively for DeBeOS on AWS Graviton (arm64 Haiku).

**Status: builds, launches its multi-process pipeline, and renders live websites.**
Rendering is Skia CPU raster; the platform has no GPU or GL device, so WebGL is
present but reports no device (see `angle-shim/`).

| Render | Source |
|---|---|
| `live-hackernews-render.png` | the live Hacker News front page, fetched over HTTPS |
| `live-example-render.png` | live example.com (exercises CJK/Arabic/Cyrillic font fallback) |
| `debeos-demo-render.png` | a local modern page (flexbox, grid, gradients, shadows, `background-clip: text`) |

## Install

```
pkgman install ladybird
ladybird-headless-shot --headless=screenshot --screenshot-delay 6 \
    --screenshot-path out.png https://news.ycombinator.com/
```

The package and its new dependencies (`icu78`, `ffmpeg7`, `libpng16` 1.6.53-2 with
APNG, `simdjson` 5.0.2, `libtommath` 1.3.0) are built by the scripts in
`package/`; see `package/README.md`.

## How it got here

The port went in waves. Each write-up records what that wave cleared and how:

| Wave | Write-up | Milestone |
|---|---|---|
| 1 | `README-wave1.md` | full CMake configure against system libraries; only Skia unresolved |
| 2 | `README-wave2.md` | Skia m148 CPU raster, APNG libpng, clean configure |
| 3 | `README-wave3.md` | Rust toolchain wall, cranelift JIT, WebGL/EGL no-device backend, Skia RTTI rebuild, Haiku loader fixes, `AK/ThreadID` IPC fix, `headless-shot`, live rendering |

The full builder reproduction recipe (scripts, wrappers, patches, pkg-config shims,
versions) is in `graviton/ladybird/`.

## Files

| Path | What it is |
|---|---|
| `ladybird-haiku-arm64-wave3.patch` | the current Ladybird tree patch (supersedes the wave 1 and wave 2 patches) |
| `ladybird-cmake-wave1.patch`, `ladybird-haiku-arm64.patch` | wave 1 and wave 2 patches, kept for history |
| `angle-shim/` | GLES/EGL headers authored from the Khronos specs, plus the no-device backend |
| `headless-shot/` | the minimal non-Qt chrome that navigates to a URL and screenshots it |
| `package/` | hpkg build scripts for Ladybird and its dependencies |
| `skia-haiku-arm64.patch`, `build-skia.sh` | Skia port and build |
| `build-libpng-apng.sh` | APNG-enabled libpng |
| `provision-deps.sh`, `provision-wave2.sh`, `configure-wave*.sh` | builder provisioning and configure |
