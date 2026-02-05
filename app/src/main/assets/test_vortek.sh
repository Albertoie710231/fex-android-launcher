#!/bin/bash
#
# Test Vortek Vulkan Rendering Pipeline
#

echo "=== Vortek Vulkan Rendering Test ==="
echo ""

# Setup environment
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
export VORTEK_SERVER_PATH=/tmp/vortek.sock

echo "1. Checking Vortek socket..."
if [ -S "$VORTEK_SERVER_PATH" ]; then
    echo "   OK: Vortek socket found at $VORTEK_SERVER_PATH"
else
    echo "   ERROR: Vortek socket not found!"
    echo "   Make sure the Steam Launcher service is running."
    exit 1
fi

echo ""
echo "2. Checking Vulkan ICD..."
if [ -f "$VK_ICD_FILENAMES" ]; then
    echo "   OK: Vortek ICD found"
    cat "$VK_ICD_FILENAMES"
else
    echo "   ERROR: Vortek ICD not found at $VK_ICD_FILENAMES"
    exit 1
fi

echo ""
echo "3. Testing vulkaninfo..."
if command -v vulkaninfo &> /dev/null; then
    # Run vulkaninfo and extract key info
    vulkaninfo --summary 2>&1 | head -30
else
    echo "   vulkaninfo not found, trying /usr/local/bin/vulkaninfo"
    if [ -x /usr/local/bin/vulkaninfo ]; then
        /usr/local/bin/vulkaninfo --summary 2>&1 | head -30
    else
        echo "   ERROR: vulkaninfo not available"
    fi
fi

echo ""
echo "4. Checking for vkcube..."
VKCUBE=""
if command -v vkcube &> /dev/null; then
    VKCUBE="vkcube"
elif [ -x /tmp/vkcube ]; then
    VKCUBE="/tmp/vkcube"
elif [ -x /usr/local/bin/vkcube ]; then
    VKCUBE="/usr/local/bin/vkcube"
fi

if [ -n "$VKCUBE" ]; then
    echo "   Found vkcube at: $VKCUBE"
    echo ""
    echo "5. Running vkcube (headless mode)..."
    echo "   Press Ctrl+C to stop"
    echo ""
    # Try to run vkcube with headless/offscreen options
    # Different versions of vkcube have different options
    $VKCUBE --present_mode 0 2>&1 || \
    $VKCUBE 2>&1 || \
    echo "   vkcube failed to run"
else
    echo "   vkcube not found"
    echo ""
    echo "5. Running simple Vulkan test..."

    # If we have the test binary, run it
    if [ -x /tmp/test_vortek_render ]; then
        /tmp/test_vortek_render
    else
        echo "   No test binary available"
        echo "   To compile the test:"
        echo "   gcc -o /tmp/test_vortek_render /tmp/test_vortek_render.c -lvulkan"
    fi
fi

echo ""
echo "=== Test Complete ==="
