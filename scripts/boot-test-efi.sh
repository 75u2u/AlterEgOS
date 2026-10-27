#!/usr/bin/env bash
# Boot alteregos-x86_64.img the way a real UEFI PC would (OVMF firmware,
# the image as a disk, no -kernel/-append) and check on the serial line that
# init came up, the desktop found the framebuffer and the agent started in
# its window. On this path nothing talks on the serial line itself, so the
# check reads the kernel-log lines the three programs write to /dev/kmsg.
#
#   scripts/boot-test-efi.sh [timeout-seconds]
set -euo pipefail

LIMIT="${1:-300}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$REPO/build/x86_64/images/alteregos-x86_64.img"
LOG="$REPO/build/x86_64/boot-efi.log"

OVMF="${OVMF:-}"
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/edk2/x64/OVMF.fd; do
    [ -n "$OVMF" ] && break
    [ -f "$f" ] && OVMF="$f"
done
if [ -z "$OVMF" ]; then
    echo "OVMF firmware not found (apt install ovmf, or set OVMF=/path/to/OVMF.fd)" >&2
    exit 2
fi

WANT_INIT='alteregos-init: AlterEgOS .* booted'
WANT_SCREEN='alterego-screen: up [0-9]*x[0-9]*'
WANT_AGENT='alterego: up, persona'

: > "$LOG"
qemu-system-x86_64 -machine q35 -cpu max -m 512 -smp 2 \
    -bios "$OVMF" \
    -drive file="$IMG",format=raw,if=virtio \
    -device virtio-rng-pci -vga std -no-reboot -nographic -serial mon:stdio \
    </dev/null >"$LOG" 2>&1 &
qemu=$!

ok=1
for ((i = 0; i < LIMIT; i++)); do
    if grep -q "$WANT_INIT" "$LOG" && grep -q "$WANT_SCREEN" "$LOG" && grep -q "$WANT_AGENT" "$LOG"; then
        ok=0
        break
    fi
    kill -0 "$qemu" 2>/dev/null || break
    sleep 1
done
kill "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true

echo "== last 40 lines of $LOG"
tail -n 40 "$LOG"
if [ "$ok" = 0 ]; then
    echo "== EFI boot test: OK, init, the screen and AlterEgo all reported in via kmsg"
else
    echo "== EFI boot test: FAILED (init: $(grep -c "$WANT_INIT" "$LOG"), screen: $(grep -c "$WANT_SCREEN" "$LOG"), agent: $(grep -c "$WANT_AGENT" "$LOG"))"
fi
exit "$ok"
