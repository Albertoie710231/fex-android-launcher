#!/bin/bash
# Build FEX-Emu with thunks enabled (Vulkan, GL, EGL, etc.) for ARM64
#
# Two-phase build in amd64 Docker:
#   Phase 1: Build thunkgen natively on amd64 (needs libclang + LLVM)
#   Phase 2: Cross-compile FEX + thunks for aarch64, using native thunkgen
#
# Thunks allow x86-64 guest code to call ARM64 host libraries directly,
# which is needed for Vulkan passthrough (Vortek) on Android.
#
# Output:
#   - FEX binaries (aarch64): out/opt/fex/bin/
#   - Host thunks (aarch64):  out/opt/fex/lib/fex-emu/HostThunks/
#   - Guest thunks (x86-64):  out/opt/fex/share/fex-emu/GuestThunks/

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
JOBS=${1:-$(nproc)}

cd "$SCRIPT_DIR"

if [ ! -d "fex-src/FEX/.git" ]; then
    echo "ERROR: FEX source not found at fex-src/FEX/"
    echo "Run build_fex_cross.sh first to clone the source."
    exit 1
fi

echo "=== Building FEX with thunks ($JOBS jobs) ==="
echo "This will take a while on first run (installing many dev packages)."
echo ""

docker run --rm --platform linux/amd64 \
    -v "$(pwd)/fex-src:/src" \
    -v "$(pwd)/out:/out" \
    ubuntu:22.04 bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive

echo '=== Setting up build environment ==='

# Enable ARM64 multiarch for cross-compilation
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

# Build tools (native amd64)
apt-get install -y -qq \
    git cmake ninja-build pkg-config \
    gcc g++ clang llvm lld \
    nasm python3 2>&1 | tail -1

# thunkgen dependencies (native amd64): libclang for AST parsing, LLVM, OpenSSL
apt-get install -y -qq \
    llvm-dev libclang-dev libssl-dev 2>&1 | tail -1

# ARM64 cross-compilation toolchain + libraries
apt-get install -y -qq \
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

# Host thunks need ARM64 OpenGL/Vulkan/Wayland/X11/XCB dev headers
apt-get install -y -qq \
    libgl-dev:arm64 \
    libegl-dev:arm64 \
    libwayland-dev:arm64 \
    libasound2-dev:arm64 \
    libx11-dev:arm64 \
    libx11-xcb-dev:arm64 \
    libxcb1-dev:arm64 \
    libxrandr-dev:arm64 2>&1 | tail -1

# Guest thunks need native x86-64 dev headers (x86-64 IS native on amd64 Docker)
apt-get install -y -qq \
    libgl-dev \
    libegl-dev \
    libwayland-dev \
    libasound2-dev \
    libdrm-dev 2>&1 | tail -1

# 32-bit guest thunks need full multilib support (32-bit CRT, libstdc++, headers)
apt-get install -y -qq \
    gcc-multilib g++-multilib 2>&1
echo '=== 32-bit multilib install status: '\$?' ==='

# Create i686 include symlinks for thunkgen's libclang (needs 32-bit C++ headers)
mkdir -p /usr/i686-linux-gnu/include
# 32-bit bits/c++config.h lives in arch-specific path
for cppdir in /usr/include/c++/11/i686-linux-gnu /usr/include/i386-linux-gnu/c++/11; do
    if [ -d \"\$cppdir\" ]; then
        echo \"Found 32-bit C++ headers: \$cppdir\"
    fi
done
# Symlink the i386 architecture-specific C++ headers
if [ -d /usr/include/i386-linux-gnu ]; then
    for dir in /usr/include/i386-linux-gnu/*/; do
        [ -d \"\$dir\" ] || continue
        basename=\$(basename \"\$dir\")
        mkdir -p /usr/i686-linux-gnu/include/\$basename
        for f in \"\$dir\"*; do
            [ -e \"\$f\" ] || continue
            fname=\$(basename \"\$f\")
            [ ! -e /usr/i686-linux-gnu/include/\$basename/\$fname ] && ln -s \"\$f\" /usr/i686-linux-gnu/include/\$basename/\$fname
        done
    done
fi
# Also symlink architecture-specific C++ include path
if [ -d /usr/include/x86_64-linux-gnu/c++/11/32 ]; then
    mkdir -p /usr/i686-linux-gnu/include/c++/11
    ln -sf /usr/include/x86_64-linux-gnu/c++/11/32/bits /usr/i686-linux-gnu/include/c++/11/bits
elif [ -d /usr/include/i386-linux-gnu/c++/11 ]; then
    mkdir -p /usr/i686-linux-gnu/include/c++
    ln -sf /usr/include/i386-linux-gnu/c++/11 /usr/i686-linux-gnu/include/c++/11
fi

# Create GCC 32-bit lib symlinks so gcc -m32 can find them
# libc6-dev-i386 puts CRT in /usr/lib32/, GCC 32-bit support files in .../11/32/
echo '=== Verifying 32-bit support ==='
ls /usr/lib32/Scrt1.o /usr/lib32/crti.o /usr/lib32/crtn.o 2>&1 || echo 'WARNING: 32-bit CRT files missing'
ls /usr/lib/gcc/x86_64-linux-gnu/11/32/ 2>&1 || echo 'WARNING: GCC 32-bit dir missing — installing manually'

# If GCC 32-bit support dir is missing, create it with needed files
if [ ! -d /usr/lib/gcc/x86_64-linux-gnu/11/32 ]; then
    mkdir -p /usr/lib/gcc/x86_64-linux-gnu/11/32
    # Copy the 64-bit CRT start files and create 32-bit versions
    # gcc -m32 will look here for crtbeginS.o etc.
fi

# Fix libc.so linker script for ARM64 cross-compilation
if [ -f /usr/aarch64-linux-gnu/lib/libc.so ]; then
    sed -i 's|/usr/aarch64-linux-gnu/lib/||g' /usr/aarch64-linux-gnu/lib/libc.so
fi

# Create header symlinks in ARM64 sysroot so cmake cross-compilation finds them.
# Architecture-independent headers (GL, EGL, wayland, etc.) live in /usr/include/
# but the cross-compile toolchain only looks in CMAKE_SYSROOT (/usr/aarch64-linux-gnu/).
mkdir -p /usr/aarch64-linux-gnu/include
# Symlink header directories
for dir in GL EGL KHR GLES GLES2 GLES3 wayland alsa drm libdrm vulkan X11 xcb; do
    if [ -d /usr/include/\$dir ] && [ ! -e /usr/aarch64-linux-gnu/include/\$dir ]; then
        ln -s /usr/include/\$dir /usr/aarch64-linux-gnu/include/\$dir
    fi
done
# Symlink individual header files (xf86drm.h, EGL/eglplatform.h, etc.)
for hdr in /usr/include/*.h; do
    basename=\$(basename \"\$hdr\")
    if [ ! -e /usr/aarch64-linux-gnu/include/\$basename ]; then
        ln -s \"\$hdr\" /usr/aarch64-linux-gnu/include/\$basename
    fi
done

# Create library symlinks in ARM64 sysroot so find_package(OpenGL), etc. work.
# ARM64 libs are installed at /usr/lib/aarch64-linux-gnu/ by multiarch packages,
# but cmake sysroot search looks in /usr/aarch64-linux-gnu/lib/.
for lib in /usr/lib/aarch64-linux-gnu/lib*.so*; do
    basename=\$(basename \"\$lib\")
    if [ ! -e /usr/aarch64-linux-gnu/lib/\$basename ]; then
        ln -s \"\$lib\" /usr/aarch64-linux-gnu/lib/\$basename
    fi
done
# Also symlink pkg-config files
mkdir -p /usr/aarch64-linux-gnu/lib/pkgconfig
for pc in /usr/lib/aarch64-linux-gnu/pkgconfig/*.pc; do
    basename=\$(basename \"\$pc\")
    if [ ! -e /usr/aarch64-linux-gnu/lib/pkgconfig/\$basename ]; then
        ln -s \"\$pc\" /usr/aarch64-linux-gnu/lib/pkgconfig/\$basename
    fi
done

# ============================================================
# Phase 1: Build thunkgen natively on amd64
# ============================================================
echo ''
echo '=== Phase 1: Building thunkgen natively on amd64 ==='

mkdir -p /tmp/thunkgen-build
cd /tmp/thunkgen-build

# Create a minimal CMakeLists.txt wrapper that builds only thunkgen
cat > CMakeLists.txt << 'WRAPPER_CMAKE'
cmake_minimum_required(VERSION 3.14)
project(thunkgen-standalone)

# thunkgen requires C++20 (std::optional, std::variant, starts_with, contains)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FEX bundles fmt library
set(FMT_INSTALL OFF)
add_subdirectory(/src/FEX/External/fmt fmt)

# Build thunkgen
add_subdirectory(/src/FEX/ThunkLibs/Generator generator)
WRAPPER_CMAKE

CC=clang CXX=clang++ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release . 2>&1
ninja thunkgen 2>&1

THUNKGEN_PATH=/tmp/thunkgen-build/generator/thunkgen
echo \"thunkgen built: \$(file \$THUNKGEN_PATH)\"

# ============================================================
# Phase 2: Cross-compile FEX + thunks for aarch64
# ============================================================
echo ''
echo '=== Phase 2: Cross-compiling FEX + thunks for aarch64 ==='

cd /src/FEX

# Clean previous build to pick up new cmake config with thunks
rm -rf Build
mkdir -p Build && cd Build

cmake -G Ninja \
    --toolchain ../Data/CMake/toolchain_aarch64_ubuntu.cmake \
    -DCMAKE_INSTALL_PREFIX=/opt/fex \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LTO=False \
    -DBUILD_TESTING=False \
    -DENABLE_ASSERTIONS=False \
    -DBUILD_FEX_LINUX_TESTS=False \
    -DBUILD_FEXCONFIG=False \
    -DBUILD_THUNKS=True \
    -DTHUNKGEN_PATH=\$THUNKGEN_PATH \
    -DENABLE_JEMALLOC=False \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
    -DENABLE_CLANG_THUNKS=True \
    -DSKIP_THUNKS_32=False \
    .. 2>&1

echo ''
echo '=== Building FEX + thunks ==='
ninja -j$JOBS 2>&1

echo ''
echo '=== Installing to /out ==='
rm -rf /out/*
DESTDIR=/out ninja install 2>&1 | tail -10

chmod -R 777 /out
chmod -R 777 /src/FEX/Build

echo ''
echo '=== Build complete ==='
echo ''
echo '--- FEX binaries (aarch64): ---'
ls -la /out/opt/fex/bin/ 2>/dev/null || echo '(none)'
echo ''
echo '--- Host thunks (aarch64): ---'
ls -la /out/opt/fex/lib/fex-emu/HostThunks/ 2>/dev/null || echo '(none)'
echo ''
echo '--- Guest thunks (x86-64): ---'
ls -la /out/opt/fex/share/fex-emu/GuestThunks/ 2>/dev/null || echo '(none)'
echo ''
echo '--- Guest thunks (x86 32-bit): ---'
ls -la /out/opt/fex/share/fex-emu/GuestThunks_32/ 2>/dev/null || echo '(none)'
echo ''
echo '--- Host thunks 32 (aarch64): ---'
ls -la /out/opt/fex/lib/fex-emu/HostThunks_32/ 2>/dev/null || echo '(none)'
echo ''
echo '--- Seccomp handler strings in FEX: ---'
strings /out/opt/fex/bin/FEX | grep -i seccomp || true
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

# Update FEX binaries
cp "$SCRIPT_DIR/out/opt/fex/bin/FEXServer" bin/FEXServer
cp "$SCRIPT_DIR/out/opt/fex/bin/FEX"       bin/FEX
rm -f bin/FEXLoader && ln -s FEX bin/FEXLoader

# Add host thunks (aarch64 .so files loaded by FEXServer at runtime)
if [ -d "$SCRIPT_DIR/out/opt/fex/lib/fex-emu/HostThunks" ]; then
    mkdir -p lib/fex-emu/HostThunks
    cp "$SCRIPT_DIR/out/opt/fex/lib/fex-emu/HostThunks/"*.so lib/fex-emu/HostThunks/ 2>/dev/null || true
    echo "  Host thunks:"
    ls lib/fex-emu/HostThunks/ 2>/dev/null || echo "    (none)"
fi

# Add 32-bit host thunks if they exist
if [ -d "$SCRIPT_DIR/out/opt/fex/lib/fex-emu/HostThunks_32" ]; then
    mkdir -p lib/fex-emu/HostThunks_32
    cp "$SCRIPT_DIR/out/opt/fex/lib/fex-emu/HostThunks_32/"*.so lib/fex-emu/HostThunks_32/ 2>/dev/null || true
fi

# Add guest thunks (x86-64 and x86-32) — deployed to rootfs by ContainerManager
if [ -d "$SCRIPT_DIR/out/opt/fex/share/fex-emu/GuestThunks" ]; then
    mkdir -p share/fex-emu/GuestThunks
    cp "$SCRIPT_DIR/out/opt/fex/share/fex-emu/GuestThunks/"*.so share/fex-emu/GuestThunks/ 2>/dev/null || true
    echo "  64-bit guest thunks:"
    ls share/fex-emu/GuestThunks/ 2>/dev/null || echo "    (none)"
fi

if [ -d "$SCRIPT_DIR/out/opt/fex/share/fex-emu/GuestThunks_32" ]; then
    mkdir -p share/fex-emu/GuestThunks_32
    cp "$SCRIPT_DIR/out/opt/fex/share/fex-emu/GuestThunks_32/"*.so share/fex-emu/GuestThunks_32/ 2>/dev/null || true
    echo "  32-bit guest thunks:"
    ls share/fex-emu/GuestThunks_32/ 2>/dev/null || echo "    (none)"
fi

tar czf "$ASSETS/fex-bin.tgz" bin/ lib/ share/
cd "$SCRIPT_DIR"
rm -rf "$TMPDIR"
echo "  fex-bin.tgz updated"

echo ""
echo "=== Guest thunks (auto-deployed by ContainerManager) ==="
echo "Guest thunks are now included in fex-bin.tgz and deployed automatically."
echo ""
echo "64-bit guest thunks:"
ls -la out/opt/fex/share/fex-emu/GuestThunks/ 2>/dev/null || echo "(none built)"
echo ""
echo "32-bit guest thunks:"
ls -la out/opt/fex/share/fex-emu/GuestThunks_32/ 2>/dev/null || echo "(none built)"

echo ""
echo "=== Done! ==="
echo "  cd $PROJECT_DIR"
echo "  ./gradlew assembleDebug"
echo "  adb install -r app/build/outputs/apk/debug/app-debug.apk"
