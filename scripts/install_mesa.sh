#!/bin/bash
# Install Mesa swrast i386 (software GL) and ALL dependencies into FEX rootfs
# Steam binary is 32-bit i386, needs i386 Mesa
# Breaks all hardlinks so Android doesn't choke on them

set -e

echo "=== Building i386 Mesa deps package in Docker ==="

docker run --rm -v /tmp:/output ubuntu:22.04 bash -c '
dpkg --add-architecture i386
apt-get update -qq
apt-get install -y -qq libgl1-mesa-dri:i386 libgl1-mesa-glx:i386 2>/dev/null | tail -1

ARCH=i386-linux-gnu
STAGING=/staging
mkdir -p $STAGING/usr/lib/$ARCH/dri

# Copy swrast driver
cp /usr/lib/$ARCH/dri/swrast_dri.so $STAGING/usr/lib/$ARCH/dri/

# Collect all shared lib deps recursively
collect_deps() {
    ldd "$1" 2>/dev/null | grep "=> /" | awk "{print \$3}" | sort -u
}

ALL_DEPS=$(collect_deps /usr/lib/$ARCH/dri/swrast_dri.so)

# Also get deps of libGLX_mesa
MESA_GLX=$(find /usr/lib/$ARCH -name "libGLX_mesa.so*" -type f 2>/dev/null | head -1)
if [ -n "$MESA_GLX" ]; then
    ALL_DEPS="$ALL_DEPS
$(collect_deps $MESA_GLX)"
fi

# Copy each dep as a regular file (breaks hardlinks)
# Resolve /lib -> /usr/lib symlink so files go to the right place
for lib in $(echo "$ALL_DEPS" | sort -u); do
    if [ -f "$lib" ]; then
        realfile=$(readlink -f "$lib")
        dest="$STAGING$realfile"
        mkdir -p "$(dirname $dest)"
        cp "$realfile" "$dest" 2>/dev/null || true
        # Also copy under the soname if different
        basename_lib=$(basename "$lib")
        basename_real=$(basename "$realfile")
        if [ "$basename_lib" != "$basename_real" ]; then
            cp "$realfile" "$STAGING$(dirname $realfile)/$basename_lib" 2>/dev/null || true
        fi
    fi
done

# Also copy libGLX_mesa and libglapi explicitly
for f in /usr/lib/$ARCH/libGLX_mesa.so* /usr/lib/$ARCH/libglapi.so*; do
    [ -f "$f" ] && cp "$(readlink -f $f)" "$STAGING/usr/lib/$ARCH/$(basename $f)" 2>/dev/null || true
done

echo "=== Files collected ==="
find $STAGING -type f | wc -l
du -sh $STAGING

cd $STAGING && tar cf /output/mesa_full.tar usr/
ls -lh /output/mesa_full.tar
echo "DONE"
'

echo ""
echo "=== Pushing to device ==="
adb push /tmp/mesa_full.tar /data/local/tmp/

echo "=== Extracting on device ==="
adb shell "run-as com.mediatek.steamlauncher tar xf /data/local/tmp/mesa_full.tar -C files/fex-rootfs/Ubuntu_22_04/"

echo "=== Cleanup ==="
adb shell "rm /data/local/tmp/mesa_full.tar"

echo "=== Verify ==="
adb shell "run-as com.mediatek.steamlauncher ls -lh files/fex-rootfs/Ubuntu_22_04/usr/lib/i386-linux-gnu/dri/swrast_dri.so 2>&1"

echo "DONE"
