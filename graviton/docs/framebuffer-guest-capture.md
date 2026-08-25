# Seeing the screen: a Haiku arm64 guest with a real framebuffer, photographed

We can build GUI applications for arm64, but until now we could not look at one.
This is the rig that closes that gap, and the evidence that it is not lying.

## Why an instance cannot do this

A Graviton EC2 instance has no display device at all — no VGA, no framebuffer.
That was settled by booting Linux on the same instance shape as a control and
finding nothing either. There is no framebuffer to be had.

The images we bake therefore set `TARGET_SCREEN`, and `app_server` builds a
`RemoteHWInterface`: it listens on loopback and forwards a *display list* to a
remote client that renders it. That interface cannot be captured from the server
side, and not by accident —

- `RemoteHWInterface::FrontBuffer()` returns `NULL`; there is no local surface.
- `RemoteDrawingEngine::ReadBitmap()` sends `RP_READ_BITMAP` and then waits for
  the *client* to send pixels back.

The pixels only ever exist in the client. Running `screenshot` inside such a
guest cannot work without writing a protocol client to be screenshotted.

## What does work

Run the guest under QEMU/KVM on a Graviton `.metal` host — still real Graviton
silicon — and give it a synthetic display:

    -device ramfb

`ramfb` is a plain linear framebuffer set up through `fw_cfg`; UEFI publishes it
as a GOP, which is exactly the interface `haiku_loader` already consumes on
arm64. From there the ordinary path runs itself: the kernel publishes
`/dev/graphics/framebuffer`, and `app_server` binds it through
`AccelerantHWInterface` with `framebuffer.accelerant` — a *local* framebuffer,
not the remote interface.

Capture is then a host-side concern. QEMU's monitor has `screendump <file>`,
which writes the live display surface to a PPM. Nothing needs to run inside
Haiku, and no VNC client is needed (`-vnc` is wired up in the boot script only
as an unused fallback).

    graviton/builder/boot-fb-guest.sh <dir> <image> [sshPort] [vncDisplay]
    graviton/scripts/qemu-screendump <dir>/mon.sock <dir>/shots <name>

Boot the guest from a **qcow2 overlay** over an existing image rather than a
copy. Writes land in the overlay, the base is never touched, and the cost is a
few megabytes instead of the twenty-odd gigabytes a builder image weighs:

    qemu-img create -f qcow2 -F raw -b <base.image> <dir>/gui.qcow2

## The one change the image needed

Our builder images ship a launch override that declares
`x-vnd.Haiku-app_server`, `x-vnd.Be-TRAK` and `x-vnd.Be-TSKB` `disabled`. That
is correct for a headless builder — on a machine with no framebuffer
`app_server` cannot construct a `Desktop`, and being declared a *service* it is
restarted with no back-off, roughly 180 times per console interval, which
starves the box and kills every other `BApplication` holding an `app_server`
link, `net_server` included, taking the network and therefore `sshd` with it.

On a guest that *does* have a framebuffer it is exactly the wrong setting, and
its symptom is quiet: `app_server`, `Tracker` and `Deskbar` are simply absent
from `ps`, and `launch_roster list` does not mention them at all — not even as
disabled entries. Remove the override, `sync`, and restart the guest.

`TARGET_SCREEN` turned out not to need touching, which is worth knowing rather
than guessing at: the seed image used here carries no
`~/config/settings/boot/UserSetupEnvironment`, so nothing sets it. And in any
case `ScreenManager::AcquireScreens()` hands out an already-registered
accelerant screen *before* it considers building a `RemoteHWInterface` from the
target string — so on a machine with a working framebuffer the local interface
wins even when `TARGET_SCREEN` is set. Verify which interface was chosen; do not
infer it from the environment.

## Verifying the interface, not assuming it

Three independent checks, all cheap:

    # 1. something opened the framebuffer and asked for its accelerant
    grep -a 'framebuffer: acc:' <dir>/logs/boot.log
    #    -> framebuffer: acc: framebuffer.accelerant

    # 2. app_server has the accelerant add-on mapped into its address space
    ssh ... 'listimage <app_server team>' | grep accelerant
    #    -> /boot/system/add-ons/accelerants/framebuffer.accelerant

    # 3. app_server reports a mode
    ssh ... screenmode
    #    -> Resolution: 1024 768, 24 bits, 60 Hz

Check 2 is the decisive one: a `RemoteHWInterface` maps no accelerant.

Note that the QEMU serial log is a binary file as far as `grep` is concerned —
`grep -E pattern file` prints nothing while `grep -c` counts fine. Always
`grep -a`.

## A capture that cannot quietly lie

A `screendump` that returns cleanly is not evidence of a picture. An unpainted
display produces a file of exactly the right size, full of one colour, and exits
0. A silently blank capture is also indistinguishable from "the application does
not render", which is the expensive failure: it sends the next person to debug
something that is working.

So `qemu-screendump` always prints the pixel content — distinct colour count,
the modal colour and its share, the fraction of non-modal pixels — and returns a
verdict. Measured on this rig at 1024x768x24:

| subject                                | distinct colours | non-modal pixels | verdict |
| -------------------------------------- | ---------------- | ---------------- | ------- |
| synthesised uniform black frame        | 1                | 0.0000%          | BLANK   |
| bare desktop: Deskbar, Tracker, icons  | 1482             | 1.5589%          | content |
| the same desktop plus one app window   | 2383             | 34.5140%         | content |

The middle row is the trap. A correct, fully painted Haiku desktop is **98.44% a
single colour**, because that colour is the default workspace blue
`rgb(51,102,152)`. "Almost all one colour" is therefore not the blank signature.
The blank signature is at most two distinct colours, or under 0.5% of pixels
differing from the modal one — and the threshold was confirmed by feeding the
tool a synthesised uniform frame and watching the verdict fire, rather than by
trusting a check that had never triggered.

Two captures taken three seconds apart with nothing happening differed by a few
hundred pixels — a clock and an uptime counter repainting. That is the idle noise
floor; a diff of that order means nothing changed.

## Driving the guest

Launch GUI applications over ssh. An ssh session has `TARGET_SCREEN` unset, so
it asks for a `Desktop` keyed on `(uid, NULL)` — which is the same key
`app_server` used, so the window appears on the framebuffer:

    ssh ... 'nohup /boot/system/apps/AboutSystem >/dev/null 2>&1 &'

The QEMU monitor's `mouse_move` did **not** move the Haiku cursor here; it
queues *relative* motion and the guest has an absolute tablet. Pointer control
is unverified — do not build on it.

## Rebooting

`shutdown -r` inside the guest hangs: the shutdown team stays in `ps` and the
guest never comes back. Instead `sync` inside the guest, kill QEMU on the host,
and re-run the boot script. The `sync` is load-bearing — file data is otherwise
not flushed, and a settings change made and not synced is simply not on disk
after the restart.
