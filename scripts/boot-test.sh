#!/usr/bin/env bash
# Boot the built image in QEMU (software emulation is fine) and check that
# init came up and AlterEgo greeted on the serial console. Used by CI and by
# hand after scripts/build.sh.
#
#   scripts/boot-test.sh x86_64 [timeout-seconds]
#
# Exit 0 when both lines appear, 1 otherwise; the full console log is left in
# build/<arch>/boot.log.
set -euo pipefail

ARCH="${1:-x86_64}"
LIMIT="${2:-300}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$REPO/build/$ARCH/images"
LOG="$REPO/build/$ARCH/boot.log"

WANT_INIT='\[init\] AlterEgOS .* booted'
WANT_AGENT='AlterEgo: はじめまして'

case "$ARCH" in
    x86_64)
        cmd=(qemu-system-x86_64 -machine q35 -cpu max -m 512 -smp 2
             -kernel "$IMG/bzImage" -initrd "$IMG/rootfs.cpio.gz"
             -append "console=ttyS0 init=/sbin/init panic=5")
        ;;
    aarch64)
        cmd=(qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -smp 2
             -kernel "$IMG/Image" -initrd "$IMG/rootfs.cpio.gz"
             -append "console=ttyAMA0 init=/sbin/init panic=5")
        ;;
    *) echo "usage: $0 {x86_64|aarch64} [seconds]" >&2; exit 2 ;;
esac
cmd+=(-device virtio-rng-pci -no-reboot -nographic -serial mon:stdio)

: > "$LOG"
"${cmd[@]}" </dev/null >"$LOG" 2>&1 &
qemu=$!

ok=1
for ((i = 0; i < LIMIT; i++)); do
    if grep -q "$WANT_INIT" "$LOG" && grep -q "$WANT_AGENT" "$LOG"; then
        ok=0
        break
    fi
    if ! kill -0 "$qemu" 2>/dev/null; then
        break   # QEMU exited (kernel panic + panic=5, or a bad image)
    fi
    sleep 1
done
kill "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true

echo "== last 40 lines of $LOG"
tail -n 40 "$LOG"
if [ "$ok" = 0 ]; then
    echo "== boot test ($ARCH): OK, init and AlterEgo both reported in"
else
    echo "== boot test ($ARCH): FAILED (init: $(grep -c "$WANT_INIT" "$LOG"), agent: $(grep -c "$WANT_AGENT" "$LOG"))"
fi
exit "$ok"
