#!/usr/bin/env bash
# Cross-compile wrapper_testbench, push to device, run inside the app's
# data dir via run-as so the bionic loader can resolve the wrapper's
# transitive deps (libandroid-sysvshm, libxcb*, libdrm, libadrenotools,
# libc++_shared) the same way games do — from imagefs_bionic/usr/lib.
# Exit status = test failure count.
set -euo pipefail

NDK="${NDK:-$HOME/Android/Sdk/ndk/27.3.13750724}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VULKAN_INCLUDE="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include"

OUT="/tmp/wrapper_testbench"
"$CC" \
    -I"$VULKAN_INCLUDE" \
    -O2 -Wall -Wextra \
    -o "$OUT" \
    "$SCRIPT_DIR/main.c" \
    -ldl

echo "built: $OUT ($(stat -c '%s bytes' "$OUT"))"

PKG=com.mediatek.steamlauncher
adb push "$OUT" /data/local/tmp/wrapper_testbench >/dev/null
adb shell "run-as $PKG cp /data/local/tmp/wrapper_testbench files/wrapper_testbench"
adb shell "run-as $PKG chmod 755 files/wrapper_testbench"

# Run inside the app's data dir. LD_LIBRARY_PATH must be ABSOLUTE
# (bionic loader rejects relative). Includes:
#   - imagefs_bionic/usr/lib  (wrapper deps: xcb/drm/sysvshm/etc.)
#   - app nativeLibraryDir    (libc++_shared etc. fallback)
DATA_DIR="/data/data/$PKG"
APP_LIB=$(adb shell "pm path $PKG" | head -1 | sed 's|package:||;s|/[^/]*$||')/lib/arm64
adb shell "run-as $PKG sh -c '
    cd $DATA_DIR/files
    export LD_LIBRARY_PATH=$DATA_DIR/files/imagefs_bionic/usr/lib:$APP_LIB
    export WRAPPER_TESTBENCH_LIB=$DATA_DIR/files/imagefs_bionic/usr/lib/libvulkan_wrapper.so
    ./wrapper_testbench
'"
echo "(exit=$?)"
