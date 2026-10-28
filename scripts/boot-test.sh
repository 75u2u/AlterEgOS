#!/usr/bin/env bash
# Boot the built image in QEMU (software emulation is fine) and check that
# init came up and AlterEgo greeted on the serial console. Used by CI and by
# hand after scripts/build.sh.
#
#   scripts/boot-test.sh x86_64 [timeout-seconds]
#   scripts/boot-test.sh x86_64 [timeout-seconds] --gui
#
# --gui boots the way scripts/run-qemu.sh does by default: a VGA screen and
# console=tty0, so init must start the desktop; the screen program reports
# "up" on the kernel log, which the serial line carries. No window is opened
# (-display none), so it runs on a CI runner.
#
# Without --gui the agent talks on the serial line, so the test also talks
# back: it attaches a marked memory disk (the way run-qemu.sh does), says
# 「こんにちは」, expects the newborn's echo and the kernel-log line that the
# disk was formatted, then boots a second time and expects AlterEgo to
# remember the conversation (it greets 「……user？」 instead of 「はじめまして」).
#
# Exit 0 when the expected lines appear, 1 otherwise; the full console log is
# left in build/<arch>/boot.log (boot-gui.log for --gui, boot2.log for the
# second boot).
set -euo pipefail

ARCH="${1:-x86_64}"
LIMIT="${2:-300}"
GUI=0
[ "${3:-}" = "--gui" ] && GUI=1
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$REPO/build/$ARCH/images"
LOG="$REPO/build/$ARCH/boot.log"
[ "$GUI" = 1 ] && LOG="$REPO/build/$ARCH/boot-gui.log"

WANT_INIT='\[init\] AlterEgOS .* booted'
WANT_AGENT='AlterEgo: はじめまして'
LABEL="$ARCH"
if [ "$GUI" = 1 ]; then
    # init's own line goes to the screen; the serial line only carries kmsg
    LABEL="$ARCH, gui"
    WANT_INIT='alteregos-init: AlterEgOS .* booted'
    WANT_AGENT='alterego-screen: up [0-9]*x[0-9]*'
    CONSOLE_X86="console=ttyS0 console=tty0"
    CONSOLE_ARM="console=ttyAMA0 console=tty0"
else
    CONSOLE_X86="console=ttyS0"
    CONSOLE_ARM="console=ttyAMA0"
fi

case "$ARCH" in
    x86_64)
        cmd=(qemu-system-x86_64 -machine q35 -cpu max -m 512 -smp 2
             -kernel "$IMG/bzImage" -initrd "$IMG/rootfs.cpio.gz"
             -append "$CONSOLE_X86 init=/sbin/init panic=5")
        [ "$GUI" = 1 ] && cmd+=(-vga std)
        ;;
    aarch64)
        cmd=(qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -smp 2
             -kernel "$IMG/Image" -initrd "$IMG/rootfs.cpio.gz"
             -append "$CONSOLE_ARM init=/sbin/init panic=5")
        [ "$GUI" = 1 ] && cmd+=(-device virtio-gpu-pci)
        ;;
    *) echo "usage: $0 {x86_64|aarch64} [seconds] [--gui]" >&2; exit 2 ;;
esac
cmd+=(-device virtio-rng-pci -no-reboot -serial mon:stdio)
if [ "$GUI" = 1 ]; then
    cmd+=(-display none -device virtio-keyboard-pci -device virtio-tablet-pci)
else
    cmd+=(-nographic)
fi

# One boot: wait for $1 and $2 in the log; with $3, type it once $1 and $2
# have appeared and then wait for $4 as well. Sets ok=0 on success.
boot() {
    local want1="$1" want2="$2" say="${3:-}" want3="${4:-}" log="$5"
    : > "$log"
    local in
    in=$(mktemp -u)
    mkfifo "$in"
    "${cmd[@]}" <"$in" >"$log" 2>&1 &
    local qemu=$!
    exec 3>"$in"           # keep the writer open so QEMU's stdin stays alive
    rm -f "$in"
    ok=1
    local said=0
    for ((i = 0; i < LIMIT; i++)); do
        if grep -q "$want1" "$log" && grep -q "$want2" "$log"; then
            if [ -z "$say" ]; then ok=0; break; fi
            if [ "$said" = 0 ]; then sleep 2; printf '%s\n' "$say" >&3; said=1; fi
            if grep -q "$want3" "$log"; then ok=0; break; fi
        fi
        if ! kill -0 "$qemu" 2>/dev/null; then
            break   # QEMU exited (kernel panic + panic=5, or a bad image)
        fi
        sleep 1
    done
    exec 3>&-
    kill "$qemu" 2>/dev/null || true
    wait "$qemu" 2>/dev/null || true
    echo "== last 40 lines of $log"
    tail -n 40 "$log"
}

if [ "$GUI" = 1 ]; then
    boot "$WANT_INIT" "$WANT_AGENT" "" "" "$LOG"
else
    # a fresh marked memory disk, as scripts/run-qemu.sh makes it
    STATE="$REPO/build/$ARCH/boot-test-state.img"
    rm -f "$STATE"
    truncate -s 16M "$STATE"
    printf 'ALTEREGO-STATE\n' | dd of="$STATE" conv=notrunc status=none
    cmd+=(-drive file="$STATE",format=raw,if=virtio)
    boot "$WANT_INIT" "$WANT_AGENT" "こんにちは" 'AlterEgo: ……こんにちは？' "$LOG"
    if [ "$ok" = 0 ] && ! grep -q 'alterego: memory disk /dev/vd[a-z] formatted' "$LOG"; then
        echo "== boot test ($LABEL): the memory disk was not formatted"
        ok=1
    fi
    if [ "$ok" = 0 ]; then
        LABEL="$ARCH, second boot"
        boot "$WANT_INIT" 'AlterEgo: ……user？' "" "" "$REPO/build/$ARCH/boot2.log"
        [ "$ok" = 0 ] && LABEL="$ARCH, memory kept across a reboot"
    fi
fi

if [ "$ok" = 0 ]; then
    echo "== boot test ($LABEL): OK, init and AlterEgo both reported in"
else
    echo "== boot test ($LABEL): FAILED (see the log above)"
fi
exit "$ok"
