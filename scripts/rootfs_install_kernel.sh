#!/bin/bash
# Install kernel + modules + initrd in the VM rootfs
# Run with: sudo bash scripts/rootfs_install_kernel.sh
set -euo pipefail

ROOTFS_IMG="/home/alberto/vm_rootfs.img"
MOUNTPOINT="/tmp/vm_rootfs_mount"

mkdir -p "$MOUNTPOINT"

# Unmount if already mounted
umount "$MOUNTPOINT" 2>/dev/null || true

echo "=== Resizing image to 4G ==="
umount "$MOUNTPOINT" 2>/dev/null || true
sleep 1
truncate -s 4G "$ROOTFS_IMG"
e2fsck -f -y "$ROOTFS_IMG" || true
resize2fs "$ROOTFS_IMG"
mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"
cp /usr/bin/qemu-aarch64-static "$MOUNTPOINT/usr/bin/"

echo "=== Fixing broken packages and installing kernel ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    dpkg --configure -a 2>/dev/null || true
    apt-get -f install -y 2>/dev/null || true
    # Remove broken linux-firmware if it failed
    dpkg --remove --force-remove-reinstreq linux-firmware 2>/dev/null || true
    # Install kernel without firmware dependency
    apt-get install -y --no-install-recommends linux-modules-5.15.0-173-generic linux-image-5.15.0-173-generic
"

echo "=== Checking installed kernel ==="
ls "$MOUNTPOINT"/boot/vmlinuz-* 2>/dev/null
ls "$MOUNTPOINT"/boot/initrd* 2>/dev/null
ls "$MOUNTPOINT"/lib/modules/ 2>/dev/null

# Copy kernel and initrd out for QEMU -kernel boot
KVER=\$(ls "$MOUNTPOINT/lib/modules/" | head -1)
if [ -n "\$KVER" ]; then
    cp "$MOUNTPOINT/boot/vmlinuz-\$KVER" /tmp/vm_kernel_ubuntu
    cp "$MOUNTPOINT/boot/initrd.img-\$KVER" /tmp/vm_initrd_ubuntu.gz
    echo ""
    echo "=== Kernel extracted ==="
    ls -lh /tmp/vm_kernel_ubuntu /tmp/vm_initrd_ubuntu.gz
fi

# Clean up
rm -f "$MOUNTPOINT/usr/bin/qemu-aarch64-static"
umount "$MOUNTPOINT"
rmdir "$MOUNTPOINT" 2>/dev/null || true

echo ""
echo "=== Done ==="
echo "Push to device:"
echo "  adb push /tmp/vm_kernel_ubuntu /data/local/tmp/vm_kernel"
echo "  adb push /tmp/vm_initrd_ubuntu.gz /data/local/tmp/vm_initrd.gz"
echo "  adb push /tmp/vm_rootfs.img /data/local/tmp/vm_rootfs.img"
