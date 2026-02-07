#!/bin/bash
# Incremental rebuild of FEX-Emu for ARM64
# Uses existing Build directory — only recompiles changed files.
# Much faster than build_fex_cross.sh (which does a clean build).
#
# After build, copies binaries to jniLibs and updates fex-bin.tgz.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
JOBS=${1:-$(nproc)}

cd "$SCRIPT_DIR"

if [ ! -f "fex-src/FEX/Build/build.ninja" ]; then
    echo "ERROR: No existing Build directory found."
    echo "Run build_fex_cross.sh first for a full build."
    exit 1
fi

echo "=== Incremental cross-compile FEX for ARM64 ($JOBS jobs) ==="

docker run --rm --platform linux/amd64 \
    -v "$(pwd)/fex-src:/src" \
    -v "$(pwd)/out:/out" \
    ubuntu:22.04 bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive

dpkg --add-architecture arm64

cat > /etc/apt/sources.list.d/arm64.list << 'SOURCES'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
SOURCES

cat > /etc/apt/sources.list << 'SOURCES'
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-updates main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-security main restricted universe multiverse
SOURCES

apt-get update -qq
apt-get install -y -qq \
    git cmake ninja-build pkg-config \
    clang llvm lld llvm-dev \
    nasm python3 \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    libdrm-dev:arm64 \
    libxcb-present-dev:arm64 \
    libxcb-dri2-0-dev:arm64 \
    libxcb-dri3-dev:arm64 \
    libxcb-glx0-dev:arm64 \
    libxcb-shm0-dev:arm64 \
    libxshmfence-dev:arm64 \
    libsdl2-dev:arm64 \
    libepoxy-dev:arm64 \
    libssl-dev:arm64 \
    libc6-dev:arm64 2>&1 | tail -1

# Fix libc.so linker script
if [ -f /usr/aarch64-linux-gnu/lib/libc.so ]; then
    sed -i 's|/usr/aarch64-linux-gnu/lib/||g' /usr/aarch64-linux-gnu/lib/libc.so
fi

echo '=== Building (incremental) ==='
cd /src/FEX/Build
ninja -j$JOBS 2>&1

echo '=== Installing to /out ==='
rm -rf /out/*
DESTDIR=/out ninja install 2>&1 | tail -5

chmod -R 777 /out
chmod -R 777 /src/FEX/Build

echo ''
echo '=== Build complete ==='
ls -la /out/opt/fex/bin/FEXServer /out/opt/fex/bin/FEX
echo ''
echo '--- Seccomp handler strings in FEXServer: ---'
strings /out/opt/fex/bin/FEXServer | grep -i seccomp
echo '--- Seccomp handler strings in FEX: ---'
strings /out/opt/fex/bin/FEX | grep -i seccomp
"

echo ""
echo "=== Copying binaries to jniLibs ==="
JNILIBS="$PROJECT_DIR/app/src/main/jniLibs/arm64-v8a"
cp out/opt/fex/bin/FEXServer "$JNILIBS/libFEXServer.so"
cp out/opt/fex/bin/FEX       "$JNILIBS/libFEX.so"
cp out/opt/fex/bin/FEX       "$JNILIBS/libFEXInterpreter.so"
echo "  libFEXServer.so, libFEX.so, libFEXInterpreter.so updated"

echo ""
echo "=== Updating fex-bin.tgz ==="
ASSETS="$PROJECT_DIR/app/src/main/assets"
TMPDIR=$(mktemp -d)
cd "$TMPDIR"
tar xzf "$ASSETS/fex-bin.tgz"
cp "$SCRIPT_DIR/out/opt/fex/bin/FEXServer" bin/FEXServer
cp "$SCRIPT_DIR/out/opt/fex/bin/FEX"       bin/FEX
# FEXLoader is a symlink to FEX
rm -f bin/FEXLoader && ln -s FEX bin/FEXLoader
tar czf "$ASSETS/fex-bin.tgz" bin/ lib/ share/
cd "$SCRIPT_DIR"
rm -rf "$TMPDIR"
echo "  fex-bin.tgz updated"

echo ""
echo "=== Verify seccomp fix ==="
strings "$JNILIBS/libFEXServer.so" | grep "seccomp"

echo ""
echo "=== Done! Now run: ==="
echo "  cd $PROJECT_DIR"
echo "  ./gradlew assembleDebug"
echo "  adb install -r app/build/outputs/apk/debug/app-debug.apk"
