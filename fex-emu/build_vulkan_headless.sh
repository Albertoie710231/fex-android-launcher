#!/bin/bash
# Build the x86-64 headless Vulkan wrapper (libvulkan_headless.so)
# Uses Docker with Ubuntu 22.04 cross-compiler

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ASSETS_DIR="$PROJECT_DIR/app/src/main/assets"

echo "=== Building libvulkan_headless.so (x86-64) ==="

docker run --rm -v "$ASSETS_DIR:/work" ubuntu:22.04 bash -c "
    apt-get update -qq &&
    apt-get install -y -qq gcc-x86-64-linux-gnu > /dev/null 2>&1 &&
    x86_64-linux-gnu-gcc -shared -fPIC -O2 \
        -o /work/libvulkan_headless.so \
        /work/vulkan_headless.c \
        -ldl -lpthread &&
    echo 'BUILD OK' &&
    ls -la /work/libvulkan_headless.so
"

echo "=== Deploying to device rootfs ==="
adb push "$ASSETS_DIR/libvulkan_headless.so" /data/local/tmp/
adb shell run-as com.mediatek.steamlauncher \
    cp /data/local/tmp/libvulkan_headless.so \
    files/fex-rootfs/Ubuntu_22_04/usr/lib/libvulkan_headless.so

echo "=== Done ==="
