#!/usr/bin/env bash
# Run Sekiro on PC with the same VK_GC instrumentation layer the tablet
# uses, plus DXVK_HUD overlay. Produces per-600-submit `[vk_gc periodic]`
# log lines directly comparable to /sdcard/.../ys9_stderr.live.log on
# the tablet, so we can answer "is 39k VkImageView normal DXVK behavior
# or specific to our Mali stack?"
#
# Usage modes:
#   sekiro_telemetry.sh install
#       Build libvulkan_gc.so (Linux x86_64) and install the implicit
#       layer manifest to ~/.local/share/vulkan/implicit_layer.d/.
#       Idempotent. Run once before launching the game.
#
#   sekiro_telemetry.sh launch-options
#       Print the Steam launch option string. Paste it into
#       Steam → Sekiro → Properties → General → LAUNCH OPTIONS.
#       Then start Sekiro from Steam normally; logs land in
#       ~/sekiro_telemetry/run-YYYYMMDD-HHMMSS.log.
#
#   sekiro_telemetry.sh wrap '<launch command>'
#       Direct launch wrapper. Sets all env vars and execs the given
#       command. Useful if you launch via wine/proton from the terminal
#       instead of through Steam.
#
#   sekiro_telemetry.sh tail
#       Tail the most recent run log, filtered to vk_gc + DXVK info.
#
#   sekiro_telemetry.sh uninstall
#       Remove the layer manifest. Library .so stays in build dir.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LAYER_SRC="$REPO/fex-emu/vulkan_gc_layer.c"
BUILD_DIR="$REPO/scripts/pc/build"
SO_PATH="$BUILD_DIR/libvulkan_gc_x64.so"
MANIFEST_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
MANIFEST_PATH="$MANIFEST_DIR/VK_LAYER_VK_GC_x64.json"
LOG_DIR="$HOME/sekiro_telemetry"

cmd_install() {
    [ -f "$LAYER_SRC" ] || { echo "ERROR: layer source missing at $LAYER_SRC"; exit 1; }

    mkdir -p "$BUILD_DIR" "$MANIFEST_DIR" "$LOG_DIR"

    echo "==> compile $LAYER_SRC -> $SO_PATH"
    gcc -shared -fPIC -O2 -Wall \
        -o "$SO_PATH" "$LAYER_SRC"

    cat > "$MANIFEST_PATH" <<JSON
{
    "file_format_version" : "1.0.0",
    "layer" : {
        "name": "VK_LAYER_VK_GC",
        "type": "GLOBAL",
        "library_path": "$SO_PATH",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "Periodic vkDeviceWaitIdle + per-class create/destroy stats.",
        "functions": {
            "vkGetInstanceProcAddr": "vk_gc_GetInstanceProcAddr",
            "vkGetDeviceProcAddr": "vk_gc_GetDeviceProcAddr"
        },
        "instance_extensions": [],
        "device_extensions": [],
        "enable_environment": {
            "VK_GC_ENABLE": "1"
        },
        "disable_environment": {
            "VK_GC_DISABLE": "1"
        }
    }
}
JSON
    echo "==> manifest written: $MANIFEST_PATH"
    echo "==> done. Layer is opt-in via VK_GC_ENABLE=1 (see launch-options)."
}

# Env vars the wrapper sets:
#   VK_GC_ENABLE=1            activates our layer
#   VK_GC_INTERVAL=120        waitIdle every 120 submits (tablet default)
#   VK_GC_LOG_EVERY=600       periodic stats every 600 submits (tablet default)
#   DXVK_HUD=fps,frametime,gpuload,memory,allocations,api
#   DXVK_LOG_LEVEL=info       so DXVK prints adapter/feature lines for cross-check
#   DXVK_LOG_PATH=$LOG_DIR    DXVK per-DLL logs land alongside our layer log
print_env() {
    cat <<ENV
VK_GC_ENABLE=1 \\
VK_GC_INTERVAL=120 \\
VK_GC_LOG_EVERY=600 \\
DXVK_HUD=fps,frametime,gpuload,memory,allocations,api \\
DXVK_LOG_LEVEL=info \\
DXVK_LOG_PATH="$LOG_DIR"
ENV
}

cmd_launch_options() {
    [ -f "$MANIFEST_PATH" ] || { echo "Run '$0 install' first."; exit 1; }
    cat <<HEREDOC
=== Steam launch option ===

Right-click Sekiro in Steam -> Properties -> General -> LAUNCH OPTIONS.
Paste this in (one line, including the trailing %command%):

env VK_GC_ENABLE=1 VK_GC_INTERVAL=120 VK_GC_LOG_EVERY=600 DXVK_HUD=fps,frametime,gpuload,memory,allocations,api DXVK_LOG_LEVEL=info DXVK_LOG_PATH=$LOG_DIR PROTON_LOG=1 PROTON_LOG_DIR=$LOG_DIR %command%

After a play session you'll find:
  $LOG_DIR/sekiro_d3d11.log   -> DXVK D3D11 init (adapter, features, swap chain)
  $LOG_DIR/sekiro_dxgi.log    -> DXVK DXGI side
  $LOG_DIR/steam-*.log        -> Proton stderr including [vk_gc periodic] lines
                                  (PROTON_LOG=1 makes Proton log to file)

Use '$0 tail' to follow the latest run live.
HEREDOC
}

cmd_wrap() {
    [ "$#" -ge 1 ] || { echo "Usage: $0 wrap '<command>'"; exit 1; }
    [ -f "$MANIFEST_PATH" ] || { echo "Run '$0 install' first."; exit 1; }

    mkdir -p "$LOG_DIR"
    local stamp; stamp="$(date +%Y%m%d-%H%M%S)"
    local log="$LOG_DIR/run-$stamp.log"

    export VK_GC_ENABLE=1 VK_GC_INTERVAL=120 VK_GC_LOG_EVERY=600
    export DXVK_HUD=fps,frametime,gpuload,memory,allocations,api
    export DXVK_LOG_LEVEL=info
    export DXVK_LOG_PATH="$LOG_DIR"
    echo "==> launching, log: $log"
    exec bash -c "$*" 2>&1 | tee "$log"
}

cmd_tail() {
    local latest
    latest="$(ls -1t "$LOG_DIR"/run-*.log 2>/dev/null | head -1)"
    if [ -z "$latest" ]; then
        # Fall back to Proton stderr or DXVK logs
        latest="$(ls -1t "$LOG_DIR"/*.log 2>/dev/null | head -1)"
    fi
    [ -n "$latest" ] || { echo "no logs in $LOG_DIR yet"; exit 1; }
    echo "==> tailing $latest (vk_gc + DXVK info only; Ctrl-C to exit)"
    tail -F "$latest" | grep --line-buffered -E "vk_gc|^info:|^warn:|^err:"
}

cmd_uninstall() {
    rm -f "$MANIFEST_PATH" && echo "==> removed $MANIFEST_PATH"
}

case "${1:-help}" in
    install)         cmd_install ;;
    launch-options)  cmd_launch_options ;;
    wrap)            shift; cmd_wrap "$@" ;;
    tail)            cmd_tail ;;
    uninstall)       cmd_uninstall ;;
    *)
        echo "Usage: $0 {install|launch-options|wrap '<cmd>'|tail|uninstall}"
        exit 1 ;;
esac
