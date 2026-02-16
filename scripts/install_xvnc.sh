#!/bin/bash
# Download and install Xvnc (TigerVNC) + all dependencies into the FEX rootfs on device.
# Xvnc is used instead of Xvfb because Xvfb segfaults under FEX-Emu.
# Run this on your host PC (not on the device).
#
# Usage: ./scripts/install_xvnc.sh

set -e

TMPDIR="/tmp/xvnc_install_$$"
TARBALL="$TMPDIR/xvnc_bundle.tar.gz"
EXTRACTED="$TMPDIR/extracted"

cleanup() {
    echo "Cleaning up $TMPDIR..."
    sudo rm -rf "$TMPDIR" 2>/dev/null || rm -rf "$TMPDIR" 2>/dev/null
}
trap cleanup EXIT

mkdir -p "$TMPDIR/debs" "$EXTRACTED"

echo "=== Step 1: Download Xvnc + all dependencies via Docker ==="
docker run --rm -v "$TMPDIR/debs:/output" ubuntu:22.04 bash -c '
apt-get update -qq 2>/dev/null
apt-get install --download-only -y tigervnc-standalone-server 2>/dev/null | tail -3
cp /var/cache/apt/archives/*.deb /output/ 2>/dev/null
echo ""
echo "Downloaded $(ls /output/*.deb 2>/dev/null | wc -l) packages"
'

echo ""
echo "=== Step 2: Extract all packages ==="
docker run --rm \
    -v "$TMPDIR/debs:/debs:ro" \
    -v "$EXTRACTED:/output" \
    ubuntu:22.04 bash -c '
for deb in /debs/*.deb; do
    dpkg-deb -x "$deb" /output/ 2>/dev/null
done

# CRITICAL: Ubuntu 22.04 uses merged-usr layout where /bin, /sbin, /lib, /lib64
# are symlinks to their usr/ counterparts. Package extraction can replace these
# symlinks with real directories, breaking the entire rootfs.
for dir in bin sbin lib lib64; do
    usrdir="usr/$dir"
    if [ -d "/output/$dir" ] && [ ! -L "/output/$dir" ]; then
        echo "WARNING: Removing /$dir directory to protect rootfs symlink"
        if [ "$(ls -A /output/$dir/ 2>/dev/null)" ]; then
            mkdir -p "/output/$usrdir"
            cp -a "/output/$dir"/* "/output/$usrdir/" 2>/dev/null || true
        fi
        rm -rf "/output/$dir"
    fi
done

echo "Xvnc binary: $(ls -la /output/usr/bin/Xvnc 2>/dev/null | awk "{print \$5}") bytes"
'

echo ""
echo "=== Step 3: Convert hard links to symlinks and create tarball ==="
# Android SELinux blocks hard links in app data dirs.
# Convert all hard links to symlinks so tar extraction works.
docker run --rm -v "$EXTRACTED:/output" ubuntu:22.04 bash -c '
cd /output
# Find files with multiple hard links and convert extras to symlinks
find . -type f -links +1 -printf "%i %p\n" | sort -n | \
    awk "{
        if (inode == \$1) {
            system(\"ln -sf \" first \" \" \$2)
        } else {
            inode = \$1
            first = \$2
        }
    }"
echo "Hard links converted to symlinks"
'
cd "$EXTRACTED"
tar czf "$TARBALL" .
echo "Tarball: $(du -h "$TARBALL" | cut -f1)"

echo ""
echo "=== Step 4: Push to device ==="
adb push "$TARBALL" /data/local/tmp/xvnc_bundle.tar.gz

echo ""
echo "=== Step 5: Extract into rootfs ==="
adb shell "run-as com.mediatek.steamlauncher tar xzf /data/local/tmp/xvnc_bundle.tar.gz -C files/fex-rootfs/Ubuntu_22_04/"

echo ""
echo "=== Step 6: Verify merged-usr symlinks are intact ==="
for pair in "bin:usr/bin" "sbin:usr/sbin" "lib:usr/lib" "lib64:usr/lib64"; do
    dir="${pair%%:*}"
    target="${pair##*:}"
    LINK=$(adb shell "run-as com.mediatek.steamlauncher ls -la files/fex-rootfs/Ubuntu_22_04/$dir" 2>/dev/null | head -1)
    if echo "$LINK" | grep -q "$target"; then
        echo "OK: /$dir -> $target"
    else
        echo "FIXING: /$dir symlink broken, restoring..."
        adb shell "run-as com.mediatek.steamlauncher sh -c 'cp -a files/fex-rootfs/Ubuntu_22_04/$dir/* files/fex-rootfs/Ubuntu_22_04/$target/ 2>/dev/null; rm -rf files/fex-rootfs/Ubuntu_22_04/$dir; ln -s $target files/fex-rootfs/Ubuntu_22_04/$dir'"
        echo "Fixed: /$dir -> $target"
    fi
done

echo ""
echo "=== Step 7: Verify Xvnc ==="
adb shell "run-as com.mediatek.steamlauncher ls -la files/fex-rootfs/Ubuntu_22_04/usr/bin/Xvnc"

echo ""
echo "=== Done! ==="
echo "Use 'Start X Server' button in the app, or run in FEX terminal:"
echo "  Xvnc :99 -geometry 1280x720 -depth 24 -SecurityTypes None -nolisten local -listen tcp &"
echo "  export DISPLAY=localhost:99"
