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
#
#   sekiro_telemetry.sh dxvk-swap <staged-dir> [proton-name]
#       Swap the named Proton install's BUNDLED DXVK (at
#       <proton>/files/lib64/wine/dxvk/) with <staged-dir>/{d3d11,dxgi}.dll.
#       Proton repaves the prefix's system32 from this directory on every
#       launch, so swapping the prefix directly is futile — Proton wins
#       every time. Backups go to ~/sekiro_telemetry/dxvk-backup/<proton>/
#       (namespaced per Proton install since each ships its own DXVK
#       version). Default proton-name = "Proton 9.0 (Beta)".
#       NOTE: this affects ALL games using that Proton, not just Sekiro.
#       Run dxvk-restore when capture is done.
#
#   sekiro_telemetry.sh dxvk-restore [proton-name]
#       Restore the named Proton's bundled DXVK from backup.
#
#   sekiro_telemetry.sh dxvk-status [proton-name]
#       Show md5sums of (a) Proton's bundled DXVK, (b) backup, and (c)
#       the prefix's currently-installed DXVK (which Proton overwrites
#       on every launch).
#
#   sekiro_telemetry.sh launch-shader-capture
#       Print a Steam launch-options string that adds
#       DXVK_SHADER_DUMP_PATH to the existing telemetry env. Each run
#       writes <name>.spv + <name>.dxbc to a new timestamped subdir of
#       ~/sekiro_telemetry/shaders/. Use these .spv files as input to
#       the wrapper_testbench shader replay harness to verify whether
#       captured DXVK SPIR-V survives Mali's vkCreateShaderModule.
#
#   sekiro_telemetry.sh run-shader-capture [proton-version]
#       Launch Sekiro directly via Proton with the shader-capture env
#       set. Bypasses Steam launch options entirely so you don't have
#       to paste-and-revert. Steam itself should be running first so
#       steam_api64.dll has a process to talk to. proton-version
#       defaults to "Proton 9.0 (Beta)"; pass another folder name from
#       ~/.steam/steam/steamapps/common/ to override (e.g.
#       "Proton - Experimental").

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LAYER_SRC="$REPO/fex-emu/vulkan_gc_layer.c"
BUILD_DIR="$REPO/scripts/pc/build"
SO_PATH="$BUILD_DIR/libvulkan_gc_x64.so"
MANIFEST_DIR="$HOME/.local/share/vulkan/implicit_layer.d"
MANIFEST_PATH="$MANIFEST_DIR/VK_LAYER_VK_GC_x64.json"
LOG_DIR="$HOME/sekiro_telemetry"
SEKIRO_APPID=814380
STEAM_ROOT="$HOME/.steam/steam"
SEKIRO_PFX="$STEAM_ROOT/steamapps/compatdata/$SEKIRO_APPID/pfx"
SEKIRO_DIR="$STEAM_ROOT/steamapps/common/Sekiro"
DXVK_BACKUP_DIR="$LOG_DIR/dxvk-backup"
SHADER_DUMP_ROOT="$LOG_DIR/shaders"
DEFAULT_PROTON_NAME="Proton 9.0 (Beta)"

# Auto-pick the right Proton for the prefix's CURRENT version. The
# prefix's `version` file is the source of truth: forcing a different
# Proton triggers a one-way prefix upgrade (which can hang for
# minutes the first time and is hard to revert). Map version-prefix
# strings to installed Proton folder names; fall back to the explicit
# default if no match.
proton_for_prefix() {
    local VFILE="$1/version"
    [ -f "$VFILE" ] || { printf '%s' "$DEFAULT_PROTON_NAME"; return; }
    local V; V="$(cat "$VFILE")"
    case "$V" in
        9.0-*)   printf '%s' "Proton 9.0 (Beta)" ;;
        8.0-*)   printf '%s' "Proton 8.0" ;;
        # Proton Experimental and Hotfix don't ship a stable version
        # prefix the way numbered releases do, so we don't auto-pick
        # those; user can pass them explicitly.
        *)       printf '%s' "$DEFAULT_PROTON_NAME" ;;
    esac
}

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

# Verify the prefix path exists; reused by all dxvk-* + shader-capture subcommands.
require_prefix() {
    if [ ! -d "$SEKIRO_PFX/drive_c/windows/system32" ]; then
        echo "ERROR: Sekiro Steam prefix not found at $SEKIRO_PFX"
        echo "       Steam appid is $SEKIRO_APPID. Verify the game has run at least once."
        exit 1
    fi
}

# Resolve a Proton install's DXVK directory. Proton overwrites the
# prefix's system32 DXVK from this location on every launch — so the
# only way to inject our DXVK is to put it here. $1 = proton folder
# name (e.g. "Proton 9.0 (Beta)"); defaults to $DEFAULT_PROTON_NAME.
proton_dxvk_dir() {
    local NAME="${1:-$DEFAULT_PROTON_NAME}"
    local DIR="$STEAM_ROOT/steamapps/common/$NAME/files/lib64/wine/dxvk"
    [ -d "$DIR" ] || { echo "ERROR: Proton DXVK dir not at '$DIR'." 1>&2; return 1; }
    printf '%s' "$DIR"
}

cmd_dxvk_swap() {
    [ "${1:-}" ] || { echo "Usage: $0 dxvk-swap <staged-dir> [proton-name]"; exit 1; }
    local STAGED="$1"
    local PROTON_NAME="${2:-$DEFAULT_PROTON_NAME}"
    [ -f "$STAGED/d3d11.dll" ] || { echo "ERROR: $STAGED/d3d11.dll missing"; exit 1; }
    [ -f "$STAGED/dxgi.dll"  ] || { echo "ERROR: $STAGED/dxgi.dll missing"; exit 1; }

    local DXVK_DIR; DXVK_DIR="$(proton_dxvk_dir "$PROTON_NAME")" || exit 1
    mkdir -p "$DXVK_BACKUP_DIR"

    # Idempotent backup — only the FIRST swap captures the bundled
    # Proton DXVK, so repeated swaps don't lose the original. Backup
    # is namespaced per Proton install since each ships its own DXVK.
    local BACKUP_NS; BACKUP_NS="$DXVK_BACKUP_DIR/$(echo "$PROTON_NAME" | tr ' /()' '_')"
    mkdir -p "$BACKUP_NS"
    if [ ! -f "$BACKUP_NS/d3d11.dll.original" ]; then
        cp -v "$DXVK_DIR/d3d11.dll" "$BACKUP_NS/d3d11.dll.original"
        cp -v "$DXVK_DIR/dxgi.dll"  "$BACKUP_NS/dxgi.dll.original"
        echo "==> backup of $PROTON_NAME's bundled DXVK at $BACKUP_NS"
    else
        echo "==> backup already present at $BACKUP_NS (left intact)"
    fi

    # Proton ships its bundled DXVK files as read-only; chmod first
    # so cp can overwrite. We restore the read-only mode after the
    # write to match the rest of Proton's tree.
    chmod u+w "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll"
    cp -v "$STAGED/d3d11.dll" "$DXVK_DIR/d3d11.dll"
    cp -v "$STAGED/dxgi.dll"  "$DXVK_DIR/dxgi.dll"
    chmod a-w "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll"
    echo "==> DXVK swapped in $PROTON_NAME's tree. md5sum:"
    md5sum "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll"
    echo
    echo "WARN: this affects ALL games that use $PROTON_NAME, not just Sekiro."
    echo "      Run '$0 dxvk-restore' (with same proton-name) to revert."
}

cmd_dxvk_restore() {
    local PROTON_NAME="${1:-$DEFAULT_PROTON_NAME}"
    local DXVK_DIR; DXVK_DIR="$(proton_dxvk_dir "$PROTON_NAME")" || exit 1
    local BACKUP_NS; BACKUP_NS="$DXVK_BACKUP_DIR/$(echo "$PROTON_NAME" | tr ' /()' '_')"
    [ -f "$BACKUP_NS/d3d11.dll.original" ] \
        || { echo "ERROR: no backup at $BACKUP_NS — nothing to restore"; exit 1; }
    chmod u+w "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll"
    cp -v "$BACKUP_NS/d3d11.dll.original" "$DXVK_DIR/d3d11.dll"
    cp -v "$BACKUP_NS/dxgi.dll.original"  "$DXVK_DIR/dxgi.dll"
    chmod a-w "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll"
    echo "==> restored $PROTON_NAME's bundled DXVK. md5sum:"
    md5sum "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll"
}

cmd_dxvk_status() {
    local PROTON_NAME="${1:-$DEFAULT_PROTON_NAME}"
    local DXVK_DIR; DXVK_DIR="$(proton_dxvk_dir "$PROTON_NAME")" || exit 1
    local BACKUP_NS; BACKUP_NS="$DXVK_BACKUP_DIR/$(echo "$PROTON_NAME" | tr ' /()' '_')"
    echo "==> currently in $PROTON_NAME's tree (what Proton copies into prefixes):"
    md5sum "$DXVK_DIR/d3d11.dll" "$DXVK_DIR/dxgi.dll" 2>&1
    if [ -f "$BACKUP_NS/d3d11.dll.original" ]; then
        echo "==> backup of bundled DXVK at $BACKUP_NS:"
        md5sum "$BACKUP_NS/d3d11.dll.original" "$BACKUP_NS/dxgi.dll.original" 2>&1
    else
        echo "==> no backup at $BACKUP_NS — never swapped"
    fi
    if [ -f "$SEKIRO_PFX/drive_c/windows/system32/d3d11.dll" ]; then
        echo "==> Sekiro prefix's currently-installed DXVK (Proton repaves on launch):"
        md5sum "$SEKIRO_PFX/drive_c/windows/system32/d3d11.dll" \
               "$SEKIRO_PFX/drive_c/windows/system32/dxgi.dll" 2>&1
    fi
}

cmd_run_shader_capture() {
    [ -f "$MANIFEST_PATH" ] || { echo "Run '$0 install' first."; exit 1; }
    require_prefix
    [ -x "$SEKIRO_DIR/sekiro.exe" ] || [ -f "$SEKIRO_DIR/sekiro.exe" ] \
        || { echo "ERROR: $SEKIRO_DIR/sekiro.exe not found"; exit 1; }

    # If the user didn't explicitly pass a Proton, pick whichever one
    # matches the prefix's `version` file — this avoids triggering a
    # forced upgrade that can hang for minutes and is hard to revert.
    local PROTON_NAME="${1:-$(proton_for_prefix "$STEAM_ROOT/steamapps/compatdata/$SEKIRO_APPID")}"
    local PROTON_DIR="$STEAM_ROOT/steamapps/common/$PROTON_NAME"
    [ -x "$PROTON_DIR/proton" ] \
        || { echo "ERROR: Proton not at '$PROTON_DIR/proton'. Available:";
             ls "$STEAM_ROOT/steamapps/common/" | grep -i proton; exit 1; }
    echo "==> auto-picked Proton: $PROTON_NAME (matches prefix version $(cat "$STEAM_ROOT/steamapps/compatdata/$SEKIRO_APPID/version" 2>/dev/null))"

    # Soft-check Steam is up. Direct Proton launch needs Steam running
    # so steam_api64.dll has something to talk to (Sekiro spins up its
    # Steam DRM check at startup; without Steam it exits silently).
    if ! pgrep -x steam >/dev/null 2>&1; then
        echo "WARN: Steam doesn't appear to be running. Sekiro will likely"
        echo "      exit on the steam_api check. Start Steam first, then"
        echo "      re-run this command."
        echo "      (Continue anyway in 5s — Ctrl-C to abort.)"
        sleep 5
    fi

    local stamp; stamp="$(date +%Y%m%d-%H%M%S)"
    local DUMP_DIR="$SHADER_DUMP_ROOT/$stamp"
    local RUN_LOG="$LOG_DIR/run-$stamp.log"
    mkdir -p "$DUMP_DIR" "$LOG_DIR"

    # Note which DXVK Proton will use (informational only; for Phase 0
    # we want stock DXVK, so don't error out if it's not the swap).
    local DXVK_DIR
    DXVK_DIR="$(proton_dxvk_dir "$PROTON_NAME" 2>/dev/null)" || true
    if [ -n "$DXVK_DIR" ]; then
        echo "==> DXVK in $PROTON_NAME's tree (will be copied into prefix on launch):"
        md5sum "$DXVK_DIR/d3d11.dll" | sed 's|^|    |'
    fi

    echo "==> dump dir:  $DUMP_DIR"
    echo "==> run log:   $RUN_LOG"
    echo "==> Proton:    $PROTON_DIR"
    echo "==> game exe:  $SEKIRO_DIR/sekiro.exe"
    echo "==> launching..."

    cd "$SEKIRO_DIR"
    env \
        VK_GC_ENABLE=1 \
        VK_GC_INTERVAL=120 \
        VK_GC_LOG_EVERY=600 \
        DXVK_HUD=fps,frametime,gpuload,memory,allocations,api \
        DXVK_LOG_LEVEL=info \
        DXVK_LOG_PATH="$LOG_DIR" \
        DXVK_SHADER_DUMP_PATH="$DUMP_DIR" \
        WINEDLLOVERRIDES="d3d11=n,b;dxgi=n,b" \
        PROTON_LOG=1 \
        PROTON_LOG_DIR="$LOG_DIR" \
        STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT" \
        STEAM_COMPAT_DATA_PATH="$STEAM_ROOT/steamapps/compatdata/$SEKIRO_APPID" \
        "$PROTON_DIR/proton" run "$SEKIRO_DIR/sekiro.exe" 2>&1 | tee "$RUN_LOG"

    local SPV_COUNT
    SPV_COUNT="$(ls -1 "$DUMP_DIR"/*.spv 2>/dev/null | wc -l)"
    echo "==> game exited"
    echo "==> captured $SPV_COUNT .spv shader(s) in $DUMP_DIR"
    echo "==> run log:  $RUN_LOG"
    if [ "$SPV_COUNT" -eq 0 ]; then
        echo "WARN: no shaders captured. Likely causes:"
        echo "      - WINEDLLOVERRIDES didn't take effect (DXVK never loaded)"
        echo "      - the game exited before any shader compile"
        echo "      Inspect $RUN_LOG for the failure mode."
    fi
}

cmd_launch_shader_capture() {
    [ -f "$MANIFEST_PATH" ] || { echo "Run '$0 install' first."; exit 1; }
    local stamp; stamp="$(date +%Y%m%d-%H%M%S)"
    local DUMP_DIR="$SHADER_DUMP_ROOT/$stamp"
    mkdir -p "$DUMP_DIR"

    cat <<HEREDOC
=== Steam launch option (shader-capture run) ===

Dump dir for this run: $DUMP_DIR
(both .spv and .dxbc files land here — only .spv is used by replay)

Right-click Sekiro in Steam -> Properties -> General -> LAUNCH OPTIONS.
Paste this in (one line, including the trailing %command%):

env VK_GC_ENABLE=1 VK_GC_INTERVAL=120 VK_GC_LOG_EVERY=600 DXVK_HUD=fps,frametime,gpuload,memory,allocations,api DXVK_LOG_LEVEL=info DXVK_LOG_PATH=$LOG_DIR DXVK_SHADER_DUMP_PATH=$DUMP_DIR WINEDLLOVERRIDES="d3d11=n,b;dxgi=n,b" PROTON_LOG=1 PROTON_LOG_DIR=$LOG_DIR %command%

After the play session, count the captured shaders:
  ls $DUMP_DIR/*.spv | wc -l

Then move the directory to a known location and feed it to the testbench
shader replay harness. To return to the original Proton DXVK afterwards:
  $0 dxvk-restore
HEREDOC
}

case "${1:-help}" in
    install)                 cmd_install ;;
    launch-options)          cmd_launch_options ;;
    wrap)                    shift; cmd_wrap "$@" ;;
    tail)                    cmd_tail ;;
    uninstall)               cmd_uninstall ;;
    dxvk-swap)               shift; cmd_dxvk_swap "$@" ;;
    dxvk-restore)            shift; cmd_dxvk_restore "$@" ;;
    dxvk-status)             shift; cmd_dxvk_status "$@" ;;
    launch-shader-capture)   cmd_launch_shader_capture ;;
    run-shader-capture)      shift; cmd_run_shader_capture "$@" ;;
    *)
        echo "Usage: $0 {install|launch-options|wrap '<cmd>'|tail|uninstall|"
        echo "          dxvk-swap <staged-dir>|dxvk-restore|dxvk-status|"
        echo "          launch-shader-capture|run-shader-capture [proton-name]}"
        exit 1 ;;
esac
