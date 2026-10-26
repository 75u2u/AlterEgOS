#!/usr/bin/env bash
# Boot the images built by scripts/build.sh in QEMU, console on this terminal.
#
#   scripts/run-qemu.sh x86_64
#   scripts/run-qemu.sh aarch64
#
# Quit with Ctrl-A then X. The agent talks on the serial console; tty2 (the
# busybox escape shell) is not reachable in -nographic mode, which is fine for
# day 1. Pass QEMU_EXTRA="-display gtk" to get a VGA window with tty2.
set -euo pipefail

ARCH="${1:-x86_64}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$REPO/build/$ARCH/images"
MEM="${MEM:-1024}"

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
        echo "usage: $0 {x86_64|aarch64}" >&2; exit 2 ;;
esac
