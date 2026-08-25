# DeBeOS brand assets

The mark is a "D" built from circuit traces with a fracture running through it — the
fracture being a deliberate nod to DeBeOS separating from Haiku as a tracked upstream.

**Brand ink: `#196D86`** (RGB 25, 109, 134), sampled from the source artwork.

## Files

| File | Use |
|---|---|
| `debeos-logo.png` + `-512/-256/-128` | full lockup (mark + `debeos` wordmark). README, docs, anywhere with room |
| `debeos-mark.png` + `-512/-256/-128/-64/-32` | mark alone. Icons, avatars, favicons |
| `debeos-mark-mono.png` | flat black on alpha, for single-colour and stencil contexts |

All PNGs have a **transparent background**, derived from the white-background source,
so they sit on light or dark surfaces.

## Known limitation: do not use the mark below 64 px

**The mark is illegible at 16 px and marginal at 32 px.** This was measured, not
assumed — rendered at 16/32/64/128 and inspected. The concentric traces collapse into
a smudge and the shape starts reading as a "P".

An attempt to derive a legible small variant mechanically (flood-fill the channels
between traces to produce a solid silhouette) **failed and was discarded**: the
channels are not enclosed — they open through the right-hand side of the D — so
filling changed almost nothing. The 16 px asset was removed rather than shipped
broken.

**A legible favicon needs a purpose-drawn variant**, not a downscale: a single trace
instead of four concentric ones, noticeably thicker strokes, and probably the fracture
dropped entirely at that size. That is a design task, deliberately left undone rather
than faked.

Use `-64` or larger wherever the size is ours to choose. Where a 16 px icon is
mandatory (a browser favicon), expect a smudge until the small-size mark exists.

## Wordmark note

The wordmark is lowercase `debeos`, which loses the `DeBeOS` capitalisation that
carries the BeOS reference, and reads ambiguously (*de-be-os* versus *deb-eos*).
Written prose should always use **DeBeOS**. Worth revisiting in the wordmark; the mark
itself is unaffected.

## Where the brand appears in the system

On a headless cloud instance there is **no framebuffer at all** on Graviton — proven
with Linux as a control — so Haiku's boot splash (`data/artwork/boot_splash/`) would
be baked in and never displayed. The brand is therefore placed only where it is
actually seen:

- the **kernel boot banner**, visible via `aws ec2 get-console-output --latest`
  (`src/system/kernel/main.cpp`) — added *alongside* the existing `Haiku revision:`
  line, which carries the real `hrev` and is grepped for by tooling
- the **AMI description** (`graviton/pipeline/scripts/import-and-register.sh`)
- this repository's README
- the **About System window** — the product name, and the logo, which now comes from
  `debeos-logo-128.png` instead of the unofficial-distro placeholder
- the **boot loader menu** and its serial banner
  (`src/system/boot/platform/generic/text_menu.cpp`, `src/system/boot/loader/`)
- the **shell login banner** (`data/etc/profile`)

The **AMI name prefix stays `haiku-graviton`**, along with the pipeline, CodeBuild
project and bucket names and the `Project=haiku-graviton` tag. Those are stable machine
identifiers: the tag is load-bearing in an IAM condition, and renaming the prefix would
force CloudFormation to replace the pipeline and its projects.

## The boot splash

The splash is still invisible on a headless cloud instance, but it is now built anyway,
for the **Raspberry Pi 5 / Pi 3** targets, which have displays.

`splash_logo-debeos.png` (372x96) is the splash lockup: the mark at 84 px — above the
64 px floor above — beside the wordmark, laid out horizontally to fit the same canvas
Haiku uses. It is a **lightened** brand tint (`#4FB6D6`), not `#196D86`: the loader
zero-fills the framebuffer to black before blitting, and the brand ink at its normal
lightness only reaches about 3:1 against black. Alpha is not an option either — the
converter strips it and the blit does no blending — so the black background is baked in.

**Nothing in `build/jam/` converts a PNG into a splash.** The arrays live in checked-in
headers, and `images-sans-tm.h` is the one a DeBeOS build uses (`images.h` picks it
whenever `HAIKU_DISTRO_COMPATIBILITY_OFFICIAL` is unset, which is the default). After
changing the art, regenerate it by hand and commit the result:

```
jam -q '<build>generate_boot_screen'
cd data/artwork/boot_splash
generate_boot_screen splash_logo-debeos.png 50 50 splash_icons.png 50 50 \
        ../../../headers/private/kernel/boot/images-sans-tm.h
```

Two things to know before doing that. The logo and the icons are quantized **together**
to one shared 256-colour palette, so replacing the logo re-encodes the icons' 8-bit copy
as well — expect that hunk in the diff and do not read it as damage. And the logo arrays
grew from ~276 bytes to ~15.8 KB, because the slot they replaced held
`splash_logo-empty.png`, a blank image: the "sans-tm" variant historically shipped *no*
logo at all rather than a rebranded one. That is charged against the boot loader's size
budget, so keep an eye on it if the art gets more detailed.
