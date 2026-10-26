#!/usr/bin/env bash
# Build AlterEgOS for one architecture with Buildroot.
#
#   scripts/build.sh x86_64          # kernel + initramfs → build/x86_64/images/
#   scripts/build.sh aarch64
#   scripts/build.sh x86_64 menuconfig
#
# Buildroot is downloaded once into build/buildroot/. The first build takes
# 30–60 minutes (it compiles the toolchain and the kernel); later builds of
# src/ alone take seconds: `scripts/build.sh x86_64 alteregos-rebuild`.
set -euo pipefail

ARCH="${1:-x86_64}"
TARGET="${2:-all}"

case "$ARCH" in
    x86_64|aarch64) ;;
    *) echo "usage: $0 {x86_64|aarch64} [make-target]" >&2; exit 2 ;;
esac

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BR_VERSION="${BR_VERSION:-2025.02.3}"
BR_DIR="$REPO/build/buildroot"
OUT="$REPO/build/$ARCH"

if [ ! -f "$BR_DIR/Makefile" ]; then
    mkdir -p "$REPO/build"
    tarball="buildroot-$BR_VERSION.tar.xz"
    echo "== downloading Buildroot $BR_VERSION"
    curl -fL -o "$REPO/build/$tarball" "https://buildroot.org/downloads/$tarball"
    tar -C "$REPO/build" -xf "$REPO/build/$tarball"
    mv "$REPO/build/buildroot-$BR_VERSION" "$BR_DIR"
    rm -f "$REPO/build/$tarball"
fi

mkdir -p "$OUT"
if [ ! -f "$OUT/.config" ]; then
    echo "== configuring alteregos_${ARCH}_defconfig"
    make -C "$BR_DIR" O="$OUT" BR2_EXTERNAL="$REPO/buildroot" "alteregos_${ARCH}_defconfig"
fi

echo "== make $TARGET ($ARCH)"
make -C "$BR_DIR" O="$OUT" BR2_EXTERNAL="$REPO/buildroot" "$TARGET"

if [ "$TARGET" = "all" ]; then
    echo "== images in $OUT/images:"
    ls -la "$OUT/images"
fi
