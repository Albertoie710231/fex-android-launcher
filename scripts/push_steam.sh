#!/bin/bash
# Push Steam client tar to device and fix symlinks for FEX rootfs
# Requires: /tmp/steam_symlinks.tar from bootstrap_steam.sh

set -e

TAR=/tmp/steam_symlinks.tar
PKG=com.mediatek.steamlauncher
ROOTFS="files/fex-rootfs/Ubuntu_22_04/home/user"

if [ ! -f "$TAR" ]; then
    echo "ERROR: $TAR not found. Run bootstrap_steam.sh first."
    exit 1
fi

echo "=== Removing old Steam install ==="
adb shell "run-as $PKG rm -rf $ROOTFS/.steam" 2>/dev/null || true

echo "=== Pushing tar to device (1.9GB) ==="
adb push "$TAR" /data/local/tmp/steam_symlinks.tar

echo "=== Extracting on device ==="
adb shell "run-as $PKG tar xf /data/local/tmp/steam_symlinks.tar -C $ROOTFS/"

echo "=== Fixing absolute symlinks to relative ==="
adb shell "run-as $PKG sh -c '
cd $ROOTFS/.steam

# Remove broken absolute symlinks and recreate as relative
rm -f steam root sdk32 sdk64 bin bin32 bin64

ln -s debian-installation steam
ln -s debian-installation root
ln -s debian-installation/linux32 sdk32
ln -s debian-installation/linux64 sdk64
ln -s debian-installation/ubuntu12_32 bin32
ln -s debian-installation/ubuntu12_64 bin64
ln -s bin32 bin

echo \"=== Fixed symlinks ===\"
ls -la
'"

echo "=== Verifying ==="
adb shell "run-as $PKG ls $ROOTFS/.steam/steam/ubuntu12_32/steam $ROOTFS/.steam/steam/ubuntu12_32/steamui.so 2>&1"

echo "=== Cleanup tmp ==="
adb shell "rm -f /data/local/tmp/steam_symlinks.tar"

echo "DONE"
