#!/bin/bash
# Install FEX-Emu inside the VM rootfs
# Run with: sudo bash scripts/rootfs_install_fex.sh
set -euo pipefail

ROOTFS_IMG="/home/alberto/vm_rootfs.img"
MOUNTPOINT="/tmp/vm_rootfs_mount"

mkdir -p "$MOUNTPOINT"
umount "$MOUNTPOINT" 2>/dev/null || true
mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"
cp /usr/bin/qemu-aarch64-static "$MOUNTPOINT/usr/bin/"

echo "=== Installing FEX-Emu from PPA ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    rm -f /etc/resolv.conf
    echo 'nameserver 8.8.8.8' > /etc/resolv.conf

    # Install prerequisites
    apt-get update 2>/dev/null
    apt-get install -y --no-install-recommends software-properties-common gpg-agent 2>/dev/null

    # Add FEX PPA
    add-apt-repository -y ppa:fex-emu/fex 2>/dev/null || true
    apt-get update 2>/dev/null

    # Install FEX
    apt-get install -y --no-install-recommends fex-emu-armv8.0 2>/dev/null || \
    apt-get install -y --no-install-recommends fex-emu 2>/dev/null || \
    echo 'FEX PPA install failed, will try manual install'
" 2>&1 | tail -10

# Check if FEX was installed
if chroot "$MOUNTPOINT" which FEXLoader 2>/dev/null; then
    echo "FEX installed via PPA"
else
    echo "FEX PPA not available for this arch/version, trying manual install..."
    # Download FEX release binary
    chroot "$MOUNTPOINT" /bin/bash -c "
        apt-get install -y --no-install-recommends curl squashfs-tools 2>/dev/null
        cd /tmp
        # Get latest FEX release
        curl -sL 'https://github.com/FEX-Emu/FEX/releases/download/FEX-2501/FEX-aarch64-2501.tar.gz' -o fex.tar.gz 2>/dev/null || \
        curl -sL 'https://github.com/FEX-Emu/FEX/releases/download/FEX-2601/FEX-aarch64-2601.tar.gz' -o fex.tar.gz 2>/dev/null
        if [ -f fex.tar.gz ]; then
            mkdir -p /opt/fex
            tar xzf fex.tar.gz -C /opt/fex
            ln -sf /opt/fex/bin/FEXLoader /usr/local/bin/FEXLoader
            ln -sf /opt/fex/bin/FEXInterpreter /usr/local/bin/FEXInterpreter
            rm -f fex.tar.gz
            echo 'FEX installed manually to /opt/fex'
        else
            echo 'ERROR: Could not download FEX'
        fi
    " 2>&1 | tail -5
fi

# Download x86-64 rootfs for FEX (SquashFS)
echo "=== Setting up x86-64 rootfs ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    mkdir -p /root/.fex-emu
    if [ ! -f /root/.fex-emu/Ubuntu_22_04.sqsh ]; then
        echo 'Downloading x86-64 rootfs (this is large, ~1GB)...'
        curl -sL 'https://rootfs.fex-emu.gg/Ubuntu_22_04/2025-01-08/Ubuntu_22_04.sqsh' -o /root/.fex-emu/Ubuntu_22_04.sqsh 2>/dev/null || \
        echo 'WARNING: Rootfs download failed. Push it manually to the VM.'
    fi
    ls -lh /root/.fex-emu/Ubuntu_22_04.sqsh 2>/dev/null || echo 'No rootfs yet'
" 2>&1 | tail -3

# Clean up
rm -f "$MOUNTPOINT/usr/bin/qemu-aarch64-static"
umount "$MOUNTPOINT"

echo ""
echo "=== Done ==="
echo "Push rootfs: adb push $ROOTFS_IMG /data/local/tmp/vm_rootfs.img"
