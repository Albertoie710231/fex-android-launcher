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
    libc6-dev:arm64 \
    libgl-dev:arm64 libegl-dev:arm64 \
    libwayland-dev:arm64 libasound2-dev:arm64 \
    libx11-dev:arm64 libx11-xcb-dev:arm64 \
    libxcb1-dev:arm64 libxrandr-dev:arm64 \
    libgl-dev libegl-dev libwayland-dev libasound2-dev libdrm-dev 2>&1 | tail -1

# Create header symlinks in ARM64 sysroot for thunks cross-compilation
mkdir -p /usr/aarch64-linux-gnu/include
for dir in GL EGL KHR GLES GLES2 GLES3 wayland alsa drm libdrm vulkan X11 xcb; do
    if [ -d /usr/include/\$dir ] && [ ! -e /usr/aarch64-linux-gnu/include/\$dir ]; then
        ln -s /usr/include/\$dir /usr/aarch64-linux-gnu/include/\$dir
    fi
done
for hdr in /usr/include/*.h; do
    basename=\$(basename \"\$hdr\")
    if [ ! -e /usr/aarch64-linux-gnu/include/\$basename ]; then
        ln -s \"\$hdr\" /usr/aarch64-linux-gnu/include/\$basename
    fi
done
for lib in /usr/lib/aarch64-linux-gnu/lib*.so*; do
    basename=\$(basename \"\$lib\")
    if [ ! -e /usr/aarch64-linux-gnu/lib/\$basename ]; then
        ln -s \"\$lib\" /usr/aarch64-linux-gnu/lib/\$basename
    fi
done
mkdir -p /usr/aarch64-linux-gnu/lib/pkgconfig
for pc in /usr/lib/aarch64-linux-gnu/pkgconfig/*.pc; do
    basename=\$(basename \"\$pc\")
    if [ ! -e /usr/aarch64-linux-gnu/lib/pkgconfig/\$basename ]; then
        ln -s \"\$pc\" /usr/aarch64-linux-gnu/lib/pkgconfig/\$basename
    fi
done

# Fix libc.so linker script
if [ -f /usr/aarch64-linux-gnu/lib/libc.so ]; then
    sed -i 's|/usr/aarch64-linux-gnu/lib/||g' /usr/aarch64-linux-gnu/lib/libc.so
fi

# Build thunkgen if Build depends on it (thunks-enabled build)
if grep -q thunkgen /src/FEX/Build/build.ninja 2>/dev/null; then
    echo '=== Building thunkgen (needed for thunks) ==='
    apt-get install -y -qq llvm-dev libclang-dev libssl-dev 2>&1 | tail -1
    if [ ! -f /tmp/thunkgen-build/generator/thunkgen ]; then
        mkdir -p /tmp/thunkgen-build
        cd /tmp/thunkgen-build
        cat > CMakeLists.txt << 'WRAPPER_CMAKE'
cmake_minimum_required(VERSION 3.14)
project(thunkgen-standalone)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(FMT_INSTALL OFF)
add_subdirectory(/src/FEX/External/fmt fmt)
add_subdirectory(/src/FEX/ThunkLibs/Generator generator)
WRAPPER_CMAKE
        cmake -G Ninja -DCMAKE_BUILD_TYPE=Release . 2>&1
        ninja thunkgen 2>&1
        echo \"thunkgen built: \$(file /tmp/thunkgen-build/generator/thunkgen)\"
    fi
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
# Update host thunks if present
if [ -d "$SCRIPT_DIR/out/opt/fex/lib/fex-emu/HostThunks" ]; then
    mkdir -p lib/fex-emu/HostThunks
    cp "$SCRIPT_DIR/out/opt/fex/lib/fex-emu/HostThunks/"*.so lib/fex-emu/HostThunks/ 2>/dev/null || true
fi
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
