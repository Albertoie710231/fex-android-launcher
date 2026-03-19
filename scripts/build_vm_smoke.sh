#!/bin/bash
# Build minimal kernel + initrd for AVF smoke test
#
# The kernel is a pre-built Android GKI (Generic Kernel Image) for ARM64.
# The initrd is a tiny cpio archive with a single init script.
#
# Output goes to app/src/main/assets/vm_kernel.gz and vm_initrd.gz

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ASSETS_DIR="$PROJECT_DIR/app/src/main/assets"
BUILD_DIR="$PROJECT_DIR/build/vm-smoke"

mkdir -p "$BUILD_DIR" "$ASSETS_DIR"

echo "=== Building AVF Smoke Test Assets ==="

# --- Kernel ---
# Option 1: Use a pre-built kernel from Android CI
# The cuttlefish kernel works well with AVF/crosvm
KERNEL_URL="https://android-build.googlesource.com"
KERNEL_FILE="$BUILD_DIR/Image"

if [ ! -f "$ASSETS_DIR/vm_kernel.gz" ]; then
    echo ""
    echo "=== KERNEL ==="
    echo "You need an ARM64 kernel compatible with crosvm/AVF."
    echo ""
    echo "Option A: Download from Android CI (recommended)"
    echo "  1. Go to: https://ci.android.com/"
    echo "  2. Search for branch: aosp-android-mainline-kernel"
    echo "  3. Find build target: kernel_aarch64"
    echo "  4. Download Image.gz"
    echo "  5. Copy to: $ASSETS_DIR/vm_kernel.gz"
    echo ""
    echo "Option B: Use the cuttlefish kernel from your SDK"
    echo "  Look for Image.gz in your Android SDK/emulator images"
    echo ""
    echo "Option C: Cross-compile a minimal kernel"
    echo "  git clone https://android.googlesource.com/kernel/common -b android-mainline"
    echo "  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig"
    echo "  # Enable: VIRTIO, VIRTIO_CONSOLE, VIRTIO_BLK, 9P_FS, VSOCK"
    echo "  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image.gz -j\$(nproc)"
    echo "  cp arch/arm64/boot/Image.gz $ASSETS_DIR/vm_kernel.gz"
    echo ""

    # Try to find a kernel from common locations
    for candidate in \
        "/usr/share/qemu-efi-aarch64/Image" \
        "$HOME/Android/Sdk/system-images/android-*/default/arm64-v8a/kernel-ranchu" \
        "/tmp/Image.gz" \
        "/tmp/Image"; do
        # Use globbing
        for f in $candidate; do
            if [ -f "$f" ]; then
                echo "Found candidate kernel: $f"
                if [[ "$f" == *.gz ]]; then
                    cp "$f" "$ASSETS_DIR/vm_kernel.gz"
                else
                    gzip -c "$f" > "$ASSETS_DIR/vm_kernel.gz"
                fi
                echo "Copied to $ASSETS_DIR/vm_kernel.gz"
                break 2
            fi
        done
    done

    if [ ! -f "$ASSETS_DIR/vm_kernel.gz" ]; then
        echo "WARNING: No kernel found. You must provide one manually."
        echo "Creating empty placeholder (app will show error at runtime)"
        touch "$ASSETS_DIR/vm_kernel.gz"
    fi
else
    echo "Kernel already exists: $ASSETS_DIR/vm_kernel.gz ($(stat -c%s "$ASSETS_DIR/vm_kernel.gz") bytes)"
fi

# --- Initrd ---
echo ""
echo "=== INITRD ==="

INITRD_DIR="$BUILD_DIR/initrd-root"
rm -rf "$INITRD_DIR"
mkdir -p "$INITRD_DIR"/{bin,proc,sys,dev,tmp}

# Create init script
cat > "$INITRD_DIR/init" << 'INIT_EOF'
#!/bin/sh
# AVF Smoke Test Init
mount -t proc proc /proc
mount -t sysfs sys /sys

echo "========================================="
echo "  AVF Smoke Test - VM Booted Successfully"
echo "========================================="
echo ""
echo "Kernel: $(uname -r)"
echo "Arch:   $(uname -m)"
echo ""

# The key test — can we set vm.max_map_count?
if [ -f /proc/sys/vm/max_map_count ]; then
    CURRENT=$(cat /proc/sys/vm/max_map_count)
    echo "vm.max_map_count (before): $CURRENT"

    echo 1048576 > /proc/sys/vm/max_map_count
    NEW=$(cat /proc/sys/vm/max_map_count)
    echo "vm.max_map_count (after):  $NEW"

    if [ "$NEW" = "1048576" ]; then
        echo ""
        echo "*** SUCCESS: vm.max_map_count set to 1048576 ***"
        echo "*** Webhelper will survive in this VM! ***"
    else
        echo ""
        echo "*** FAILED: Could not set vm.max_map_count ***"
    fi
else
    echo "WARNING: /proc/sys/vm/max_map_count not found"
fi

echo ""
echo "Memory: $(grep MemTotal /proc/meminfo 2>/dev/null || echo 'unknown')"
echo "CPUs:   $(nproc 2>/dev/null || grep -c processor /proc/cpuinfo 2>/dev/null || echo 'unknown')"
echo ""
echo "Smoke test complete. Shutting down."

# Give time for output to be read
sleep 2

# Power off
echo o > /proc/sysrq-trigger 2>/dev/null || poweroff -f 2>/dev/null || reboot -f 2>/dev/null
INIT_EOF
chmod 755 "$INITRD_DIR/init"

# We need a static busybox for the shell commands in init
# Check if one is available
BUSYBOX=""
for candidate in \
    /usr/bin/busybox \
    /bin/busybox \
    "$HOME/.local/bin/busybox-aarch64"; do
    if [ -f "$candidate" ]; then
        BUSYBOX="$candidate"
        break
    fi
done

if [ -n "$BUSYBOX" ]; then
    echo "Using busybox: $BUSYBOX"
    cp "$BUSYBOX" "$INITRD_DIR/bin/busybox"
    chmod 755 "$INITRD_DIR/bin/busybox"
    # Create symlinks for commands used in init
    for cmd in sh mount cat echo grep nproc sleep uname poweroff reboot; do
        ln -sf busybox "$INITRD_DIR/bin/$cmd"
    done
else
    echo "WARNING: No busybox found."
    echo "The init script needs /bin/sh. Without busybox, the VM may not boot."
    echo ""
    echo "Install busybox or download a static ARM64 binary:"
    echo "  wget https://busybox.net/downloads/binaries/1.35.0-aarch64-linux-musl/busybox -O $INITRD_DIR/bin/busybox"
    echo "  chmod +x $INITRD_DIR/bin/busybox"
fi

# Create initrd cpio archive
echo ""
echo "Creating initrd..."
cd "$INITRD_DIR"
find . | cpio -o -H newc 2>/dev/null | gzip > "$ASSETS_DIR/vm_initrd.gz"
echo "Initrd created: $ASSETS_DIR/vm_initrd.gz ($(stat -c%s "$ASSETS_DIR/vm_initrd.gz") bytes)"

echo ""
echo "=== Done ==="
echo "Assets in: $ASSETS_DIR/"
ls -la "$ASSETS_DIR/vm_kernel.gz" "$ASSETS_DIR/vm_initrd.gz" 2>/dev/null
echo ""
echo "Next: Build and deploy the app, then press 'Check VM' and 'Test VM' in Terminal."
