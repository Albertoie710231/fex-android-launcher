#!/bin/bash
# Build FEX-Emu for ARM64 using Docker + QEMU emulation
# Source and build are persistent in ./fex-src/ and ./out/
#
# QEMU ARM64 emulation causes random compiler segfaults.
# The build uses ninja retry loops to work around this.
# Use -j1 if segfaults are too frequent with higher parallelism.

set -e

# Number of parallel jobs (reduce if it crashes)
JOBS=${1:-4}
# Max retries for transient QEMU segfaults
MAX_RETRIES=${2:-20}

cd "$(dirname "$0")"
mkdir -p out fex-src

# Clone FEX only if not already cloned
if [ ! -d "fex-src/FEX/.git" ]; then
    echo "=== Cloning FEX (first time only) ==="
    git clone --depth 1 --recurse-submodules https://github.com/FEX-Emu/FEX.git fex-src/FEX
else
    echo "=== FEX source already exists, skipping clone ==="
fi

# Don't clean build dir - ninja handles incremental builds
# This is critical for surviving QEMU segfaults (retry from where we stopped)

echo "=== Building FEX for ARM64 with $JOBS parallel jobs (max $MAX_RETRIES retries) ==="

docker run --rm --platform linux/arm64 \
    -v "$(pwd)/fex-src:/src" \
    -v "$(pwd)/out:/out" \
    ubuntu:22.04 bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive

echo '=== Installing dependencies ==='
echo 'Acquire::Queue-Mode \"host\";' > /etc/apt/apt.conf.d/99parallel
apt-get update
apt-get install -y -q git cmake ninja-build pkg-config clang llvm lld \
    libdrm-dev libxcb-present-dev libxcb-dri2-0-dev libxcb-dri3-dev \
    libxcb-glx0-dev libxcb-shm0-dev libxshmfence-dev libclang-dev \
    libsdl2-dev libepoxy-dev libssl-dev squashfs-tools g++ \
    qtbase5-dev qtdeclarative5-dev nasm

cd /src/FEX

# Only run cmake if Build dir doesn't have build.ninja
if [ ! -f Build/build.ninja ]; then
    echo '=== Configuring FEX ==='
    mkdir -p Build && cd Build

    # Use GCC - both gcc and clang segfault under QEMU, but gcc less frequently
    # FEX's GCC rejection is patched out in CMakeLists.txt
    CC=gcc CXX=g++ cmake \
        -DCMAKE_INSTALL_PREFIX=/opt/fex \
        -DCMAKE_BUILD_TYPE=Release \
        -DUSE_LINKER=OFF \
        -DENABLE_LTO=False \
        -DBUILD_TESTS=False \
        -DENABLE_ASSERTIONS=False \
        -DBUILD_THUNKS=False \
        -DBUILD_FEX_LINUX_TESTS=False \
        -G Ninja ..
else
    echo '=== Build already configured, resuming ==='
    cd Build
fi

echo '=== Building FEX with $JOBS parallel jobs ==='

# Retry loop: QEMU ARM64 emulation causes random segfaults in the compiler.
# Ninja tracks what's already built, so each retry picks up where we left off.
for attempt in \$(seq 1 $MAX_RETRIES); do
    echo \"--- Build attempt \$attempt/$MAX_RETRIES ---\"
    if ninja -j$JOBS 2>&1; then
        echo '=== Build succeeded! ==='
        break
    else
        if [ \$attempt -eq $MAX_RETRIES ]; then
            echo '=== Build failed after $MAX_RETRIES attempts ==='
            exit 1
        fi
        echo \"--- Compiler crashed (QEMU segfault), retrying... ---\"
        sleep 1
    fi
done

echo '=== Installing to /out ==='
rm -rf /out/*
DESTDIR=/out ninja install

# Fix permissions
chmod -R 777 /out
chmod -R 777 /src/FEX/Build

echo ''
echo '=== Build complete! ==='
ls -la /out/opt/fex/bin/
"

echo ""
echo "=== FEX binaries are in: $(pwd)/out/opt/fex/ ==="
