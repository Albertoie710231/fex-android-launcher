#!/bin/bash
# Set up Vulkan inside the VM rootfs for GPU bridge testing
# Run with: sudo bash scripts/rootfs_setup_vulkan.sh
set -euo pipefail

ROOTFS_IMG="/home/alberto/vm_rootfs.img"
MOUNTPOINT="/tmp/vm_rootfs_mount"
PROJECT_DIR="/home/alberto/Documentos/MEDIATEK_DIRVERS_TEST"

mkdir -p "$MOUNTPOINT"
umount "$MOUNTPOINT" 2>/dev/null || true
mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"
cp /usr/bin/qemu-aarch64-static "$MOUNTPOINT/usr/bin/"

echo "=== Installing Vulkan tools ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    rm -f /etc/resolv.conf
    echo 'nameserver 8.8.8.8' > /etc/resolv.conf
    apt-get update 2>/dev/null
    apt-get install -y --no-install-recommends mesa-vulkan-drivers vulkan-tools libvulkan1 2>/dev/null || true
" 2>&1 | tail -5

echo "=== Copying Vortek ICD ==="
# Copy the ARM64 glibc Vortek client library
# This is the library that connects to VortekRenderer via socket
VORTEK_LIB="$PROJECT_DIR/app/src/main/jniLibs/arm64-v8a/libvulkan_vortek.so"
if [ -f "$VORTEK_LIB" ]; then
    cp "$VORTEK_LIB" "$MOUNTPOINT/usr/lib/aarch64-linux-gnu/libvulkan_vortek.so"
    chmod 755 "$MOUNTPOINT/usr/lib/aarch64-linux-gnu/libvulkan_vortek.so"
    echo "Copied libvulkan_vortek.so"
else
    echo "WARNING: libvulkan_vortek.so not found at $VORTEK_LIB"
fi

# Create Vortek ICD JSON
mkdir -p "$MOUNTPOINT/usr/share/vulkan/icd.d"
cat > "$MOUNTPOINT/usr/share/vulkan/icd.d/vortek_icd.json" << 'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/usr/lib/aarch64-linux-gnu/libvulkan_vortek.so",
        "api_version": "1.1.128"
    }
}
EOF
echo "Created vortek_icd.json"

# Create a test script
cat > "$MOUNTPOINT/usr/local/bin/test_gpu.sh" << 'SCRIPT'
#!/bin/bash
echo "=== GPU Bridge Test ==="

# Start network
ip link set eth0 up 2>/dev/null
ip addr add 10.0.2.15/24 dev eth0 2>/dev/null
ip route add default via 10.0.2.2 2>/dev/null

# Start Vortek VM bridge (Unix socket → TCP to host)
echo "Starting Vortek bridge..."
vortek_vm_bridge /tmp/vortek.sock 10.0.2.2 5900 &
BRIDGE_PID=$!
sleep 1

if [ -S /tmp/vortek.sock ]; then
    echo "Vortek bridge: OK (/tmp/vortek.sock)"
else
    echo "Vortek bridge: FAILED"
    exit 1
fi

# Set Vortek as the Vulkan ICD
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
export VORTEK_SERVER_PATH=/tmp/vortek.sock

echo "Running vulkaninfo..."
vulkaninfo --summary 2>&1 || echo "vulkaninfo failed (expected if Vortek protocol needs FEX thunks)"

echo ""
echo "=== Test complete ==="
kill $BRIDGE_PID 2>/dev/null
SCRIPT
chmod 755 "$MOUNTPOINT/usr/local/bin/test_gpu.sh"

# Clean up
rm -f "$MOUNTPOINT/usr/bin/qemu-aarch64-static"
umount "$MOUNTPOINT"

echo ""
echo "=== Done ==="
echo "Push rootfs and test:"
echo "  adb push $ROOTFS_IMG /data/local/tmp/vm_rootfs.img"
echo "  Then Boot VM and run: test_gpu.sh"
