#!/usr/bin/env bash
# Boot the images built by scripts/build.sh in QEMU, console on this terminal.
#
#   scripts/run-qemu.sh x86_64          # kernel + initramfs, serial console
#   scripts/run-qemu.sh aarch64
#   scripts/run-qemu.sh x86_64 --gui    # with a screen: the desktop (chat and
#   scripts/run-qemu.sh aarch64 --gui   # shell windows, mouse) in a QEMU window
#   scripts/run-qemu.sh x86_64 --efi    # the USB image through UEFI firmware,
#                                       # the way a real PC boots it (needs ovmf);
#                                       # add --gui to see its screen too
#
# Also works standalone from an unpacked Release archive (the images are then
# looked up next to this script instead of under build/<arch>/images).
#
# Quit with Ctrl-A then X on the serial console (plain mode), or close the
# QEMU window (--gui). In the plain mode the agent talks on the serial
# console. With --gui the kernel console is the screen, so init starts the
# desktop there and the serial line only carries the kernel log. --efi
# without --gui runs headless: the desktop still starts on the VGA screen
# you cannot see, like on a real PC.
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
MEM="${MEM:-1024}"
EFI=0
GUI=0
shift || true
for opt in "$@"; do
    case "$opt" in
        --efi) EFI=1 ;;
        --gui) GUI=1 ;;
        *) echo "usage: $0 {x86_64|aarch64} [--efi] [--gui]" >&2; exit 2 ;;
    esac
done

# --gui: a window with the screen, keyboard and an absolute-position mouse
# (virtio-tablet, so the pointer follows the host's without grabbing it);
# the serial console stays on this terminal for the kernel log.
if [ "$GUI" = 1 ]; then
    DISPLAY_ARGS=(-display "${QEMU_DISPLAY:-gtk}" -serial mon:stdio
                  -device virtio-keyboard-pci -device virtio-tablet-pci)
    CONSOLE_X86="console=ttyS0 console=tty0"
    CONSOLE_ARM="console=ttyAMA0 console=tty0"
else
    DISPLAY_ARGS=(-nographic)
    CONSOLE_X86="console=ttyS0"
    CONSOLE_ARM="console=ttyAMA0"
fi

if [ "$EFI" = 1 ]; then
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
        -device virtio-rng-pci -vga std \
        "${DISPLAY_ARGS[@]}" ${QEMU_EXTRA:-}
fi

case "$ARCH" in
    x86_64)
        exec qemu-system-x86_64 \
            -machine q35,accel=kvm:tcg -cpu max -m "$MEM" -smp 2 \
            -kernel "$IMG/bzImage" -initrd "$IMG/rootfs.cpio.gz" \
            -append "$CONSOLE_X86 init=/sbin/init quiet" \
            -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
            -device virtio-rng-pci -vga std \
            "${DISPLAY_ARGS[@]}" ${QEMU_EXTRA:-}
        ;;
    aarch64)
        exec qemu-system-aarch64 \
            -machine virt -cpu cortex-a72 -m "$MEM" -smp 2 \
            -kernel "$IMG/Image" -initrd "$IMG/rootfs.cpio.gz" \
            -append "$CONSOLE_ARM init=/sbin/init quiet" \
            -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
            -device virtio-rng-pci -device virtio-gpu-pci \
            "${DISPLAY_ARGS[@]}" ${QEMU_EXTRA:-}
        ;;
    *)
        echo "usage: $0 {x86_64|aarch64} [--efi] [--gui]" >&2; exit 2 ;;
esac
