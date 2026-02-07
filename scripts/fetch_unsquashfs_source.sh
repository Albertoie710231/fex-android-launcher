#!/bin/bash
# Downloads squashfs-tools and xz-utils source for building unsquashfs via NDK
set -e

VENDOR_DIR="$(dirname "$0")/../app/src/main/cpp/vendor"
mkdir -p "$VENDOR_DIR"

echo "=== Downloading squashfs-tools source ==="
SQFS_VERSION="4.6.1"
SQFS_URL="https://github.com/plougher/squashfs-tools/archive/refs/tags/${SQFS_VERSION}.tar.gz"
cd "$VENDOR_DIR"

if [ ! -d "squashfs-tools" ]; then
    curl -L "$SQFS_URL" | tar xz
    mv "squashfs-tools-${SQFS_VERSION}/squashfs-tools" squashfs-tools
    rm -rf "squashfs-tools-${SQFS_VERSION}"
    echo "squashfs-tools source downloaded"
else
    echo "squashfs-tools source already exists"
fi

echo ""
echo "=== Downloading xz-utils source (for liblzma) ==="
XZ_VERSION="5.4.5"
XZ_URL="https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.gz"

if [ ! -d "xz-utils" ]; then
    curl -L "$XZ_URL" | tar xz
    mv "xz-${XZ_VERSION}" xz-utils
    echo "xz-utils source downloaded"
else
    echo "xz-utils source already exists"
fi

echo ""
echo "=== Done ==="
echo "Sources are in: $VENDOR_DIR"
echo "Now build the APK normally - NDK will compile unsquashfs automatically."
