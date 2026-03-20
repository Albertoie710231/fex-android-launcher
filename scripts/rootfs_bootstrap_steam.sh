#!/bin/bash
# Bootstrap Steam inside the x86-64 rootfs on the host PC
# Run with: sudo bash scripts/rootfs_bootstrap_steam.sh
set -euo pipefail

ROOTFS_IMG="/home/alberto/vm_rootfs_x86.img"
MOUNTPOINT="/tmp/vm_x86_mount"

export PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH

mkdir -p "$MOUNTPOINT"
umount "$MOUNTPOINT" 2>/dev/null || true
mount -o loop "$ROOTFS_IMG" "$MOUNTPOINT"

# Mount required filesystems for chroot
mount -t proc proc "$MOUNTPOINT/proc"
mount -t sysfs sys "$MOUNTPOINT/sys"
mount --bind /dev "$MOUNTPOINT/dev"
mount --bind /dev/pts "$MOUNTPOINT/dev/pts"
mount --bind /tmp "$MOUNTPOINT/tmp"

# DNS
rm -f "$MOUNTPOINT/etc/resolv.conf"
echo "nameserver 8.8.8.8" > "$MOUNTPOINT/etc/resolv.conf"

echo "=== Creating steam user ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
    id steam 2>/dev/null || useradd -m -s /bin/bash steam
    echo 'steam ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/steam
"

echo "=== Starting Xvfb ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
    Xvfb :99 -screen 0 1280x720x24 &
    sleep 2
    echo 'Xvfb running'
"

echo "=== Bootstrapping Steam ==="
chroot "$MOUNTPOINT" /bin/bash -c "
    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
    export DISPLAY=:99
    su steam -c 'DISPLAY=:99 steam -no-browser' &
    STEAM_PID=\$!
    echo 'Steam PID: '\$STEAM_PID
    echo 'Waiting for Steam to bootstrap (up to 3 minutes)...'
    for i in \$(seq 1 36); do
        sleep 5
        if [ -f /home/steam/.local/share/Steam/steam.sh ]; then
            echo 'Steam bootstrap COMPLETE!'
            break
        fi
        echo \"Waiting... (\${i}0s)\"
        ls /home/steam/.local/share/Steam/ 2>/dev/null | head -3
    done
    kill \$STEAM_PID 2>/dev/null
    killall steam Xvfb 2>/dev/null
"

echo "=== Steam files ==="
ls -lh "$MOUNTPOINT/home/steam/.local/share/Steam/" 2>/dev/null | head -15

# Cleanup mounts
umount "$MOUNTPOINT/dev/pts" 2>/dev/null
umount "$MOUNTPOINT/dev" 2>/dev/null
umount "$MOUNTPOINT/proc" 2>/dev/null
umount "$MOUNTPOINT/sys" 2>/dev/null
umount "$MOUNTPOINT/tmp" 2>/dev/null
umount "$MOUNTPOINT"

echo ""
echo "=== Done ==="
echo "Push: adb push $ROOTFS_IMG /data/local/tmp/vm_rootfs_x86.img"
