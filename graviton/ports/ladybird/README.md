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
| `ladybird-audio-haiku.patch` | the `BSoundPlayer` playback backend (see "Audio" below) |
| `angle-shim/` | GLES/EGL headers authored from the Khronos specs, plus the no-device backend |
| `headless-shot/` | the minimal non-Qt chrome that navigates to a URL and screenshots it |
| `package/` | hpkg build scripts for Ladybird and its dependencies |
| `skia-haiku-arm64.patch`, `build-skia.sh` | Skia port and build |
| `build-libpng-apng.sh` | APNG-enabled libpng |
| `provision-deps.sh`, `provision-wave2.sh`, `configure-wave*.sh` | builder provisioning and configure |

## Audio

`ladybird-audio-haiku.patch` adds `Libraries/LibMedia/Audio/PlaybackStreamHaiku.{h,cpp}`
and the two CMake branches that select it. Before it, `Meta/CMake/audio.cmake` took its
`NOT WIN32` arm, failed to find PulseAudio, fell out of the `if` entirely and left
`LADYBIRD_AUDIO_BACKEND` undefined, so `LibMedia` compiled with no
`LIBMEDIA_AUDIO_BACKEND` and every stream became a `NullPlaybackStream` — silence, with
only a configure-time warning to say so. The patch also turns
`LibMedia/CMakeLists.txt`'s trailing `elseif (DEFINED LADYBIRD_AUDIO_BACKEND)` into an
`else()`, so a platform with no backend fails the configure instead of building a silent
browser.

The backend is `BSoundPlayer`, and the shape it ended up with is dictated by what the
media kit does on the calling thread. Each of these was read out of
`src/kits/media/`, not assumed:

- **The node is started once and runs for the stream's whole life**; suspending
  zero-fills rather than stopping it. `BSoundPlayer::Stop(block, flush)` polls
  `IsPlaying()` for up to 250 × 2000 µs and then snoozes `Latency() + 2000`
  (`SoundPlayer.cpp:228-262`), so it can take half a second plus the chain's latency and
  cannot be called from the buffer callback, which is where queued control tasks are
  picked up. `PlaybackStreamAudioUnit` keeps its output unit running for the same reason.
- **`HasData` stays true for the stream's whole life.** Clearing it makes
  `SoundPlayNode` memset the buffer *instead of* invoking the callback
  (`SoundPlayNode.cpp:852-856`), which would silence the stream and strand every queued
  task with it.
- **Volume is applied as software gain in the callback, not through
  `BSoundPlayer::SetVolume()`.** That reaches `BParameter::SetValue`, a synchronous
  request/reply round-trip to the mixer node (`ParameterWeb.cpp:1349+`), and
  `fVolumeSlider` can be NULL. The cost is that Haiku's own per-application volume
  control does not see Ladybird's setting; there is a FIXME on it in the source.
- **Nothing resolves a promise on the calling thread.** `Core::ThreadedPromise::resolve()`
  spins waiting for a rejection handler when `EventLoop::is_running()` is false
  (`ThreadedPromise.h:161-177`), and the caller only attaches `when_rejected()` after the
  call returns, so a synchronous resolve would deadlock. Every promise is resolved from
  the buffer callback.
- **Drain and discard are the same operation.** `Stop()` ignores its `flush` argument
  outright — `SoundPlayer.cpp:237` says `// TODO: flush is ignored` — and nothing else
  gives a handle on the buffers already in flight, so a discard still plays up to one
  chain latency of submitted audio.
- **A partial fill is zero-padded.** The node plays the whole `buffer_size` whether or
  not the callback filled it.
- `Latency()` does a `GetLatencyFor()` round-trip to the media server
  (`SoundPlayer.cpp:266-291`), so it is read once at construction, never from the
  callback. `PerformanceTime()` is a shared-area read and is callback-safe.

The result needs no extra thread, no semaphore and no blocking call anywhere in the audio
path. Device enumeration (`LIBMEDIA_AUDIO_DEVICE_ENUMERATION`) and capture
(`LIBMEDIA_AUDIO_CAPTURE_BACKEND`) are deliberately not wired up; the WASAPI branch is the
precedent for a playback-only backend.
