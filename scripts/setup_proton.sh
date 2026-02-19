#!/bin/bash
# Setup Proton-GE for running Windows games via Wine/DXVK inside FEX-Emu.
#
# This script runs INSIDE the FEX x86-64 guest environment.
# It downloads and extracts GE-Proton10-30 and initializes Wine.
# X11 server (libXlorie) is started by the Android app, NOT by this script.
#
# Usage:
#   ./setup_proton.sh              # Full setup (download + extract + wine init)
#   ./setup_proton.sh --extract    # Extract only (tarball already in /tmp/)
#   ./setup_proton.sh --deps       # Check dependencies only
#   ./setup_proton.sh --x11        # Check X11 server status
#   ./setup_proton.sh --wineboot   # Initialize Wine prefix only

set -e

PROTON_VERSION="GE-Proton10-30"
PROTON_URL="https://github.com/GloriousEggroll/proton-ge-custom/releases/download/${PROTON_VERSION}/${PROTON_VERSION}.tar.gz"
PROTON_DIR="/opt/proton-ge"
TARBALL="/tmp/${PROTON_VERSION}.tar.gz"
WINEPREFIX="/home/user/.wine"

# Colors (if terminal supports it)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }

# ============================================================
# Check Dependencies
# ============================================================
check_deps() {
    echo "=== Checking Dependencies ==="
    local missing=0

    # Python3
    if command -v python3 >/dev/null 2>&1; then
        ok "python3: $(python3 --version 2>&1)"
    else
        warn "python3 not found (Proton launcher script needs it)"
        missing=$((missing + 1))
    fi

    # Core libraries
    for lib in libfreetype.so.6 libfontconfig.so.1 libSDL2-2.0.so.0 libxkbcommon.so.0; do
        if find /usr/lib/x86_64-linux-gnu -name "$lib" 2>/dev/null | head -1 | grep -q .; then
            ok "$lib"
        else
            warn "$lib MISSING"
            missing=$((missing + 1))
        fi
    done

    # X11 libraries
    for lib in libX11.so.6 libXext.so.6 libXrandr.so.2 libXcursor.so.1 libXi.so.6; do
        if find /usr/lib/x86_64-linux-gnu -name "$lib" 2>/dev/null | head -1 | grep -q .; then
            ok "$lib"
        else
            warn "$lib MISSING"
            missing=$((missing + 1))
        fi
    done

    # X11 server (libXlorie — runs on Android side, not inside FEX)
    if (echo > /dev/tcp/localhost/6000) 2>/dev/null; then
        ok "X11 server listening on TCP port 6000 (libXlorie)"
    else
        warn "X11 server not running (press 'Start X' in the app)"
        missing=$((missing + 1))
    fi

    echo ""
    if [ $missing -eq 0 ]; then
        ok "All dependencies satisfied"
    else
        warn "$missing dependencies missing (Wine may still work without some)"
    fi

    # Disk space
    echo ""
    echo "Disk space:"
    df -h /home 2>/dev/null | tail -1 || true

    # Memory
    echo ""
    echo "Memory:"
    free -m 2>/dev/null | head -2 || true
}

# ============================================================
# Download Proton-GE
# ============================================================
download_proton() {
    if [ -f "$TARBALL" ]; then
        SIZE=$(stat -c%s "$TARBALL" 2>/dev/null || echo 0)
        if [ "$SIZE" -gt 100000000 ]; then
            ok "Tarball already downloaded: ${SIZE} bytes"
            return 0
        fi
        warn "Tarball too small (${SIZE} bytes), re-downloading..."
        rm -f "$TARBALL"
    fi

    echo "Downloading ${PROTON_VERSION} (~486MB)..."
    echo "URL: ${PROTON_URL}"
    echo ""

    if command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$TARBALL" "$PROTON_URL" || {
            fail "wget failed"
            if command -v curl >/dev/null 2>&1; then
                echo "Trying curl..."
                curl -L -o "$TARBALL" "$PROTON_URL"
            else
                return 1
            fi
        }
    elif command -v curl >/dev/null 2>&1; then
        curl -L -o "$TARBALL" "$PROTON_URL"
    else
        fail "Neither wget nor curl available!"
        echo ""
        echo "Alternative: download on your PC and push via adb:"
        echo "  wget $PROTON_URL"
        echo "  adb push ${PROTON_VERSION}.tar.gz <rootfs>/tmp/"
        return 1
    fi

    SIZE=$(stat -c%s "$TARBALL" 2>/dev/null || echo 0)
    if [ "$SIZE" -lt 100000000 ]; then
        fail "Download incomplete (${SIZE} bytes)"
        rm -f "$TARBALL"
        return 1
    fi

    ok "Downloaded: ${SIZE} bytes"
}

# ============================================================
# Extract Proton-GE
# ============================================================
extract_proton() {
    if [ -x "${PROTON_DIR}/files/bin/wine64" ]; then
        ok "Proton-GE already installed"
        "${PROTON_DIR}/files/bin/wine64" --version
        return 0
    fi

    if [ ! -f "$TARBALL" ]; then
        fail "Tarball not found: $TARBALL"
        echo "Run setup_proton.sh first (or push tarball via adb)"
        return 1
    fi

    echo "Extracting ${PROTON_VERSION} to /opt/ ..."
    mkdir -p /opt
    tar xzf "$TARBALL" -C /opt/ 2>&1 | tail -5

    # Rename to standard path
    if [ -d "/opt/${PROTON_VERSION}" ]; then
        rm -rf "$PROTON_DIR"
        mv "/opt/${PROTON_VERSION}" "$PROTON_DIR"
    fi

    # Verify
    if [ -x "${PROTON_DIR}/files/bin/wine64" ]; then
        ok "Proton-GE extracted successfully"
        "${PROTON_DIR}/files/bin/wine64" --version
        echo ""
        echo "Key binaries:"
        ls -la "${PROTON_DIR}/files/bin/" | grep -E 'wine|wineserver' | head -10
    else
        fail "wine64 not found after extraction!"
        ls -la "${PROTON_DIR}/" 2>/dev/null || echo "${PROTON_DIR} does not exist"
        return 1
    fi

    # Clean up tarball
    rm -f "$TARBALL"
    ok "Cleaned up tarball"
}

# ============================================================
# Check X11 Server (libXlorie — native ARM64, started by Android app)
# ============================================================
check_xserver() {
    echo "=== X11 Server Check ==="
    echo "libXlorie runs natively on Android (ARM64), NOT inside FEX."
    echo "Press 'Start X' button in the app to start it."
    echo ""
    if (echo > /dev/tcp/localhost/6000) 2>/dev/null; then
        ok "X11 server listening on TCP port 6000 (display :0)"
    else
        warn "X11 server not detected. Press 'Start X' button in the app."
    fi
}

# ============================================================
# Remove System Wine 6.0.3 (interferes with Proton-GE)
# ============================================================
clean_system_wine() {
    echo "=== Replacing System Wine with Proton-GE (relative symlinks) ==="
    # CRITICAL: Symlinks MUST be relative — absolute ones escape FEX rootfs overlay!

    if [ -d "/usr/lib/x86_64-linux-gnu/wine" ] && [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
        mv /usr/lib/x86_64-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine.system.bak
        ok "Backed up system Wine modules"
    fi
    if [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
        ln -sf ../../../opt/proton-ge/files/lib/wine /usr/lib/x86_64-linux-gnu/wine
    fi
    ok "Symlink: /usr/lib/x86_64-linux-gnu/wine -> ../../../opt/proton-ge/files/lib/wine"

    if [ -d "/usr/lib/wine" ] && [ ! -L "/usr/lib/wine" ]; then
        mv /usr/lib/wine /usr/lib/wine.system.bak
    fi
    rm -f /usr/lib/wine 2>/dev/null
    ln -sf ../../opt/proton-ge/files/lib/wine /usr/lib/wine
    ok "Symlink: /usr/lib/wine -> ../../opt/proton-ge/files/lib/wine"

    # Verify symlinks resolve correctly
    ls /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/kernel32.dll >/dev/null 2>&1 && ok "kernel32.dll reachable via symlink" || warn "kernel32.dll NOT reachable via symlink"

    # Remove system Wine binaries
    for bin in wine wine64 wineserver wineboot winecfg msiexec regedit regsvr32; do
        [ -f "/usr/bin/$bin" ] && rm -f "/usr/bin/$bin"
    done
    ok "System Wine binaries removed from /usr/bin/"
}

# ============================================================
# Initialize Wine Prefix
# ============================================================
init_wineboot() {
    if [ ! -x "${PROTON_DIR}/files/bin/wine64" ]; then
        fail "Proton-GE not installed. Run setup_proton.sh first"
        return 1
    fi

    export WINEPREFIX
    export PATH="${PROTON_DIR}/files/bin:${PATH}"
    export WINEDLLPATH="${PROTON_DIR}/files/lib/wine/x86_64-unix:${PROTON_DIR}/files/lib/wine/x86_64-windows:${PROTON_DIR}/files/lib/wine/i386-unix:${PROTON_DIR}/files/lib/wine/i386-windows"
    export WINELOADER="${PROTON_DIR}/files/bin/wine"
    export WINESERVER="${PROTON_DIR}/files/bin/wineserver"
    export DISPLAY=localhost:0
    # esync ENABLED — Android kernel supports eventfd, FEX passes syscalls through
    export PROTON_NO_FSYNC=1
    export LD_LIBRARY_PATH="${PROTON_DIR}/files/lib/wine/x86_64-unix:${PROTON_DIR}/files/lib:${LD_LIBRARY_PATH:-}"

    echo "=== Initializing Wine Prefix ==="
    echo "WINEPREFIX=${WINEPREFIX}"
    echo ""

    # Remove stale prefix (may have been created with wrong Wine)
    if [ -d "$WINEPREFIX" ]; then
        echo "Removing stale Wine prefix..."
        rm -rf "$WINEPREFIX"
    fi

    mkdir -p "$WINEPREFIX"

    echo "Running wineboot -u (may take 1-2 minutes)..."
    wine64 wineboot -u 2>&1

    if [ -d "${WINEPREFIX}/drive_c" ]; then
        ok "Wine prefix initialized"
        echo ""
        echo "drive_c contents:"
        ls "${WINEPREFIX}/drive_c/"
        echo ""
        echo "system32 DLLs:"
        ls "${WINEPREFIX}/drive_c/windows/system32/" 2>/dev/null | wc -l
        echo "files in system32"
    else
        fail "Wine prefix creation failed"
        return 1
    fi
}

# ============================================================
# Main
# ============================================================
case "${1:-}" in
    --deps)
        check_deps
        ;;
    --extract)
        extract_proton
        ;;
    --x11)
        check_xserver
        ;;
    --wineboot)
        check_xserver
        init_wineboot
        ;;
    --clean-wine)
        clean_system_wine
        ;;
    *)
        echo "============================================"
        echo "  Proton-GE Setup for FEX-Emu on Android"
        echo "============================================"
        echo ""

        check_deps
        echo ""
        echo "---"
        echo ""

        download_proton
        echo ""

        extract_proton
        echo ""

        clean_system_wine
        echo ""

        check_xserver
        echo ""

        init_wineboot
        echo ""

        echo "============================================"
        echo "  Setup Complete!"
        echo "============================================"
        echo ""
        echo "To launch a game:"
        echo "  1. Press 'Start X' button in the app (starts libXlorie)"
        echo "  export DISPLAY=localhost:0"
        echo "  export WINEPREFIX=/home/user/.wine"
        echo "  export PATH=${PROTON_DIR}/files/bin:\$PATH"
        echo "  export LD_PRELOAD=/usr/lib/libvulkan_headless.so"
        echo "  wine64 /path/to/game.exe"
        ;;
esac
