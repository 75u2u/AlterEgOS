#!/usr/bin/env bash
# Boot the images built by scripts/build.sh in QEMU, console on this terminal.
#
#   scripts/run-qemu.sh x86_64          # kernel + initramfs, serial console
#   scripts/run-qemu.sh aarch64
#   scripts/run-qemu.sh x86_64 --efi    # the USB image through UEFI firmware,
#                                       # the way a real PC boots it (needs ovmf)
#
# Also works standalone from an unpacked Release archive (the images are then
# looked up next to this script instead of under build/<arch>/images).
#
# Quit with Ctrl-A then X. In the plain mode the agent talks on the serial
# console. In --efi mode the agent talks on the VGA screen (tty1) like on a
# real PC, so pass QEMU_EXTRA="-display gtk" to see and type to it; the serial
# line then only carries the kernel log.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$HERE/rootfs.cpio.gz" ]; then
    # Unpacked Release archive: images sit next to this script, and the
    # architecture follows from which kernel is there.
    IMG="$HERE"
    DEFAULT_ARCH=x86_64; [ -f "$HERE/Image" ] && DEFAULT_ARCH=aarch64
    ARCH="${1:-$DEFAULT_ARCH}"
else
    ARCH="${1:-x86_64}"
    IMG="$HERE/../build/$ARCH/images"   # source tree after scripts/build.sh
fi
MODE="${2:-}"
MEM="${MEM:-1024}"

if [ "$MODE" = "--efi" ]; then
    [ "$ARCH" = "x86_64" ] || { echo "--efi is x86_64 only for now" >&2; exit 2; }
    OVMF="${OVMF:-}"
    for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/edk2/x64/OVMF.fd; do
        [ -n "$OVMF" ] && break
        [ -f "$f" ] && OVMF="$f"
    done
    [ -n "$OVMF" ] || { echo "OVMF not found (apt install ovmf)" >&2; exit 2; }
    exec qemu-system-x86_64 \
        -machine q35,accel=kvm:tcg -cpu max -m "$MEM" -smp 2 \
        -bios "$OVMF" \
        -drive file="$IMG/alteregos-x86_64.img",format=raw,if=virtio \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -device virtio-rng-pci \
        -nographic ${QEMU_EXTRA:-}
fi

case "$ARCH" in
    x86_64)
        exec qemu-system-x86_64 \
            -machine q35,accel=kvm:tcg -cpu max -m "$MEM" -smp 2 \
            -kernel "$IMG/bzImage" -initrd "$IMG/rootfs.cpio.gz" \
            -append "console=ttyS0 init=/sbin/init quiet" \
            -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
            -device virtio-rng-pci \
            -nographic ${QEMU_EXTRA:-}
        ;;
    aarch64)
        exec qemu-system-aarch64 \
            -machine virt -cpu cortex-a72 -m "$MEM" -smp 2 \
            -kernel "$IMG/Image" -initrd "$IMG/rootfs.cpio.gz" \
            -append "console=ttyAMA0 init=/sbin/init quiet" \
            -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
            -device virtio-rng-pci \
            -nographic ${QEMU_EXTRA:-}
        ;;
    *)
        echo "usage: $0 {x86_64|aarch64} [--efi]" >&2; exit 2 ;;
esac
