#!/bin/bash
# Setup Proton-GE for running Windows games via Wine/DXVK inside FEX-Emu.
#
# This script runs INSIDE the FEX x86-64 guest environment.
# It downloads and extracts GE-Proton10-30, sets up Xvfb, and initializes Wine.
#
# Usage:
#   ./setup_proton.sh              # Full setup (download + extract + wine init)
#   ./setup_proton.sh --extract    # Extract only (tarball already in /tmp/)
#   ./setup_proton.sh --deps       # Check dependencies only
#   ./setup_proton.sh --xvfb       # Start Xvfb only
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

    # Xvfb
    if command -v Xvfb >/dev/null 2>&1; then
        ok "Xvfb found"
    else
        warn "Xvfb MISSING (install: apt install xvfb)"
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
    df -h / 2>/dev/null | tail -1 || true

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
# Start Xvfb (TCP-only mode for Android)
# ============================================================
start_xvfb() {
    # Check if already running
    if pgrep -f 'Xvfb :99' >/dev/null 2>&1; then
        ok "Xvfb already running on :99"
        return 0
    fi

    if ! command -v Xvfb >/dev/null 2>&1; then
        fail "Xvfb not found! Install: apt install xvfb"
        return 1
    fi

    # Kill any stale instance
    pkill -f 'Xvfb :99' 2>/dev/null || true
    sleep 0.5

    # Create required directory
    mkdir -p /tmp/.X11-unix

    # Start Xvfb in TCP-only mode (Android SELinux blocks Unix sockets)
    Xvfb :99 -screen 0 1280x720x24 -ac -nolisten local -nolisten unix -listen tcp &
    XVFB_PID=$!
    sleep 1

    if kill -0 $XVFB_PID 2>/dev/null; then
        ok "Xvfb started (PID ${XVFB_PID}) on display :99 (TCP mode)"
        echo "Use: export DISPLAY=localhost:99"
    else
        fail "Xvfb failed to start"
        return 1
    fi
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
    export DISPLAY=localhost:99
    export PROTON_NO_ESYNC=1
    export PROTON_NO_FSYNC=1

    echo "=== Initializing Wine Prefix ==="
    echo "WINEPREFIX=${WINEPREFIX}"
    echo ""

    mkdir -p "$WINEPREFIX"

    echo "Running wineboot -u (may take 1-2 minutes)..."
    wineboot -u 2>&1

    if [ -d "${WINEPREFIX}/drive_c" ]; then
        ok "Wine prefix initialized"
        echo ""
        echo "drive_c contents:"
        ls "${WINEPREFIX}/drive_c/"
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
    --xvfb)
        start_xvfb
        ;;
    --wineboot)
        start_xvfb
        init_wineboot
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

        start_xvfb
        echo ""

        init_wineboot
        echo ""

        echo "============================================"
        echo "  Setup Complete!"
        echo "============================================"
        echo ""
        echo "To launch a game:"
        echo "  export DISPLAY=localhost:99"
        echo "  export WINEPREFIX=/home/user/.wine"
        echo "  export PATH=${PROTON_DIR}/files/bin:\$PATH"
        echo "  export LD_PRELOAD=/usr/lib/libvulkan_headless.so"
        echo "  wine64 /path/to/game.exe"
        ;;
esac
