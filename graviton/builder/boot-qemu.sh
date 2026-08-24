#!/usr/bin/env bash
# Boot a Haiku arm64 MMC image under QEMU, in one of three profiles.
#
#   tcg     Stage A -- pure emulation, GICv2. This is what upstream Haiku
#                      supports today; the baseline that must boot first.
#   gicv3   GICv3 under TCG. No KVM needed, so this is the fast iteration loop
#                      for developing the GICv3 driver that Stages B and C need.
#   kvm     Stage B -- KVM on a Graviton .metal host. Haiku instructions run
#                      natively on Graviton3 cores. GICv3 + virtio-PCI.
#   nitro   Stage C -- development profile approximating an EC2 Nitro guest:
#                      GICv3, ACPI, NVMe root, no virtio. Lets us build the
#                      GICv3/ITS/NVMe path without burning AMI import cycles.
#   ssh     Headless SSH test rig: like kvm but with NO framebuffer at all
#                      (app_server cannot start, exactly as on EC2) and
#                      tcp/2222 on the host forwarded to tcp/22 in the guest.
#
# IMPORTANT (measured): virtio-net must be given `vectors=0`. With MSI-X
# enabled, virtio_net transmits fine but never receives -- DHCP OFFERs are on
# the wire (verified with -object filter-dump) and are simply never delivered to
# the guest, so the interface stays unconfigured forever. Forcing legacy INTx
# with vectors=0 makes it work. This is the same GIC-ITS/MSI hazard recorded in
# docs/gap-analysis.md section 4.
set -euo pipefail

PROFILE="${1:-tcg}"
IMAGE="${IMAGE:-/opt/haiku/haiku/generated.arm64/haiku-mmc.image}"
SMP="${SMP:-4}"
MEM="${MEM:-4096}"

# AAVMF/edk2 firmware, named differently across distros
for f in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
         /opt/homebrew/share/qemu/edk2-aarch64-code.fd; do
  [ -f "$f" ] && FW="$f" && break
done
: "${FW:?no aarch64 UEFI firmware found (install qemu-efi-aarch64 / ovmf)}"

# VNC=1 exposes the guest display on 127.0.0.1:5901 for an SSH tunnel:
#   ssh -i ~/.ssh/haiku-graviton.pem -L 5901:127.0.0.1:5901 ubuntu@<host>
# then point any VNC client at localhost:5901. RDP would need a server inside
# Haiku; QEMU speaks VNC natively, so nothing is required of the guest.
if [ "${VNC:-0}" = "1" ]; then
  display=(-vnc 127.0.0.1:1 -serial file:/opt/haiku/logs/console.log)
else
  display=(-display none -serial mon:stdio -nographic)
fi
common=(-m "$MEM" -smp "$SMP" -bios "$FW" -device ramfb "${display[@]}")

case "$PROFILE" in
  tcg)
    # GICv2: upstream Haiku has no GICv3 driver yet.
    exec qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 "${common[@]}" \
      -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \
      -device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
      -drive if=none,id=hd,file="$IMAGE",format=raw \
      -device virtio-net-pci,netdev=n0 -netdev user,id=n0
    ;;
  gicv3)
    exec qemu-system-aarch64 -M virt,gic-version=3,its=on -cpu cortex-a72 "${common[@]}" \
      -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \
      -device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
      -drive if=none,id=hd,file="$IMAGE",format=raw \
      -device virtio-net-pci,netdev=n0,vectors=0 -netdev user,id=n0
    ;;
  kvm)
    # NOTE: KVM on Graviton3 does NOT support GICv2 emulation (verified:
    # "KVM does not support GICv2 emulation"). GICv3 is mandatory here.
    [ -w /dev/kvm ] || { echo "no /dev/kvm -- are you on a .metal instance?" >&2; exit 1; }
    exec qemu-system-aarch64 -M virt,gic-version=3,accel=kvm -cpu host "${common[@]}" \
      -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \
      -device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
      -drive if=none,id=hd,file="$IMAGE",format=raw \
      -device virtio-net-pci,netdev=n0,vectors=0 -netdev user,id=n0
    ;;
  nitro)
    # Approximates Nitro: NVMe root behind PCIe, GICv3 with ITS for MSI-X,
    # ACPI enabled. Deliberately no virtio -- EC2 offers none.
    [ -w /dev/kvm ] || { echo "no /dev/kvm -- are you on a .metal instance?" >&2; exit 1; }
    exec qemu-system-aarch64 -M virt,gic-version=3,its=on,acpi=on,accel=kvm \
      -cpu host "${common[@]}" \
      -drive if=none,id=nvm,file="$IMAGE",format=raw \
      -device nvme,serial=haiku0,drive=nvm
    ;;
  ssh)
    # Headless: no -device ramfb, so app_server fails in a loop just like on a
    # Nitro instance -- which is the point: sshd must come up anyway. Connect
    # with: ssh -p 2222 -i ~/.ssh/haiku-graviton-ed25519 baron@127.0.0.1
    [ -w /dev/kvm ] || { echo "no /dev/kvm -- are you on a .metal instance?" >&2; exit 1; }
    exec qemu-system-aarch64 -M virt,gic-version=3,its=on,accel=kvm -cpu host \
      -m "$MEM" -smp "$SMP" -bios "$FW" -display none -serial mon:stdio -nographic \
      -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd \
      -drive if=none,id=hd,file="$IMAGE",format=raw \
      -netdev user,id=n0,hostfwd=tcp::2222-:22 \
      -device virtio-net-pci,netdev=n0,vectors=0
    ;;
  *) echo "usage: $0 {tcg|gicv3|kvm|nitro|ssh}" >&2; exit 2 ;;
esac
