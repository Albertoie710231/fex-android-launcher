#!/bin/bash
# Package cross-compiled FEX binaries + ARM64 glibc libraries into fex-bin.tgz
# Must run after build_fex_cross.sh succeeds

set -e

cd "$(dirname "$0")"

if [ ! -f out/opt/fex/bin/FEX ]; then
    echo "ERROR: FEX binaries not found in out/. Run build_fex_cross.sh first."
    exit 1
fi

STAGE_DIR="$(pwd)/staging"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"/{bin,lib,lib/aarch64-linux-gnu,share/fex-emu}

echo "=== Copying FEX binaries ==="
cp out/opt/fex/bin/FEX            "$STAGE_DIR/bin/"
cp out/opt/fex/bin/FEXServer      "$STAGE_DIR/bin/"
cp out/opt/fex/bin/FEXInterpreter "$STAGE_DIR/bin/"
cp out/opt/fex/bin/FEXBash        "$STAGE_DIR/bin/"
cp out/opt/fex/bin/FEXGetConfig   "$STAGE_DIR/bin/"
cp out/opt/fex/bin/FEXRootFSFetcher "$STAGE_DIR/bin/"

# Compatibility symlink: FEXLoader → FEX (app references FEXLoader)
ln -s FEX "$STAGE_DIR/bin/FEXLoader"

# Copy libFEXCore.so if it exists
if [ -f out/opt/fex/lib/libFEXCore.so ]; then
    cp out/opt/fex/lib/libFEXCore.so "$STAGE_DIR/lib/"
fi

# Copy app config and thunks DB
if [ -d out/opt/fex/share/fex-emu ]; then
    cp -r out/opt/fex/share/fex-emu/* "$STAGE_DIR/share/fex-emu/"
fi

echo "=== Collecting ARM64 glibc libraries from cross-compilation sysroot ==="
# We need to grab the actual ARM64 libraries from the Docker environment
docker run --rm --platform linux/amd64 \
    -v "$STAGE_DIR:/stage" \
    ubuntu:22.04 bash -c "
set -e
export DEBIAN_FRONTEND=noninteractive
dpkg --add-architecture arm64
cat > /etc/apt/sources.list.d/arm64.list << 'SOURCES'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
SOURCES
cat > /etc/apt/sources.list << 'SOURCES'
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main restricted universe multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-updates main restricted universe multiverse
SOURCES
apt-get update
apt-get install -y -q libc6:arm64 libstdc++6:arm64 libgcc-s1:arm64

echo '=== Copying ARM64 libraries ==='
# Ubuntu multiarch: ARM64 libraries at /usr/lib/aarch64-linux-gnu/
A64=/usr/lib/aarch64-linux-gnu

# Dynamic linker
cp \$A64/ld-linux-aarch64.so.1 /stage/lib/ld-linux-aarch64.so.1
cp \$A64/ld-linux-aarch64.so.1 /stage/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1

# libc
cp \$A64/libc.so.6 /stage/lib/libc.so.6
cp \$A64/libc.so.6 /stage/lib/aarch64-linux-gnu/libc.so.6

# libm
cp \$A64/libm.so.6 /stage/lib/libm.so.6
cp \$A64/libm.so.6 /stage/lib/aarch64-linux-gnu/libm.so.6

# libstdc++
STDCPP=\$(find \$A64 -name 'libstdc++.so.6*' -type f | head -1)
cp \"\$STDCPP\" /stage/lib/libstdc++.so.6
cp \"\$STDCPP\" /stage/lib/aarch64-linux-gnu/libstdc++.so.6

# libgcc_s
cp \$A64/libgcc_s.so.1 /stage/lib/libgcc_s.so.1
cp \$A64/libgcc_s.so.1 /stage/lib/aarch64-linux-gnu/libgcc_s.so.1

# libpthread, libdl, librt (symlinks to libc on glibc 2.34+, copy if they exist)
for lib in libpthread.so.0 libdl.so.2 librt.so.1; do
    if [ -f \$A64/\$lib ]; then
        cp \$A64/\$lib /stage/lib/\$lib
        cp \$A64/\$lib /stage/lib/aarch64-linux-gnu/\$lib
    fi
done

chmod -R 755 /stage/
echo 'Done collecting libraries'
ls -la /stage/lib/
ls -la /stage/lib/aarch64-linux-gnu/
"

echo "=== Applying patchelf to FEX binaries ==="
# Change PT_INTERP from /lib/ld-linux-aarch64.so.1 to the device path
DEVICE_INTERP="/data/data/com.mediatek.steamlauncher/files/fex/lib/ld-linux-aarch64.so.1"

# Check if patchelf is available
if command -v patchelf &>/dev/null; then
    for bin in FEX FEXServer FEXInterpreter FEXBash FEXGetConfig FEXRootFSFetcher; do
        if [ -f "$STAGE_DIR/bin/$bin" ]; then
            echo "  Patching $bin"
            patchelf --set-interpreter "$DEVICE_INTERP" "$STAGE_DIR/bin/$bin" 2>/dev/null || true
        fi
    done
    echo "PT_INTERP patched to: $DEVICE_INTERP"
else
    echo "WARNING: patchelf not found! Binaries will need to be patched on device."
    echo "Install with: sudo pacman -S patchelf (Arch) or sudo apt install patchelf (Debian)"
fi

echo "=== Creating fex-bin.tgz ==="
cd "$STAGE_DIR"
tar czf ../fex-bin-new.tgz bin/ lib/ share/

echo ""
echo "=== Package created: $(pwd)/../fex-bin-new.tgz ==="
ls -lh ../fex-bin-new.tgz
echo ""
echo "Contents:"
tar tzf ../fex-bin-new.tgz

# Clean up
cd ..
rm -rf "$STAGE_DIR"
