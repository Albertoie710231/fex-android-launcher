#!/bin/bash
# Build x86-64 Ubuntu rootfs for QEMU VM — Steam runs natively here
# Run with: sudo bash scripts/build_x86_rootfs.sh
set -euo pipefail

ROOTFS_IMG="/home/alberto/vm_rootfs_x86.img"
ROOTFS_SIZE="64G"
MOUNTPOINT="/tmp/vm_x86_mount"
DISTRO="jammy"

echo "=== Building x86-64 Ubuntu $DISTRO rootfs ==="

# Create sparse image
if [ ! -f "$ROOTFS_IMG" ]; then
    truncate -s "$ROOTFS_SIZE" "$ROOTFS_IMG"
    mkfs.ext4 -F -L vm-x86-rootfs "$ROOTFS_IMG"
    echo "Created $ROOTFS_SIZE ext4 image"
else
    echo "Image exists, reusing"
fi

mkdir -p "$MOUNTPOINT"
umount "$MOUNTPOINT" 2>/dev/null || true
mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"

# Debootstrap — native x86-64, no qemu-user needed
if [ ! -f "$MOUNTPOINT/etc/hostname" ]; then
    echo "Running debootstrap (native x86-64, fast)..."
    debootstrap --arch=amd64 --variant=minbase \
        --include=systemd-sysv,apt,wget,curl,ca-certificates,iproute2,iputils-ping,nano,procps,sudo,bash,gnupg,software-properties-common \
        "$DISTRO" "$MOUNTPOINT" http://archive.ubuntu.com/ubuntu/
    echo "Debootstrap complete"
fi

echo "=== Configuring system ==="

# Hostname
echo "steam-vm" > "$MOUNTPOINT/etc/hostname"

# Apt sources
cat > "$MOUNTPOINT/etc/apt/sources.list" << EOF
deb http://archive.ubuntu.com/ubuntu/ $DISTRO main restricted universe multiverse
deb http://archive.ubuntu.com/ubuntu/ $DISTRO-updates main restricted universe multiverse
deb http://archive.ubuntu.com/ubuntu/ $DISTRO-security main restricted universe multiverse
EOF

# Network auto-config
cat > "$MOUNTPOINT/etc/rc.local" << 'EOF'
#!/bin/bash
ip link set eth0 up 2>/dev/null
ip addr add 10.0.2.15/24 dev eth0 2>/dev/null
ip route add default via 10.0.2.2 2>/dev/null
rm -f /etc/resolv.conf
echo "nameserver 8.8.8.8" > /etc/resolv.conf
echo "nameserver 8.8.4.4" >> /etc/resolv.conf
exit 0
EOF
chmod 755 "$MOUNTPOINT/etc/rc.local"

# Enable rc-local
cat > "$MOUNTPOINT/etc/systemd/system/rc-local.service" << 'EOF'
[Unit]
Description=RC Local
After=network.target

[Service]
Type=oneshot
ExecStart=/etc/rc.local
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
chroot "$MOUNTPOINT" systemctl enable rc-local.service 2>/dev/null || true

# Auto-login on serial console
mkdir -p "$MOUNTPOINT/etc/systemd/system/serial-getty@ttyS0.service.d"
cat > "$MOUNTPOINT/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" << 'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I $TERM
EOF

# vm.max_map_count
cat > "$MOUNTPOINT/etc/sysctl.d/99-vm-max-map-count.conf" << 'EOF'
vm.max_map_count = 1048576
EOF

# Disable initramfs updates (slow in VM)
echo "update_initramfs=no" > "$MOUNTPOINT/etc/initramfs-tools/update-initramfs.conf" 2>/dev/null || true

# Root password
echo "root:root" | chroot "$MOUNTPOINT" chpasswd

# Locale
echo "LANG=C.UTF-8" > "$MOUNTPOINT/etc/default/locale"

# DNS for chroot
rm -f "$MOUNTPOINT/etc/resolv.conf"
echo "nameserver 8.8.8.8" > "$MOUNTPOINT/etc/resolv.conf"

echo "=== Installing Steam dependencies ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    dpkg --add-architecture i386
    apt-get update
    apt-get install -y --no-install-recommends \
        libc6:i386 libstdc++6:i386 libgl1-mesa-glx:i386 \
        libx11-6 libx11-6:i386 libxext6 libxext6:i386 \
        libxrandr2 libxrandr2:i386 libxinerama1 libxinerama1:i386 \
        libxcursor1 libxcursor1:i386 libxi6 libxi6:i386 \
        libxtst6 libxtst6:i386 libpulse0 libpulse0:i386 \
        libnss3 libnss3:i386 libgdk-pixbuf2.0-0 \
        libgtk2.0-0 libgtk2.0-0:i386 \
        xdg-utils xterm zenity \
        libgl1-mesa-dri libgl1-mesa-dri:i386 \
        mesa-vulkan-drivers mesa-vulkan-drivers:i386 \
        libvulkan1 libvulkan1:i386 \
        xvfb dbus-x11 2>/dev/null
" 2>&1 | tail -10

echo "=== Installing Steam ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    cd /tmp
    wget -q https://cdn.fastly.steamstatic.com/client/installer/steam.deb
    dpkg -i steam.deb 2>/dev/null || apt-get -f install -y 2>/dev/null
    rm -f steam.deb
" 2>&1 | tail -5

# Install kernel + initrd for boot
echo "=== Installing kernel ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    apt-get install -y --no-install-recommends linux-image-generic 2>/dev/null || true
" 2>&1 | tail -5

# Clean up
umount "$MOUNTPOINT"

echo ""
echo "=== x86-64 rootfs ready ==="
ls -lh "$ROOTFS_IMG"
echo ""
echo "Push to device:"
echo "  adb push $ROOTFS_IMG /data/local/tmp/vm_rootfs_x86.img"
