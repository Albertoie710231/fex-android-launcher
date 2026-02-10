#!/bin/bash
# Sets up the FEX-Emu source tree with Android patches applied.
# Run this once before building with rebuild_fex_quick.sh or build_fex_cross.sh.
#
# Base: FEX-Emu commit aabdded (FEX-2506, "Merge pull request #5279")
# Patches: fex-android-patches.patch (SysV IPC emulation, seccomp handler,
#          rootfs overlay fixes, UID emulation, mkfifo workaround, etc.)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FEX_DIR="$SCRIPT_DIR/fex-src/FEX"
FEX_REPO="https://github.com/FEX-Emu/FEX.git"
FEX_COMMIT="aabdded"
PATCH_FILE="$SCRIPT_DIR/fex-android-patches.patch"

if [ ! -f "$PATCH_FILE" ]; then
    echo "ERROR: Patch file not found: $PATCH_FILE"
    exit 1
fi

if [ -d "$FEX_DIR" ]; then
    echo "FEX source already exists at $FEX_DIR"
    echo "To re-setup, remove it first: rm -rf $FEX_DIR"

    # Check if patches are already applied
    cd "$FEX_DIR"
    if git diff --quiet HEAD 2>/dev/null; then
        echo "No local changes detected. Applying patches..."
        git apply "$PATCH_FILE"
        echo "Patches applied successfully."
    else
        echo "Local changes already present. Skipping patch application."
        echo "To re-apply: cd $FEX_DIR && git checkout . && git apply $PATCH_FILE"
    fi
    exit 0
fi

echo "=== Cloning FEX-Emu ==="
mkdir -p "$SCRIPT_DIR/fex-src"
git clone --recurse-submodules "$FEX_REPO" "$FEX_DIR"

echo "=== Checking out base commit ($FEX_COMMIT) ==="
cd "$FEX_DIR"
git checkout "$FEX_COMMIT"
git submodule update --init --recursive

echo "=== Applying Android patches ==="
git apply "$PATCH_FILE"

echo "=== Done ==="
echo "FEX source ready at: $FEX_DIR"
echo "Build with: cd $SCRIPT_DIR && ./build_fex_cross.sh (full) or ./rebuild_fex_quick.sh (incremental)"
