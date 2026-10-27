#!/usr/bin/env bash
# Put a Claude API key on the USB/disk image so AlterEgo can talk after boot.
#
#   scripts/set-api-key.sh alteregos-x86_64.img sk-ant-...
#   scripts/set-api-key.sh alteregos-x86_64.img < keyfile
#
# Writes alterego/api_key into the image's first partition (the EFI System
# Partition) with mtools, so nothing needs to be mounted and no root is
# needed. Needs: sfdisk (util-linux) and mtools. The same file, written by
# hand onto the mounted USB stick, works too.
set -euo pipefail

IMG="${1:?usage: $0 image.img [api-key]}"
KEY="${2:-}"
if [ -z "$KEY" ]; then
    read -r KEY
fi
KEY="${KEY//[$'\r\n ']/}"
[ -n "$KEY" ] || { echo "empty key" >&2; exit 2; }
command -v mcopy >/dev/null || { echo "mtools not found (apt install mtools)" >&2; exit 2; }

# byte offset of the first partition
start=$(sfdisk -d "$IMG" | awk -F'start=' '/start=/{split($2,a,","); gsub(/ /,"",a[1]); print a[1]; exit}')
[ -n "$start" ] || { echo "no partition table in $IMG" >&2; exit 2; }
off=$((start * 512))

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
printf '%s\n' "$KEY" > "$tmp"
mmd -D s -i "$IMG@@$off" ::/alterego 2>/dev/null || true
mcopy -o -i "$IMG@@$off" "$tmp" ::/alterego/api_key
echo "wrote alterego/api_key into $IMG"
