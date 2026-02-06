#!/bin/bash
# FEX-Emu setup script for Steam on Android
# Downloads and installs FEX binaries, then sets up x86-64 rootfs
set -e

echo "========================================"
echo "  FEX-Emu Setup for Steam on Android"
echo "========================================"

# Check if running in rootfs
if [ ! -f /etc/os-release ]; then
    echo "ERROR: This script must be run inside the Ubuntu rootfs"
    exit 1
fi

# Ensure HOME is set
export HOME="${HOME:-/home/user}"
export PATH="/opt/fex/bin:/usr/local/bin:/usr/bin:/bin:$HOME/.local/bin:$PATH"

# Create directories only if they don't exist (avoids proot issues)
[ -d "$HOME/.local/bin" ] || mkdir -p "$HOME/.local/bin" 2>/dev/null || true
[ -d "/usr/local/bin" ] || mkdir -p /usr/local/bin 2>/dev/null || true

# ============================================
# Step 1: Install FEX binaries to /opt/fex
# ============================================
echo ""
echo "=== Step 1: Installing FEX binaries ==="

# FEX binaries are built with interpreter path /opt/fex/lib/ld-linux-aarch64.so.1
# so they MUST be installed to /opt/fex/ for the bundled glibc to be found
FEX_DIR="/opt/fex"
[ -d "$FEX_DIR" ] || mkdir -p "$FEX_DIR" 2>/dev/null || true

# Check if FEX is already installed with bundled libs
if [ -x "$FEX_DIR/bin/FEXInterpreter" ] && [ -f "$FEX_DIR/lib/aarch64-linux-gnu/libc.so.6" ]; then
    echo "FEX already installed at $FEX_DIR"
else
    echo "Extracting FEX binaries from assets..."

    # Extract from bundled assets
    # We use .tgz extension to prevent Android AAPT from decompressing
    # Also check .tar.gz and .tar as fallbacks
    FEX_ARCHIVE=""
    FEX_TAR_OPTS=""
    for f in /assets/fex-bin.tgz /assets/fex-bin.tar.gz /assets/fex-bin.tar; do
        if [ -f "$f" ]; then
            FEX_ARCHIVE="$f"
            break
        fi
    done

    if [ -z "$FEX_ARCHIVE" ]; then
        echo "ERROR: FEX binaries not found in /assets/"
        ls -la /assets/ 2>/dev/null || echo "Cannot list /assets/"
        exit 1
    fi

    echo "Extracting from: $FEX_ARCHIVE"
    # Try gzip first, fallback to plain tar (in case AAPT decompressed it)
    tar -xzf "$FEX_ARCHIVE" -C "$FEX_DIR" 2>/dev/null || \
    tar -xf "$FEX_ARCHIVE" -C "$FEX_DIR" 2>/dev/null || {
        echo "ERROR: Failed to extract FEX from $FEX_ARCHIVE"
        exit 1
    }

    # Make binaries executable
    chmod +x "$FEX_DIR/bin/"* 2>/dev/null || true
    # Libraries need execute permission for the dynamic linker
    chmod +x "$FEX_DIR/lib/"*.so* 2>/dev/null || true
    chmod +x "$FEX_DIR/lib/aarch64-linux-gnu/"*.so* 2>/dev/null || true

    # Verify installation
    if [ -x "$FEX_DIR/bin/FEXInterpreter" ]; then
        echo "FEX installed at: $FEX_DIR"
        ls -la "$FEX_DIR/bin/" 2>/dev/null
        echo ""
        echo "Bundled libraries:"
        ls -la "$FEX_DIR/lib/aarch64-linux-gnu/"*.so* 2>/dev/null || echo "  (none found)"
    else
        echo "ERROR: FEXInterpreter not found after extraction"
        echo "Contents of $FEX_DIR:"
        find "$FEX_DIR" -type f 2>/dev/null | head -20
        exit 1
    fi
fi

# Create symlinks in standard PATH locations
ln -sf "$FEX_DIR/bin/FEXInterpreter" /usr/local/bin/FEXInterpreter 2>/dev/null || \
    ln -sf "$FEX_DIR/bin/FEXInterpreter" "$HOME/.local/bin/FEXInterpreter" 2>/dev/null || true
ln -sf "$FEX_DIR/bin/FEXLoader" /usr/local/bin/FEXLoader 2>/dev/null || true
ln -sf "$FEX_DIR/bin/FEXServer" /usr/local/bin/FEXServer 2>/dev/null || true
ln -sf "$FEX_DIR/bin/FEXRootFSFetcher" /usr/local/bin/FEXRootFSFetcher 2>/dev/null || true

# ============================================
# Step 2: Test FEX
# ============================================
echo ""
echo "=== Step 2: Testing FEX ==="

export USE_HEAP=1
export FEX_DISABLETELEMETRY=1

echo "Running FEX test..."
FEX_VERSION=$("$FEX_DIR/bin/FEXLoader" --version 2>&1)
if [ $? -eq 0 ] && echo "$FEX_VERSION" | grep -q "FEX"; then
    echo "SUCCESS: $FEX_VERSION"
else
    echo ""
    echo "WARNING: FEX failed to start!"
    echo "Output: $FEX_VERSION"
    echo ""
    echo "Checking library resolution..."
    ls -la "$FEX_DIR/lib/ld-linux-aarch64.so.1" 2>/dev/null || echo "  /opt/fex/lib/ld-linux-aarch64.so.1 missing!"
    ls -la "$FEX_DIR/lib/" 2>/dev/null || echo "  /opt/fex/lib/ missing!"
    echo ""
    echo "If you see 'Illegal instruction' or SIGILL, FEX is not compatible"
    echo "with this device's CPU/kernel combination."
    exit 1
fi

# ============================================
# Step 3: Download x86-64 RootFS
# ============================================
echo ""
echo "=== Step 3: Setting up x86-64 RootFS ==="

FEX_ROOTFS_DIR="$HOME/.fex-emu/RootFS"
ROOTFS_NAME="Ubuntu_22_04"
[ -d "$FEX_ROOTFS_DIR" ] || mkdir -p "$FEX_ROOTFS_DIR" 2>/dev/null || true

# FEX rootfs can be a directory or a SquashFS/EroFS image file
ROOTFS_SQSH="$FEX_ROOTFS_DIR/${ROOTFS_NAME}.sqsh"
ROOTFS_ERO="$FEX_ROOTFS_DIR/${ROOTFS_NAME}.ero"

if [ -f "$ROOTFS_SQSH" ] || [ -f "$ROOTFS_ERO" ]; then
    echo "x86-64 RootFS image already exists"
    ls -la "$FEX_ROOTFS_DIR/"${ROOTFS_NAME}.* 2>/dev/null
elif [ -d "$FEX_ROOTFS_DIR/$ROOTFS_NAME" ] && [ -f "$FEX_ROOTFS_DIR/$ROOTFS_NAME/usr/lib/x86_64-linux-gnu/libc.so.6" ]; then
    echo "x86-64 RootFS directory already exists at $FEX_ROOTFS_DIR/$ROOTFS_NAME"
else
    echo "Downloading x86-64 RootFS SquashFS image (~300MB)..."
    echo "This may take several minutes depending on your connection."

    # Install curl if not available
    if ! command -v curl &> /dev/null; then
        echo "Installing curl..."
        apt-get update -qq 2>/dev/null
        apt-get install -y -qq curl ca-certificates 2>&1 || true
    fi

    # Use the official FEX rootfs server (SquashFS format)
    ROOTFS_URL="https://rootfs.fex-emu.gg/Ubuntu_22_04/2025-01-08/Ubuntu_22_04.sqsh"

    echo "Downloading from: $ROOTFS_URL"
    if command -v curl &> /dev/null; then
        curl -C - -L --progress-bar -f -o "$ROOTFS_SQSH" "$ROOTFS_URL"
    elif command -v wget &> /dev/null; then
        wget -c --show-progress -O "$ROOTFS_SQSH" "$ROOTFS_URL"
    else
        echo "ERROR: Neither curl nor wget available after install attempt"
        echo "Please install curl manually: apt-get install -y curl"
        exit 1
    fi

    if [ ! -f "$ROOTFS_SQSH" ] || [ ! -s "$ROOTFS_SQSH" ]; then
        echo "ERROR: Download failed"
        rm -f "$ROOTFS_SQSH"
        exit 1
    fi

    echo "RootFS image downloaded: $(du -h "$ROOTFS_SQSH" | cut -f1)"

    # Extract SquashFS to directory (needed since squashfuse doesn't work in proot)
    echo ""
    echo "Extracting SquashFS to directory..."
    echo "This may take several minutes..."

    # Install unsquashfs if needed
    if ! command -v unsquashfs &> /dev/null; then
        apt-get install -y -qq squashfs-tools 2>&1 || true
    fi

    if command -v unsquashfs &> /dev/null; then
        unsquashfs -d "$FEX_ROOTFS_DIR/$ROOTFS_NAME" -f "$ROOTFS_SQSH" 2>&1 | tail -5
        echo "Extraction complete"
    else
        echo "ERROR: unsquashfs not available, cannot extract rootfs"
        echo "Install squashfs-tools: apt-get install -y squashfs-tools"
        exit 1
    fi
fi

# ============================================
# Step 4: Configure FEX
# ============================================
echo ""
echo "=== Step 4: Configuring FEX ==="

FEX_CONFIG_DIR="$HOME/.fex-emu"
[ -d "$FEX_CONFIG_DIR" ] || mkdir -p "$FEX_CONFIG_DIR" 2>/dev/null || true

# Main config
cat > "$FEX_CONFIG_DIR/Config.json" << EOF
{
  "Config": {
    "RootFS": "$ROOTFS_NAME",
    "ThunkConfig": "$FEX_CONFIG_DIR/thunks.json",
    "X87ReducedPrecision": "1"
  }
}
EOF

# Thunks config for GPU passthrough
if [ -f "$FEX_DIR/config/thunks.json" ]; then
    cp "$FEX_DIR/config/thunks.json" "$FEX_CONFIG_DIR/thunks.json"
fi

echo "FEX configuration created at $FEX_CONFIG_DIR/Config.json"
cat "$FEX_CONFIG_DIR/Config.json"

# ============================================
# Step 5: Create helper scripts
# ============================================
echo ""
echo "=== Step 5: Creating helper scripts ==="

# Determine where to install scripts
SCRIPT_DIR="/usr/local/bin"
if [ ! -w "$SCRIPT_DIR" ]; then
    SCRIPT_DIR="$HOME/.local/bin"
    [ -d "$SCRIPT_DIR" ] || mkdir -p "$SCRIPT_DIR" 2>/dev/null || true
fi

FEX_LOADER="$FEX_DIR/bin/FEXLoader"

# fex-shell - x86-64 shell
cat > "$SCRIPT_DIR/fex-shell" << SCRIPT
#!/bin/bash
export USE_HEAP=1
export FEX_DISABLETELEMETRY=1
export PATH="/opt/fex/bin:\$HOME/.local/bin:\$PATH"
export HOME="\${HOME:-/home/user}"
exec "$FEX_LOADER" -- /bin/bash "\$@"
SCRIPT
chmod +x "$SCRIPT_DIR/fex-shell"

# fex-run - run x86-64 commands
cat > "$SCRIPT_DIR/fex-run" << SCRIPT
#!/bin/bash
export USE_HEAP=1
export FEX_DISABLETELEMETRY=1
export PATH="/opt/fex/bin:\$HOME/.local/bin:\$PATH"
export HOME="\${HOME:-/home/user}"
exec "$FEX_LOADER" -- "\$@"
SCRIPT
chmod +x "$SCRIPT_DIR/fex-run"

echo "Helper scripts created in $SCRIPT_DIR:"
echo "  fex-shell    - Start x86-64 bash shell"
echo "  fex-run      - Run x86-64 command"

# ============================================
# Done
# ============================================
echo ""
echo "========================================"
echo "  FEX-Emu Setup Complete!"
echo "========================================"
echo ""
echo "To get an x86-64 shell:"
echo "  fex-shell"
echo ""
