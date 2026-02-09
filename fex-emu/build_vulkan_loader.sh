#!/bin/bash
# Cross-compile Khronos Vulkan-Loader for ARM64 (aarch64) targeting glibc 2.35
#
# The Ubuntu 24.04 Vulkan loader requires GLIBC_2.38 (__isoc23_sscanf, __isoc23_strtol),
# but our FEX host environment has glibc 2.35. This script builds the Vulkan loader
# in the same Ubuntu 22.04 Docker environment used for FEX cross-compilation.
#
# Output: out/vulkan-loader/libvulkan.so.1 (ARM64, glibc 2.35 compatible)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS=${1:-$(nproc)}

cd "$SCRIPT_DIR"

# Clone Vulkan-Loader if not present
if [ ! -d "vulkan-loader-src" ]; then
    echo "Cloning Vulkan-Loader..."
    git clone --depth 1 --branch v1.3.283 \
        https://github.com/KhronosGroup/Vulkan-Loader.git vulkan-loader-src
fi

# Clone Vulkan-Headers if not present (needed by loader build)
if [ ! -d "vulkan-headers-src" ]; then
    echo "Cloning Vulkan-Headers..."
    git clone --depth 1 --branch v1.3.283 \
        https://github.com/KhronosGroup/Vulkan-Headers.git vulkan-headers-src
fi

echo "=== Cross-compiling Vulkan-Loader for ARM64 ($JOBS jobs) ==="

docker run --rm --platform linux/amd64 \
    -v "$(pwd)/vulkan-loader-src:/loader-src" \
    -v "$(pwd)/vulkan-headers-src:/headers-src" \
    -v "$(pwd)/fex-src/FEX/Data/CMake:/toolchain" \
    -v "$(pwd)/out:/out" \
    ubuntu:22.04 bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive

echo '=== Setting up build environment ==='

# Enable ARM64 multiarch
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

# Build tools
apt-get install -y -qq \
    cmake ninja-build pkg-config git \
    clang llvm lld \
    python3 2>&1 | tail -1

# ARM64 cross-compilation + WSI dependencies
apt-get install -y -qq \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    libc6-dev:arm64 \
    libxcb1-dev:arm64 \
    libx11-dev:arm64 \
    libx11-xcb-dev:arm64 \
    libwayland-dev:arm64 \
    libxrandr-dev:arm64 2>&1 | tail -1

# Fix libc.so linker script
if [ -f /usr/aarch64-linux-gnu/lib/libc.so ]; then
    sed -i 's|/usr/aarch64-linux-gnu/lib/||g' /usr/aarch64-linux-gnu/lib/libc.so
fi

# Install Vulkan-Headers into a prefix so the loader can find them
echo ''
echo '=== Installing Vulkan-Headers ==='
cd /headers-src
mkdir -p build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local -G Ninja ..
ninja install

# Create header/lib symlinks in ARM64 sysroot for cross-compilation
mkdir -p /usr/aarch64-linux-gnu/include
for dir in X11 xcb wayland; do
    if [ -d /usr/include/\$dir ] && [ ! -e /usr/aarch64-linux-gnu/include/\$dir ]; then
        ln -s /usr/include/\$dir /usr/aarch64-linux-gnu/include/\$dir
    fi
done
for hdr in /usr/include/*.h; do
    basename=\$(basename \"\$hdr\")
    if [ ! -e /usr/aarch64-linux-gnu/include/\$basename ]; then
        ln -s \"\$hdr\" /usr/aarch64-linux-gnu/include/\$basename 2>/dev/null || true
    fi
done
for lib in /usr/lib/aarch64-linux-gnu/lib*.so*; do
    basename=\$(basename \"\$lib\")
    if [ ! -e /usr/aarch64-linux-gnu/lib/\$basename ]; then
        ln -s \"\$lib\" /usr/aarch64-linux-gnu/lib/\$basename 2>/dev/null || true
    fi
done
mkdir -p /usr/aarch64-linux-gnu/lib/pkgconfig
for pc in /usr/lib/aarch64-linux-gnu/pkgconfig/*.pc; do
    basename=\$(basename \"\$pc\")
    if [ ! -e /usr/aarch64-linux-gnu/lib/pkgconfig/\$basename ]; then
        ln -s \"\$pc\" /usr/aarch64-linux-gnu/lib/pkgconfig/\$basename 2>/dev/null || true
    fi
done

# Set pkg-config to find ARM64 packages
export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/aarch64-linux-gnu/lib/pkgconfig
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig

# Fix git ownership check in Docker
git config --global --add safe.directory /loader-src
git config --global --add safe.directory /headers-src

echo ''
echo '=== Cross-compiling Vulkan-Loader for ARM64 ==='
cd /loader-src
rm -rf build-arm64 && mkdir build-arm64 && cd build-arm64

cmake \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
    -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu \
    -DCMAKE_SYSROOT=/usr/aarch64-linux-gnu \
    -DCMAKE_C_FLAGS=\"--gcc-toolchain=/usr -march=armv8-a\" \
    -DCMAKE_CXX_FLAGS=\"--gcc-toolchain=/usr -march=armv8-a\" \
    -DCMAKE_EXE_LINKER_FLAGS=\"-fuse-ld=lld\" \
    -DCMAKE_SHARED_LINKER_FLAGS=\"-fuse-ld=lld\" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DBUILD_WSI_XCB_SUPPORT=OFF \
    -DBUILD_WSI_XLIB_SUPPORT=OFF \
    -DBUILD_WSI_WAYLAND_SUPPORT=OFF \
    -DBUILD_WSI_DIRECTFB_SUPPORT=OFF \
    -DUPDATE_DEPS=OFF \
    -DVULKAN_HEADERS_INSTALL_DIR=/usr/local \
    -G Ninja \
    ..

ninja -j$JOBS

echo ''
echo '=== Build output ==='
ls -la loader/libvulkan.so*

# Copy output
mkdir -p /out/vulkan-loader
cp loader/libvulkan.so.1* /out/vulkan-loader/
# Also create the soname symlink
cd /out/vulkan-loader
if [ -f libvulkan.so.1.3.283 ]; then
    ln -sf libvulkan.so.1.3.283 libvulkan.so.1
    ln -sf libvulkan.so.1 libvulkan.so
fi

echo ''
echo '=== Verifying glibc requirements ==='
readelf -V /out/vulkan-loader/libvulkan.so.1 | grep -i glibc || echo 'No GLIBC version requirements found'

echo ''
echo '=== Done ==='
ls -la /out/vulkan-loader/
"

echo ""
echo "=== Output ==="
ls -la "$SCRIPT_DIR/out/vulkan-loader/"
echo ""
echo "To deploy: copy out/vulkan-loader/libvulkan.so.1 to jniLibs as libvulkan_loader.so"
