#!/bin/bash
# Configure networking in the VM rootfs
# Run with: sudo bash scripts/rootfs_configure_network.sh
set -euo pipefail

ROOTFS_IMG="/home/alberto/vm_rootfs.img"
MOUNTPOINT="/tmp/vm_rootfs_mount"

mkdir -p "$MOUNTPOINT"
umount "$MOUNTPOINT" 2>/dev/null || true
mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"
cp /usr/bin/qemu-aarch64-static "$MOUNTPOINT/usr/bin/"

echo "=== Configuring networking ==="

# Write a boot script that sets up networking before login
cat > "$MOUNTPOINT/etc/rc.local" << 'EOF'
#!/bin/bash
# Auto-configure network for QEMU slirp
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2

# Fix DNS - remove symlink and write directly
rm -f /etc/resolv.conf
echo "nameserver 8.8.8.8" > /etc/resolv.conf
echo "nameserver 8.8.4.4" >> /etc/resolv.conf

exit 0
EOF
chmod 755 "$MOUNTPOINT/etc/rc.local"

# Enable rc-local service
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

# Install dhclient and nsswitch dependencies
chroot "$MOUNTPOINT" /bin/bash -c "
    rm -f /etc/resolv.conf
    echo 'nameserver 8.8.8.8' > /etc/resolv.conf
    apt-get update
    apt-get install -y --no-install-recommends isc-dhcp-client libnss-resolve
" 2>&1 | tail -10

# Clean up
rm -f "$MOUNTPOINT/usr/bin/qemu-aarch64-static"
umount "$MOUNTPOINT"

echo ""
echo "=== Done ==="
echo "Push updated rootfs:"
echo "  adb push $ROOTFS_IMG /data/local/tmp/vm_rootfs.img"
