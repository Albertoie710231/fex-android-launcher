#!/bin/bash
#
# Setup Vulkan Headless Surface Support
#
# This script compiles the headless surface wrapper library.
#

echo "=== Setting up Vulkan Headless Surface Support ==="

# Check if gcc is available
if ! command -v gcc &> /dev/null; then
    echo "ERROR: gcc not found. Please install build tools:"
    echo "  apt-get update && apt-get install -y gcc"
    exit 1
fi

# Source file location (copied by setup.sh)
SRC="/tmp/vulkan_headless.c"
OUT="/lib/libvulkan_headless.so"

if [ ! -f "$SRC" ]; then
    echo "ERROR: Source file not found at $SRC"
    echo "Make sure vulkan_headless.c is copied to /tmp/"
    exit 1
fi

echo "Compiling headless surface wrapper..."
gcc -shared -fPIC -O2 -o "$OUT" "$SRC" -ldl -lpthread 2>&1

if [ $? -eq 0 ] && [ -f "$OUT" ]; then
    echo "SUCCESS: Compiled $OUT"
    ls -la "$OUT"
    echo ""
    echo "Usage:"
    echo "  LD_PRELOAD=$OUT vkcube"
    echo "  LD_PRELOAD=$OUT vulkaninfo"
else
    echo "ERROR: Compilation failed"
    exit 1
fi

echo ""
echo "=== Setup Complete ==="
