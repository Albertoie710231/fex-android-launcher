#!/system/bin/sh
# Test VK_KHR_xcb_surface support with vkcube

echo "=== Testing VK_KHR_xcb_surface Support ==="
echo ""

# Recompile the library with XCB support
echo "Compiling libvulkan_headless.so with XCB surface support..."
cd /data/data/com.AhmedMourad.steamlink/files/container/ubuntu-fs/root

gcc -shared -fPIC -o /lib/libvulkan_headless.so vulkan_headless.c -ldl -lpthread 2>&1
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to compile libvulkan_headless.so"
    exit 1
fi
echo "Compilation successful!"
echo ""

# Test 1: Check that VK_KHR_xcb_surface is now advertised
echo "=== Test 1: Check extension advertisement ==="
LD_PRELOAD=/lib/libvulkan_headless.so vulkaninfo 2>&1 | grep -i "xcb_surface\|headless_surface"
echo ""

# Test 2: Run vkcube with the XCB surface wrapper
echo "=== Test 2: Run vkcube (XCB surface) ==="
echo "Setting up environment..."

export VK_ICD_FILENAMES=/lib/vortek_icd.json
export LD_PRELOAD=/lib/libvulkan_headless.so
export DISPLAY=:0

echo "Running vkcube for 3 seconds..."
timeout 3 vkcube 2>&1 || true

echo ""
echo "=== Test Complete ==="
echo ""
echo "If vkcube started without 'VK_KHR_xcb_surface not supported' error,"
echo "the XCB surface extension is working!"
