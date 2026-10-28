#!/usr/bin/env bash
# Boot the images built by scripts/build.sh in QEMU. By default a window opens
# with the screen (the desktop: chat and shell windows, mouse, kana input),
# and this terminal carries the kernel log.
#
#   scripts/run-qemu.sh x86_64            # the desktop in a QEMU window
#   scripts/run-qemu.sh aarch64
#   scripts/run-qemu.sh x86_64 --efi      # the USB image through UEFI firmware,
#                                         # the way a real PC boots it (needs ovmf)
#   scripts/run-qemu.sh x86_64 --serial   # no window: the agent talks on the
#                                         # serial console in this terminal
#
# Also works standalone from an unpacked Release archive (the images are then
# looked up next to this script instead of under build/<arch>/images).
#
# Quit by closing the QEMU window, or with Ctrl-A then X on this terminal.
# With a window the kernel console is the screen, so init starts the desktop
# there. With --serial the agent talks on the serial console instead; --efi
# --serial runs the USB image headless, with the desktop on a VGA screen you
# cannot see. QEMU_DISPLAY picks the window backend (gtk, sdl, ...); without
# it the first one this QEMU supports is used.
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
GUI=1
shift || true
for opt in "$@"; do
    case "$opt" in
        --efi) EFI=1 ;;
        --gui) GUI=1 ;;
        --serial|--no-gui|--nographic) GUI=0 ;;
        *) echo "usage: $0 {x86_64|aarch64} [--efi] [--serial]" >&2; exit 2 ;;
    esac
done
QEMU="qemu-system-$ARCH"
command -v "$QEMU" >/dev/null || { echo "$QEMU not found (apt install qemu-system-x86 qemu-system-arm)" >&2; exit 2; }

# Which window backend this QEMU was built with: prefer gtk, then sdl.
pick_display() {
    local want have
    have="$("$QEMU" -display help 2>/dev/null || true)"
    for want in gtk sdl; do
        if printf '%s\n' "$have" | grep -qx "$want"; then echo "$want"; return; fi
    done
    echo "gtk"
}

# An image built before the screen existed boots the agent on the serial
# console and leaves the window black: say so instead of looking broken.
if [ "$GUI" = 1 ] && [ -f "$IMG/rootfs.cpio.gz" ] && command -v cpio >/dev/null; then
    if ! gzip -dc "$IMG/rootfs.cpio.gz" 2>/dev/null | cpio -t 2>/dev/null | grep -q 'usr/bin/alterego-screen$'; then
        echo "run-qemu.sh: $IMG/rootfs.cpio.gz has no screen program (built before src/screen existed?)." >&2
        echo "run-qemu.sh: the window will stay black; rebuild with scripts/build.sh $ARCH, or use --serial." >&2
    fi
fi

# Window mode: the screen, keyboard and an absolute-position mouse
# (virtio-tablet, so the pointer follows the host's without grabbing it);
# the serial console stays on this terminal for the kernel log.
if [ "$GUI" = 1 ]; then
    DISPLAY_ARGS=(-display "${QEMU_DISPLAY:-$(pick_display)}" -serial mon:stdio
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
    exec "$QEMU" \
        -machine q35,accel=kvm:tcg -cpu max -m "$MEM" -smp 2 \
        -bios "$OVMF" \
        -drive file="$IMG/alteregos-x86_64.img",format=raw,if=virtio \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -device virtio-rng-pci -vga std \
        "${DISPLAY_ARGS[@]}" ${QEMU_EXTRA:-}
fi

case "$ARCH" in
    x86_64)
        exec "$QEMU" \
            -machine q35,accel=kvm:tcg -cpu max -m "$MEM" -smp 2 \
            -kernel "$IMG/bzImage" -initrd "$IMG/rootfs.cpio.gz" \
            -append "$CONSOLE_X86 init=/sbin/init quiet" \
            -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
            -device virtio-rng-pci -vga std \
            "${DISPLAY_ARGS[@]}" ${QEMU_EXTRA:-}
        ;;
    aarch64)
        exec "$QEMU" \
            -machine virt -cpu cortex-a72 -m "$MEM" -smp 2 \
            -kernel "$IMG/Image" -initrd "$IMG/rootfs.cpio.gz" \
            -append "$CONSOLE_ARM init=/sbin/init quiet" \
            -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
            -device virtio-rng-pci -device virtio-gpu-pci \
            "${DISPLAY_ARGS[@]}" ${QEMU_EXTRA:-}
        ;;
    *)
        echo "usage: $0 {x86_64|aarch64} [--efi] [--serial]" >&2; exit 2 ;;
esac
