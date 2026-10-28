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

# Buildroot uses the host's cmake (>= 3.18) when there is one; without it, it
# bootstraps its own host-cmake 3.31.5, which is slow and can fail to link on some
# hosts (undefined reference to cmGlobalGhsMultiGenerator, seen on Kali). Warn early instead of failing 10 minutes in.
cmake_ok=
if command -v cmake >/dev/null 2>&1; then
    read -r cm_major cm_minor < <(cmake --version | sed -nE '1s/.* ([0-9]+)\.([0-9]+).*/\1 \2/p') || true
    if [ -n "${cm_major:-}" ] && { [ "$cm_major" -gt 3 ] || { [ "$cm_major" -eq 3 ] && [ "$cm_minor" -ge 18 ]; }; }; then
        cmake_ok=1
    fi
fi
if [ -z "$cmake_ok" ]; then
    echo "== warning: no cmake >= 3.18 on this host; Buildroot will build host-cmake itself." >&2
    echo "   Recommended: sudo apt install cmake  (then rm -rf $OUT/build/host-cmake-*)" >&2
fi

mkdir -p "$OUT"
if [ ! -f "$OUT/.config" ]; then
    echo "== configuring alteregos_${ARCH}_defconfig"
    make -C "$BR_DIR" O="$OUT" BR2_EXTERNAL="$REPO/buildroot" "alteregos_${ARCH}_defconfig"
fi

# Buildroot sets HOSTCC/HOSTCXX with := so the environment cannot override
# them; pass them on the make command line when given (HOSTCC=gcc-14
# HOSTCXX=g++-14 scripts/build.sh x86_64 builds with an older host compiler).
MAKE_ARGS=()
[ -n "${HOSTCC:-}" ] && MAKE_ARGS+=("HOSTCC=$HOSTCC")
[ -n "${HOSTCXX:-}" ] && MAKE_ARGS+=("HOSTCXX=$HOSTCXX")

echo "== make $TARGET ($ARCH)"
make -C "$BR_DIR" O="$OUT" BR2_EXTERNAL="$REPO/buildroot" ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"} "$TARGET"

if [ "$TARGET" = "all" ]; then
    echo "== images in $OUT/images:"
    ls -la "$OUT/images"
fi
