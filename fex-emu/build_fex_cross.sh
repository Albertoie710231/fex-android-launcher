#!/bin/bash
# Cross-compile FEX-Emu for ARM64 from x86_64 host using Clang
# This avoids QEMU emulation entirely (native compiler speed)
#
# Uses FEX's own toolchain_aarch64.cmake with an ARM64 sysroot

set -e

JOBS=${1:-$(nproc)}

cd "$(dirname "$0")"
mkdir -p out fex-src

# Clone FEX only if not already cloned
if [ ! -d "fex-src/FEX/.git" ]; then
    echo "=== Cloning FEX (first time only) ==="
    git clone --depth 1 --recurse-submodules https://github.com/FEX-Emu/FEX.git fex-src/FEX
else
    echo "=== FEX source already exists, skipping clone ==="
fi

echo "=== Cross-compiling FEX for ARM64 with $JOBS parallel jobs ==="

docker run --rm --platform linux/amd64 \
    -v "$(pwd)/fex-src:/src" \
    -v "$(pwd)/out:/out" \
    ubuntu:22.04 bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive

echo '=== Setting up ARM64 cross-compilation environment ==='

# Enable ARM64 multiarch
dpkg --add-architecture arm64

# Add ARM64 package sources
cat > /etc/apt/sources.list.d/arm64.list << 'SOURCES'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
SOURCES

# Pin the default sources to amd64 only
cat > /etc/apt/sources.list << 'SOURCES'
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-updates main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-security main restricted universe multiverse
SOURCES

apt-get update

# Install x86_64 build tools
apt-get install -y -q \
    git cmake ninja-build pkg-config \
    clang llvm lld llvm-dev \
    nasm python3

# Install ARM64 cross-compilation libraries
apt-get install -y -q \
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
    libc6-dev:arm64

echo '=== Configuring FEX (cross-compile ARM64) ==='
cd /src/FEX

# Always clean build for cross-compilation (cached toolchain config may differ)
rm -rf Build
mkdir -p Build && cd Build

# Fix libc.so linker script: it has absolute paths that break with sysroot.
# Replace absolute paths with relative ones that lld can resolve.
if [ -f /usr/aarch64-linux-gnu/lib/libc.so ]; then
    sed -i 's|/usr/aarch64-linux-gnu/lib/||g' /usr/aarch64-linux-gnu/lib/libc.so
fi

cmake \
    --toolchain ../Data/CMake/toolchain_aarch64_ubuntu.cmake \
    -DCMAKE_INSTALL_PREFIX=/opt/fex \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LTO=False \
    -DBUILD_TESTS=False \
    -DENABLE_ASSERTIONS=False \
    -DBUILD_THUNKS=False \
    -DBUILD_FEX_LINUX_TESTS=False \
    -DBUILD_FEXCONFIG=False \
    -G Ninja ..

echo '=== Building FEX with $JOBS parallel jobs ==='
ninja -j$JOBS

echo '=== Installing to /out ==='
rm -rf /out/*
DESTDIR=/out ninja install

# Fix permissions
chmod -R 777 /out
chmod -R 777 /src/FEX/Build

echo ''
echo '=== Cross-compilation complete! ==='
echo 'ARM64 binaries:'
ls -la /out/opt/fex/bin/
"

echo ""
echo "=== FEX ARM64 binaries are in: $(pwd)/out/opt/fex/ ==="
