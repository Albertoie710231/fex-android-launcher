#!/bin/bash
# Launch a Windows game via Wine/Proton-GE inside FEX-Emu on Android.
#
# This script runs INSIDE the FEX x86-64 guest environment.
# It handles Xvfb startup, environment configuration, and game launching.
#
# Usage:
#   ./launch_wine.sh /path/to/game.exe
#   ./launch_wine.sh /path/to/game.exe --prefix /home/user/games/mygame/pfx
#   ./launch_wine.sh /path/to/game.exe --no-hud
#   ./launch_wine.sh /path/to/game.exe --opengl   # Use wined3d instead of DXVK
#   ./launch_wine.sh --notepad                      # Quick test with notepad

set -e

PROTON_DIR="/opt/proton-ge"
DEFAULT_PREFIX="/home/user/.wine"

# Parse arguments
EXE_PATH=""
WINEPREFIX="$DEFAULT_PREFIX"
SHOW_HUD=1
USE_DXVK=1
HEADLESS=1
EXTRA_ARGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            WINEPREFIX="$2"
            shift 2
            ;;
        --no-hud)
            SHOW_HUD=0
            shift
            ;;
        --opengl)
            USE_DXVK=0
            shift
            ;;
        --no-headless)
            HEADLESS=0
            shift
            ;;
        --notepad)
            EXE_PATH="notepad"
            shift
            ;;
        --)
            shift
            EXTRA_ARGS="$*"
            break
            ;;
        -*)
            echo "Unknown option: $1"
            echo "Usage: $0 [options] /path/to/game.exe [-- extra_args]"
            exit 1
            ;;
        *)
            EXE_PATH="$1"
            shift
            ;;
    esac
done

if [ -z "$EXE_PATH" ]; then
    echo "Usage: $0 [options] /path/to/game.exe [-- extra_args]"
    echo ""
    echo "Options:"
    echo "  --prefix PATH   Wine prefix directory (default: $DEFAULT_PREFIX)"
    echo "  --no-hud        Disable DXVK HUD overlay"
    echo "  --opengl        Use wined3d (OpenGL) instead of DXVK (Vulkan)"
    echo "  --no-headless   Don't use headless frame capture"
    echo "  --notepad       Quick test with Wine notepad"
    exit 1
fi

# ============================================================
# Pre-flight checks
# ============================================================
echo "=== Wine Launcher for FEX-Emu ==="
echo ""

# Check Proton
if [ ! -x "${PROTON_DIR}/files/bin/wine64" ]; then
    echo "ERROR: Proton-GE not installed at ${PROTON_DIR}"
    echo "Run: ./setup_proton.sh"
    exit 1
fi

# Check exe (skip for built-in commands like notepad)
if [ "$EXE_PATH" != "notepad" ] && [ ! -f "$EXE_PATH" ]; then
    echo "ERROR: Game not found: $EXE_PATH"
    exit 1
fi

# ============================================================
# Start Xvfb if needed
# ============================================================
if ! pgrep -f 'Xvfb :99' >/dev/null 2>&1; then
    echo "Starting Xvfb..."
    mkdir -p /tmp/.X11-unix
    Xvfb :99 -screen 0 1280x720x24 -ac -nolisten local -nolisten unix -listen tcp &
    sleep 1
    if pgrep -f 'Xvfb :99' >/dev/null 2>&1; then
        echo "Xvfb started on :99 (TCP mode)"
    else
        echo "WARNING: Xvfb may have failed to start"
    fi
else
    echo "Xvfb already running on :99"
fi

# ============================================================
# Configure environment
# ============================================================
export WINEPREFIX
export PATH="${PROTON_DIR}/files/bin:${PATH}"
export WINEDLLPATH="${PROTON_DIR}/files/lib64/wine/x86_64-unix:${PROTON_DIR}/files/lib/wine/i386-unix"
export WINELOADER="${PROTON_DIR}/files/bin/wine64"
export WINESERVER="${PROTON_DIR}/files/bin/wineserver"
export DISPLAY=localhost:99

# Proton compatibility (Android kernel limitations)
export PROTON_NO_ESYNC=1
export PROTON_NO_FSYNC=1
export PROTON_ENABLE_NVAPI=0
export PROTON_HIDE_NVIDIA_GPU=0

# DXVK or wined3d
if [ "$USE_DXVK" -eq 1 ]; then
    export PROTON_USE_WINED3D=0
    export DXVK_ASYNC=1
    export DXVK_STATE_CACHE=1
    export DXVK_LOG_LEVEL=info

    if [ "$SHOW_HUD" -eq 1 ]; then
        export DXVK_HUD=fps,devinfo
    fi
else
    export PROTON_USE_WINED3D=1
    export MESA_GL_VERSION_OVERRIDE=4.6
    export MESA_GLSL_VERSION_OVERRIDE=460
fi

# VKD3D (DX12 -> Vulkan)
export VKD3D_FEATURE_LEVEL=12_1

# Vulkan ICD (guest-side, routed through FEX thunks to Vortek)
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json

# Mali GPU workarounds
export MALI_NO_ASYNC_COMPUTE=1

# Headless frame capture
if [ "$HEADLESS" -eq 1 ]; then
    export LD_PRELOAD=/usr/lib/libvulkan_headless.so
fi

# Misc
export XDG_RUNTIME_DIR=/tmp
export TMPDIR=/tmp

# ============================================================
# Initialize Wine prefix if needed
# ============================================================
if [ ! -d "${WINEPREFIX}/drive_c" ]; then
    echo ""
    echo "Initializing Wine prefix at ${WINEPREFIX}..."
    mkdir -p "$WINEPREFIX"
    wineboot -u 2>&1
    echo "Wine prefix created"
fi

# ============================================================
# Launch
# ============================================================
echo ""
echo "Configuration:"
echo "  EXE:        $EXE_PATH"
echo "  Prefix:     $WINEPREFIX"
echo "  Renderer:   $([ "$USE_DXVK" -eq 1 ] && echo 'DXVK (Vulkan)' || echo 'wined3d (OpenGL)')"
echo "  HUD:        $([ "$SHOW_HUD" -eq 1 ] && echo 'enabled' || echo 'disabled')"
echo "  Headless:   $([ "$HEADLESS" -eq 1 ] && echo 'enabled (TCP 19850)' || echo 'disabled')"
echo "  Extra args: ${EXTRA_ARGS:-none}"
echo ""
echo "Launching..."
echo ""

# cd to game directory for relative path assets
if [ "$EXE_PATH" != "notepad" ]; then
    EXE_DIR="$(dirname "$EXE_PATH")"
    cd "$EXE_DIR"
fi

# Launch via wine64
exec wine64 "$EXE_PATH" $EXTRA_ARGS 2>&1
