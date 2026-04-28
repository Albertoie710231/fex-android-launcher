#!/usr/bin/env bash
# Cross-compile wrapper_testbench, push to device, run inside the app's
# data dir via run-as so the bionic loader can resolve the wrapper's
# transitive deps (libandroid-sysvshm, libxcb*, libdrm, libadrenotools,
# libc++_shared) the same way games do — from imagefs_bionic/usr/lib.
# Exit status = test failure count.
set -euo pipefail

NDK="${NDK:-$HOME/Android/Sdk/ndk/27.3.13750724}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++"
GLSLC="$NDK/shader-tools/linux-x86_64/glslc"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VULKAN_INCLUDE="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include"

# --- SPIRV-Tools cross-compile (one-time, cached) -------------------------
# Built once per submodule revision; subsequent runs skip if the static
# libraries are already present and newer than the submodule HEAD. We
# only need libSPIRV-Tools.a + libSPIRV-Tools-opt.a — skip tests, fuzzers,
# and executables to cut build time and binary size.
SPIRV_TOOLS_SRC="$SCRIPT_DIR/external/SPIRV-Tools"
SPIRV_HEADERS_SRC="$SCRIPT_DIR/external/SPIRV-Headers"
# Build outside the submodule tree so the submodule working tree stays
# clean (otherwise `git status` shows "modified content" for SPIRV-Tools).
SPIRV_TOOLS_BUILD="$SCRIPT_DIR/build/spirv-tools-arm64"
SPIRV_TOOLS_LIB="$SPIRV_TOOLS_BUILD/source/libSPIRV-Tools.a"
SPIRV_TOOLS_OPT_LIB="$SPIRV_TOOLS_BUILD/source/opt/libSPIRV-Tools-opt.a"
if [ ! -f "$SPIRV_TOOLS_LIB" ] || [ ! -f "$SPIRV_TOOLS_OPT_LIB" ]; then
    echo "configuring SPIRV-Tools for android-aarch64..."
    cmake -S "$SPIRV_TOOLS_SRC" -B "$SPIRV_TOOLS_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-28 \
        -DSPIRV-Headers_SOURCE_DIR="$SPIRV_HEADERS_SRC" \
        -DSPIRV_SKIP_TESTS=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        >/dev/null
    echo "compiling SPIRV-Tools (this may take a few minutes the first time)..."
    cmake --build "$SPIRV_TOOLS_BUILD" --parallel "$(nproc)" -- -k 0 \
        SPIRV-Tools-static SPIRV-Tools-opt
fi
echo "SPIRV-Tools libs: $(stat -c '%s' "$SPIRV_TOOLS_LIB") + $(stat -c '%s' "$SPIRV_TOOLS_OPT_LIB") bytes"

# Compile the OOB probe compute shader into a C header. Generated each
# build so editing the .comp invalidates correctly.
SPV_OUT="/tmp/oob_probe.spv"
"$GLSLC" --target-env=vulkan1.3 -o "$SPV_OUT" "$SCRIPT_DIR/oob_probe.comp"
HDR_OUT="$SCRIPT_DIR/oob_probe_spv.h"
{
    printf '/* Auto-generated from oob_probe.comp by build.sh — do not edit. */\n'
    printf 'static const unsigned char oob_probe_spv[] = {\n'
    od -An -v -tx1 "$SPV_OUT" | sed -E 's/[[:space:]]+/ /g; s/ ([0-9a-f]{2})/0x\1,/g; s/^ //'
    printf '};\n'
    printf 'static const unsigned int oob_probe_spv_len = sizeof(oob_probe_spv);\n'
} > "$HDR_OUT"
echo "compiled: $SPV_OUT ($(stat -c '%s bytes' "$SPV_OUT")) → $HDR_OUT"

OUT="/tmp/wrapper_testbench"
"$CC" \
    -I"$VULKAN_INCLUDE" \
    -O2 -Wall -Wextra \
    -o "$OUT" \
    "$SCRIPT_DIR/main.c" \
    -ldl

echo "built: $OUT ($(stat -c '%s bytes' "$OUT"))"

# Build the wrapper shim. Two translation units — the C side (Vulkan
# entrypoint hooks, descriptor-write substitution, etc.) and the C++
# side (SPIR-V instrumentation engine that links SPIRV-Tools).
SHIM_OUT="/tmp/libwrapper_maintenance5_shim.so"
SHIM_C_OBJ="/tmp/shim_maintenance5.o"
SHIM_CPP_OBJ="/tmp/spv_instrumenter.o"
"$CC" \
    -I"$VULKAN_INCLUDE" \
    -O2 -Wall -Wextra -fPIC \
    -c "$SCRIPT_DIR/shim_maintenance5.c" \
    -o "$SHIM_C_OBJ"
"$CXX" \
    -I"$VULKAN_INCLUDE" \
    -I"$SPIRV_TOOLS_SRC/include" \
    -I"$SPIRV_HEADERS_SRC/include" \
    -O2 -Wall -Wextra -fPIC -std=c++17 \
    -c "$SCRIPT_DIR/spv_instrumenter.cpp" \
    -o "$SHIM_CPP_OBJ"
# Link with --whole-archive on SPIRV-Tools-opt so the optimizer pass
# registry isn't dead-stripped (passes self-register via static ctors).
"$CXX" \
    -O2 -fPIC -shared \
    -o "$SHIM_OUT" \
    "$SHIM_C_OBJ" "$SHIM_CPP_OBJ" \
    -Wl,--whole-archive "$SPIRV_TOOLS_OPT_LIB" -Wl,--no-whole-archive \
    "$SPIRV_TOOLS_LIB" \
    -ldl -static-libstdc++
echo "built: $SHIM_OUT ($(stat -c '%s bytes' "$SHIM_OUT"))"

PKG=com.mediatek.steamlauncher
adb push "$OUT" /data/local/tmp/wrapper_testbench >/dev/null
adb push "$SHIM_OUT" /data/local/tmp/libwrapper_maintenance5_shim.so >/dev/null
adb shell "run-as $PKG cp /data/local/tmp/wrapper_testbench files/wrapper_testbench"
adb shell "run-as $PKG cp /data/local/tmp/libwrapper_maintenance5_shim.so files/libwrapper_maintenance5_shim.so"
adb shell "run-as $PKG chmod 755 files/wrapper_testbench"

# Run inside the app's data dir. LD_LIBRARY_PATH must be ABSOLUTE
# (bionic loader rejects relative). Includes:
#   - imagefs_bionic/usr/lib  (wrapper deps: xcb/drm/sysvshm/etc.)
#   - app nativeLibraryDir    (libc++_shared etc. fallback)
DATA_DIR="/data/data/$PKG"
APP_LIB=$(adb shell "pm path $PKG" | head -1 | sed 's|package:||;s|/[^/]*$||')/lib/arm64
echo
echo "=========================================================="
echo "PASS A: vanilla wrapper (baseline)"
echo "=========================================================="
adb shell "run-as $PKG sh -c '
    cd $DATA_DIR/files
    export LD_LIBRARY_PATH=$DATA_DIR/files/imagefs_bionic/usr/lib:$APP_LIB
    export WRAPPER_TESTBENCH_LIB=$DATA_DIR/files/imagefs_bionic/usr/lib/libvulkan_wrapper.so
    ./wrapper_testbench
' 2>&1 | grep -E '^\[|^==='"

echo
echo "=========================================================="
echo "PASS B: maintenance5 shim (forwards to vanilla, injects ext)"
echo "=========================================================="
adb shell "run-as $PKG sh -c '
    cd $DATA_DIR/files
    export LD_LIBRARY_PATH=$DATA_DIR/files/imagefs_bionic/usr/lib:$APP_LIB
    export WRAPPER_TESTBENCH_LIB=$DATA_DIR/files/libwrapper_maintenance5_shim.so
    ./wrapper_testbench
' 2>&1 | grep -E '^\[|^==='"
echo "(done)"
