#!/bin/bash
# stream-fb-guest.sh -- boot a Haiku arm64 guest with a REAL framebuffer on a
# Graviton .metal host and STREAM its display to a VNC client, rather than
# photographing it one frame at a time.
#
# This is the Route-2 (GitHub #118) Stage-1 launcher. It is the streaming sibling
# of boot-fb-guest.sh: same synthetic-display trick (ramfb -> UEFI GOP ->
# /dev/graphics/framebuffer -> framebuffer.accelerant, no Haiku change), same
# proven usb-tablet+usb-kbd input, but VNC is now the POINT, not a fallback.
#
#   * VNC binds to a UNIX SOCKET, not a TCP port, so the display never touches the
#     network in the clear. You carry the socket out over the existing
#     SSH-over-SSM tunnel and connect a stock VNC client (macOS Screen Sharing,
#     RealVNC, TigerVNC) to the local end. QEMU owns the framebuffer and its damage
#     tracking, so you get compressed damage updates, real input, a correct cursor,
#     and reconnect that always repaints (the host holds the pixels).
#   * The monitor socket stays available so qemu-screendump can still take a
#     verdict-bearing capture of the SAME surface -- the honest instrument for
#     "is it actually painting?" that a VNC client's own view cannot give you.
#
# Measured on this transport (bootstrap image, 1280x800): keystroke->framebuffer
# p50 31 ms / p95 81 ms (floored by QEMU's 30 ms VNC poll, not Haiku); idle full
# frame 9.8 kB ZRLE; reconnect 8/8 clean. See docs/vfb-route2-streaming.md.
#
# PREREQUISITE, and it is the whole ballgame for a REAL session (not just a
# screendump target): the guest image must have LIVE NETWORKING and must not
# carry the headless builder's launch override that disables app_server/Tracker/
# Deskbar. Bootstrap-flavoured images have dead net_server and cannot be a session.
# Confirm on a FULL (non-bootstrap) image -- that is the M0 gate in the design doc.
#
#   usage: stream-fb-guest.sh <dir> <image> [sshPort]
#
#     dir       working directory; logs/ and shots/ are created under it
#     image     disk image (raw or qcow2; format is probed). Prefer a qcow2
#               overlay over the base so writes never touch the base image:
#                 qemu-img create -f qcow2 -F raw -b <base.image> <dir>/gui.qcow2
#     sshPort   host port forwarded to guest tcp/22 (default 2240). Pick one
#               nothing else uses -- `ss -ltn` first; other build guests on the
#               same metal hold their own ports and must not be disturbed.
#
# env: MEM (MiB, default 6144), SMP (default 8), DISPLAY_DEV (default ramfb;
#      set to virtio-gpu-pci once the Stage-2 guest-driver fixes have landed).
set -euo pipefail

D=${1:?usage: stream-fb-guest.sh <dir> <image> [sshPort]}
IMAGE=${2:?need a disk image}
PORT=${3:-2240}
DISPLAY_DEV=${DISPLAY_DEV:-ramfb}

for f in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
	[ -f "$f" ] && FW="$f" && break
done
: "${FW:?no aarch64 UEFI firmware found (install qemu-efi-aarch64 / ovmf)}"
[ -w /dev/kvm ] || { echo "no /dev/kvm -- this needs a .metal host" >&2; exit 1; }

mkdir -p "$D/logs" "$D/shots"
rm -f "$D/logs/boot.log" "$D/mon.sock" "$D/vnc.sock"

VNC_SOCK="$D/vnc.sock"
cat >&2 <<EOF
streaming display over UNIX socket: $VNC_SOCK
monitor socket (for qemu-screendump verdicts): $D/mon.sock

To view from your workstation, tunnel the socket out over SSH-over-SSM and point a
VNC client at the local end. With a bridge that maps a local TCP port to the
remote unix socket (e.g. socat), the client connects to localhost:<localPort>:

  # on the metal host, if your client needs TCP:
  #   socat TCP-LISTEN:5959,bind=127.0.0.1,reuseaddr,fork UNIX-CONNECT:$VNC_SOCK
  # then carry 127.0.0.1:5959 out over the project's SSH-over-SSM tunnel and
  # connect a stock VNC client (vnc://localhost:5959).

Take a verdict-bearing capture of the same surface any time with:
  graviton/scripts/qemu-screendump $D/mon.sock $D/shots frame
EOF

# GICv3 is mandatory under KVM on Graviton (no GICv2 emulation); its=on gives
# MSI-X. virtio-net needs vectors=0 or the guest transmits but never receives.
# usb-tablet + usb-kbd on qemu-xhci are the ONLY input path that binds on Haiku
# (virtio-input does not); the QEMU monitor's mouse_move does not move the cursor.
exec qemu-system-aarch64 -M virt,gic-version=3,its=on,accel=kvm -cpu host \
	-m "${MEM:-6144}" -smp "${SMP:-8}" -bios "$FW" \
	-device "$DISPLAY_DEV" \
	-vnc "unix:$VNC_SOCK" \
	-serial "file:$D/logs/boot.log" \
	-monitor "unix:$D/mon.sock,server,nowait" \
	-drive "if=none,id=hd,file=$IMAGE" \
	-device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \
	-device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
	-netdev "user,id=n0,hostfwd=tcp::$PORT-:22" \
	-device virtio-net-pci,netdev=n0,vectors=0
