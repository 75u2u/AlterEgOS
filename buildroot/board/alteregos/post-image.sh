#!/bin/sh
# Buildroot post-image hook. $1 = BINARIES_DIR (images/). Runs from the
# Buildroot top directory, so support/scripts/genimage.sh is reachable.
#
# x86_64: build alteregos-x86_64.img, a GPT disk with one EFI System Partition
# whose EFI/BOOT/BOOTX64.EFI is the kernel (initramfs embedded). Written to a
# USB stick with dd it boots on any UEFI PC with Secure Boot off.
set -e

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

if [ -f "${BINARIES_DIR}/bzImage" ]; then
    rm -rf "${BINARIES_DIR}/efi-part"
    mkdir -p "${BINARIES_DIR}/efi-part/EFI/BOOT"
    cp "${BINARIES_DIR}/bzImage" "${BINARIES_DIR}/efi-part/EFI/BOOT/BOOTX64.EFI"
    support/scripts/genimage.sh -c "${BOARD_DIR}/genimage-efi.cfg"
    rm -f "${BINARIES_DIR}/efi-part.vfat"
fi
