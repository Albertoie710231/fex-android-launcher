#!/bin/bash
# Cross-compile vortek_icd_wrapper.so for ARM64 (aarch64-linux-gnu, glibc)
#
# This creates a thin ICD wrapper that bridges the Vortek library (from Winlator)
# to the standard Vulkan ICD loader protocol.
#
# Output: out/vortek-wrapper/libvortek_icd_wrapper.so (ARM64, glibc)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JNILIBS="$SCRIPT_DIR/../app/src/main/jniLibs/arm64-v8a"

cd "$SCRIPT_DIR"

echo "=== Cross-compiling vortek_icd_wrapper.so for ARM64 ==="

docker run --rm --platform linux/amd64 \
    -v "$(pwd)/vortek_icd_wrapper.c:/src/vortek_icd_wrapper.c:ro" \
    -v "$(pwd)/out:/out" \
    ubuntu:22.04 bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq gcc-aarch64-linux-gnu 2>&1 | tail -1

echo "=== Compiling ==="
aarch64-linux-gnu-gcc \
    -shared -fPIC -O2 \
    -o /out/libvortek_icd_wrapper.so \
    /src/vortek_icd_wrapper.c \
    -ldl \
    -Wl,-soname,libvortek_icd_wrapper.so \
    -Wl,--no-undefined

echo ""
echo "=== Verifying ==="
file /out/libvortek_icd_wrapper.so
aarch64-linux-gnu-readelf -d /out/libvortek_icd_wrapper.so | grep -E "(NEEDED|SONAME)"
aarch64-linux-gnu-readelf -Ws /out/libvortek_icd_wrapper.so | grep -E "GLOBAL.*DEFAULT" | grep -v UND
echo ""
echo "=== Size ==="
ls -la /out/libvortek_icd_wrapper.so
'

echo ""
echo "=== Copying to jniLibs ==="
cp "$SCRIPT_DIR/out/libvortek_icd_wrapper.so" "$JNILIBS/libvortek_icd_wrapper.so"
ls -la "$JNILIBS/libvortek_icd_wrapper.so"

echo ""
echo "=== Done ==="
echo "Deployed to: $JNILIBS/libvortek_icd_wrapper.so"
echo "Update vortek_host_icd.json to point library_path to libvortek_icd_wrapper.so"
