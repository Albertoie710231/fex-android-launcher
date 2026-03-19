#!/bin/bash
# Build a minimal ARM64 Ubuntu rootfs for QEMU VM on Android
# Uses debootstrap + qemu-user-static for cross-architecture bootstrap
set -euo pipefail

ROOTFS_IMG="/tmp/vm_rootfs.img"
ROOTFS_SIZE="2G"
MOUNTPOINT="/tmp/vm_rootfs_mount"
DISTRO="jammy"  # Ubuntu 22.04 (same as FEX rootfs)

echo "=== Building ARM64 Ubuntu $DISTRO rootfs ==="
echo "Image: $ROOTFS_IMG ($ROOTFS_SIZE)"

# Create sparse image
if [ ! -f "$ROOTFS_IMG" ]; then
    truncate -s "$ROOTFS_SIZE" "$ROOTFS_IMG"
    mkfs.ext4 -F -L vm-rootfs "$ROOTFS_IMG"
    echo "Created ext4 image"
else
    echo "Image already exists, reusing"
fi

# Mount
mkdir -p "$MOUNTPOINT"
if ! mountpoint -q "$MOUNTPOINT"; then
    sudo mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"
    echo "Mounted at $MOUNTPOINT"
else
    echo "Already mounted"
fi

# Debootstrap first stage
if [ ! -f "$MOUNTPOINT/debootstrap/debootstrap" ] && [ ! -f "$MOUNTPOINT/etc/hostname" ]; then
    echo "Running debootstrap (this takes a few minutes)..."
    sudo debootstrap --arch=arm64 --foreign --variant=minbase \
        --include=systemd-sysv,apt,wget,ca-certificates,iproute2,iputils-ping,nano,procps,sudo,bash \
        "$DISTRO" "$MOUNTPOINT" http://ports.ubuntu.com/ubuntu-ports/
    echo "First stage complete"
fi

# Copy qemu-aarch64-static for second stage
sudo cp /usr/bin/qemu-aarch64-static "$MOUNTPOINT/usr/bin/"

# Second stage
if [ -f "$MOUNTPOINT/debootstrap/debootstrap" ]; then
    echo "Running second stage..."
    sudo chroot "$MOUNTPOINT" /debootstrap/debootstrap --second-stage
    echo "Second stage complete"
fi

# Configure the system
echo "Configuring rootfs..."

# Set hostname
echo "fex-vm" | sudo tee "$MOUNTPOINT/etc/hostname" > /dev/null

# Set up apt sources
sudo tee "$MOUNTPOINT/etc/apt/sources.list" > /dev/null << EOF
deb http://ports.ubuntu.com/ubuntu-ports/ $DISTRO main restricted universe
deb http://ports.ubuntu.com/ubuntu-ports/ $DISTRO-updates main restricted universe
deb http://ports.ubuntu.com/ubuntu-ports/ $DISTRO-security main restricted universe
EOF

# Set up networking (DHCP on eth0)
sudo tee "$MOUNTPOINT/etc/systemd/network/80-dhcp.network" > /dev/null << EOF
[Match]
Name=en*

[Network]
DHCP=yes
EOF

# Enable systemd-networkd and systemd-resolved
sudo chroot "$MOUNTPOINT" systemctl enable systemd-networkd 2>/dev/null || true
sudo chroot "$MOUNTPOINT" systemctl enable systemd-resolved 2>/dev/null || true

# Set root password to 'root'
echo "root:root" | sudo chroot "$MOUNTPOINT" /usr/sbin/chpasswd

# Set up auto-login on serial console (ttyAMA0)
sudo mkdir -p "$MOUNTPOINT/etc/systemd/system/serial-getty@ttyAMA0.service.d"
sudo tee "$MOUNTPOINT/etc/systemd/system/serial-getty@ttyAMA0.service.d/autologin.conf" > /dev/null << EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I \$TERM
EOF

# Set vm.max_map_count on boot
sudo tee "$MOUNTPOINT/etc/sysctl.d/99-vm-max-map-count.conf" > /dev/null << EOF
vm.max_map_count = 1048576
EOF

# Set up DNS (resolv.conf)
sudo tee "$MOUNTPOINT/etc/resolv.conf" > /dev/null << EOF
nameserver 8.8.8.8
nameserver 8.8.4.4
EOF

# Disable unnecessary services for faster boot
sudo chroot "$MOUNTPOINT" systemctl mask systemd-timesyncd.service 2>/dev/null || true
sudo chroot "$MOUNTPOINT" systemctl mask e2scrub_reap.service 2>/dev/null || true

# Set locale
echo "LANG=C.UTF-8" | sudo tee "$MOUNTPOINT/etc/default/locale" > /dev/null

# Clean up
sudo rm -f "$MOUNTPOINT/usr/bin/qemu-aarch64-static"

# Unmount
sudo umount "$MOUNTPOINT"
rmdir "$MOUNTPOINT" 2>/dev/null || true

echo ""
echo "=== Rootfs ready ==="
ls -lh "$ROOTFS_IMG"
echo ""
echo "To test: push to device and boot with QEMU"
echo "  adb push $ROOTFS_IMG /data/local/tmp/vm_rootfs.img"
echo "  adb shell '/data/local/tmp/qemu-dynamic -machine virt -cpu max -m 1024 -nographic -nodefaults -serial stdio -drive file=/data/local/tmp/vm_rootfs.img,format=raw,if=virtio -kernel /data/local/tmp/vm_kernel -append \"console=ttyAMA0 root=/dev/vda rw\"'"
