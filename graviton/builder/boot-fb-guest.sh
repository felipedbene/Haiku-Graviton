#!/bin/bash
# boot-fb-guest.sh -- boot a Haiku arm64 guest that has a REAL framebuffer, on a
# Graviton .metal host, and expose the display so the host can photograph it.
#
# WHY THIS EXISTS
#
# Graviton EC2 instances expose no display device at all -- no VGA, no
# framebuffer (proven with Linux as a control). Our baked images therefore set
# TARGET_SCREEN, which sends app_server to RemoteHWInterface, and that interface
# cannot be captured on the server side: FrontBuffer() returns NULL and
# ReadBitmap() forwards RP_READ_BITMAP and waits for the *client* to send pixels
# back. The pixels only ever exist in the client. So "run `screenshot` in the
# guest" is a dead end on an instance.
#
# A QEMU/KVM guest on the same Graviton silicon can be given a synthetic display
# instead: `-device ramfb` is a plain linear framebuffer that UEFI publishes
# through GOP, which is exactly what haiku_loader consumes. The kernel then
# publishes /dev/graphics/framebuffer, app_server binds it with
# AccelerantHWInterface + framebuffer.accelerant, and the host can dump the
# display surface with the QEMU monitor's `screendump` -- no VNC client, and
# nothing running inside Haiku.
#
# The monitor socket is the point of the whole arrangement; VNC is wired up only
# as a fallback and is not needed.
#
#   usage: boot-fb-guest.sh <dir> <image> [sshPort] [vncDisplay]
#
#     dir         working directory; logs/ and shots/ are created under it
#     image       disk image (raw or qcow2; format is probed)
#     sshPort     host port forwarded to guest tcp/22   (default 2240)
#     vncDisplay  VNC display number, i.e. 5900+n       (default 3)
#
# Pick an ssh port nothing else is using -- `ss -ltn` first. Other build guests
# on the same host hold ports of their own and must not be disturbed.
set -euo pipefail

D=${1:?usage: boot-fb-guest.sh <dir> <image> [sshPort] [vncDisplay]}
IMAGE=${2:?need a disk image}
PORT=${3:-2240}
VNCD=${4:-3}

for f in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
	[ -f "$f" ] && FW="$f" && break
done
: "${FW:?no aarch64 UEFI firmware found (install qemu-efi-aarch64 / ovmf)}"
[ -w /dev/kvm ] || { echo "no /dev/kvm -- this needs a .metal host" >&2; exit 1; }

mkdir -p "$D/logs" "$D/shots"
rm -f "$D/logs/boot.log" "$D/mon.sock"

# KVM on Graviton cannot emulate GICv2, so GICv3 is mandatory; ITS gives MSI-X.
# virtio-net needs vectors=0: with MSI-X the guest transmits but never receives.
exec qemu-system-aarch64 -M virt,gic-version=3,its=on,accel=kvm -cpu host \
	-m "${MEM:-6144}" -smp "${SMP:-8}" -bios "$FW" \
	-device ramfb \
	-vnc "127.0.0.1:$VNCD" \
	-serial "file:$D/logs/boot.log" \
	-monitor "unix:$D/mon.sock,server,nowait" \
	-drive "if=none,id=hd,file=$IMAGE" \
	-device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \
	-device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
	-netdev "user,id=n0,hostfwd=tcp::$PORT-:22" \
	-device virtio-net-pci,netdev=n0,vectors=0
