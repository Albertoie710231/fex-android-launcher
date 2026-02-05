#!/bin/bash
# FEX-Emu setup script for Steam on Android
# This script installs FEX in the ARM64 Ubuntu rootfs and sets up x86-64 environment
#
# Key difference from Box64:
# - Box64: Uses native ARM64 libs, wraps pthread to Bionic → semaphores FAIL
# - FEX: Uses x86 libs from x86 rootfs, emulates glibc → semaphores WORK

set -e

echo "========================================"
echo "  FEX-Emu Setup for Steam on Android"
echo "========================================"

# Check if running in rootfs
if [ ! -f /etc/os-release ]; then
    echo "ERROR: This script must be run inside the Ubuntu rootfs"
    exit 1
fi

# ============================================
# Step 1: Add FEX PPA and install
# ============================================
echo ""
echo "=== Step 1: Installing FEX-Emu ==="

# Check if already installed
if command -v FEXInterpreter &> /dev/null || command -v FEX &> /dev/null; then
    FEX_BIN=$(command -v FEX 2>/dev/null || command -v FEXInterpreter 2>/dev/null)
    echo "FEX already installed at: $FEX_BIN"
else
    # Add PPA
    if ! grep -q "fex-emu/fex" /etc/apt/sources.list.d/*.list 2>/dev/null; then
        echo "Adding FEX PPA..."
        apt-get update -qq
        apt-get install -y software-properties-common gpg
        add-apt-repository -y ppa:fex-emu/fex
        apt-get update -qq
    fi

    # Detect ARM version and install appropriate package
    # MediaTek Dimensity chips are ARMv8.2+
    echo "Installing FEX for ARMv8.2..."
    apt-get install -y fex-emu-armv8.2 || {
        echo "ARMv8.2 failed, trying ARMv8.0..."
        apt-get install -y fex-emu-armv8.0
    }

    # Verify installation
    if ! command -v FEXInterpreter &> /dev/null && ! command -v FEX &> /dev/null; then
        echo "ERROR: FEX installation failed"
        exit 1
    fi

    FEX_BIN=$(command -v FEX 2>/dev/null || command -v FEXInterpreter 2>/dev/null)
    echo "FEX installed at: $FEX_BIN"
fi

# ============================================
# Step 2: Download x86-64 RootFS
# ============================================
echo ""
echo "=== Step 2: Setting up x86-64 RootFS ==="

FEX_ROOTFS_DIR="$HOME/.fex-emu/RootFS"
ROOTFS_NAME="Ubuntu_22_04"
mkdir -p "$FEX_ROOTFS_DIR"

if [ -d "$FEX_ROOTFS_DIR/$ROOTFS_NAME" ]; then
    echo "x86-64 RootFS already exists at $FEX_ROOTFS_DIR/$ROOTFS_NAME"
else
    echo "Downloading x86-64 RootFS (~2GB)..."

    # Use FEXRootFSFetcher if available
    if command -v FEXRootFSFetcher &> /dev/null; then
        echo "Using FEXRootFSFetcher..."
        # Run non-interactively - select Ubuntu 22.04
        echo "1" | FEXRootFSFetcher || {
            echo "FEXRootFSFetcher failed, trying manual download..."
        }
    fi

    # Check if download succeeded
    if [ ! -d "$FEX_ROOTFS_DIR/$ROOTFS_NAME" ]; then
        echo ""
        echo "Attempting manual download..."

        # Try direct download from FEX rootfs server
        ROOTFS_URL="https://rootfs.fex-emu.com/file/fex-rootfs/Ubuntu_22_04.tar.gz"
        ROOTFS_TAR="$FEX_ROOTFS_DIR/${ROOTFS_NAME}.tar.gz"

        if [ ! -f "$ROOTFS_TAR" ]; then
            echo "Downloading from $ROOTFS_URL..."
            curl -L -o "$ROOTFS_TAR" "$ROOTFS_URL" || wget -O "$ROOTFS_TAR" "$ROOTFS_URL" || {
                echo ""
                echo "ERROR: Could not download rootfs automatically."
                echo "Please download manually from: https://rootfs.fex-emu.com/"
                echo "Extract to: $FEX_ROOTFS_DIR/$ROOTFS_NAME/"
                exit 1
            }
        fi

        # Extract
        echo "Extracting rootfs..."
        mkdir -p "$FEX_ROOTFS_DIR/$ROOTFS_NAME"
        tar -xzf "$ROOTFS_TAR" -C "$FEX_ROOTFS_DIR/$ROOTFS_NAME" --strip-components=1 || {
            # Try without strip-components
            tar -xzf "$ROOTFS_TAR" -C "$FEX_ROOTFS_DIR/"
        }

        # Cleanup
        rm -f "$ROOTFS_TAR"
    fi

    echo "x86-64 RootFS installed at $FEX_ROOTFS_DIR/$ROOTFS_NAME"
fi

# ============================================
# Step 3: Install Steam in x86-64 RootFS
# ============================================
echo ""
echo "=== Step 3: Installing Steam in x86-64 RootFS ==="

X86_ROOTFS="$FEX_ROOTFS_DIR/$ROOTFS_NAME"

# Check if Steam is already installed in x86 rootfs
if [ -f "$X86_ROOTFS/usr/bin/steam" ] || [ -f "$X86_ROOTFS/home/user/.steam/steam/steam.sh" ]; then
    echo "Steam already installed in x86-64 rootfs"
else
    echo "Installing Steam in x86-64 rootfs..."

    # Create install script for x86 rootfs
    cat > /tmp/install_steam_x86.sh << 'STEAMSCRIPT'
#!/bin/bash
set -e
dpkg --add-architecture i386
apt-get update
apt-get install -y steam-installer || {
    # Manual install if steam-installer not available
    apt-get install -y curl
    cd /tmp
    curl -L -o steam.deb https://cdn.cloudflare.steamstatic.com/client/installer/steam.deb
    dpkg -i steam.deb || apt-get install -f -y
    rm steam.deb
}
echo "Steam installed"
STEAMSCRIPT
    chmod +x /tmp/install_steam_x86.sh

    # Run in FEX
    echo "Running Steam install via FEX (this may take a while)..."
    FEX /tmp/install_steam_x86.sh || {
        echo "Note: Steam installation via FEX may need manual completion"
    }
fi

# ============================================
# Step 4: Configure FEX
# ============================================
echo ""
echo "=== Step 4: Configuring FEX ==="

FEX_CONFIG_DIR="$HOME/.fex-emu"
mkdir -p "$FEX_CONFIG_DIR"

# Main config
cat > "$FEX_CONFIG_DIR/Config.json" << EOF
{
  "Config": {
    "RootFS": "$ROOTFS_NAME"
  }
}
EOF

# Thunks config for Vortek Vulkan (forward Vulkan calls to our native ICD)
mkdir -p "$FEX_CONFIG_DIR/AppConfig"
cat > "$FEX_CONFIG_DIR/AppConfig/steam.json" << 'EOF'
{
  "Env": {
    "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d/vortek_icd.json",
    "STEAM_RUNTIME": "1"
  }
}
EOF

echo "FEX configuration created at $FEX_CONFIG_DIR/"

# ============================================
# Step 5: Create wrapper scripts
# ============================================
echo ""
echo "=== Step 5: Creating launcher wrappers ==="

# FEX-Steam launcher
cat > /usr/local/bin/fex-steam << 'EOF'
#!/bin/bash
# FEX wrapper for Steam
# Uses x86-64 rootfs with glibc emulation (semaphores work!)

# FEX config
export FEX_ROOTFS="$HOME/.fex-emu/RootFS/Ubuntu_22_04"

# Vulkan via Vortek
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json

# Display
export DISPLAY=${DISPLAY:-:0}

# Find FEX binary
FEX_BIN=$(command -v FEX 2>/dev/null || command -v FEXInterpreter 2>/dev/null)
if [ -z "$FEX_BIN" ]; then
    echo "ERROR: FEX not found"
    exit 1
fi

# Run Steam
echo "Starting Steam via FEX..."
exec "$FEX_BIN" -- /usr/bin/steam "$@"
EOF
chmod +x /usr/local/bin/fex-steam

# FEX shell for debugging
cat > /usr/local/bin/fex-shell << 'EOF'
#!/bin/bash
# FEX x86-64 shell for debugging

export FEX_ROOTFS="$HOME/.fex-emu/RootFS/Ubuntu_22_04"

FEX_BIN=$(command -v FEX 2>/dev/null || command -v FEXInterpreter 2>/dev/null)
if [ -z "$FEX_BIN" ]; then
    echo "ERROR: FEX not found"
    exit 1
fi

echo "Starting x86-64 shell via FEX..."
exec "$FEX_BIN" -- /bin/bash "$@"
EOF
chmod +x /usr/local/bin/fex-shell

# Semaphore test
cat > /usr/local/bin/fex-test-sem << 'EOF'
#!/bin/bash
# Test semaphores work under FEX

export FEX_ROOTFS="$HOME/.fex-emu/RootFS/Ubuntu_22_04"

FEX_BIN=$(command -v FEX 2>/dev/null || command -v FEXInterpreter 2>/dev/null)

# Create test program
cat > /tmp/sem_test.c << 'CCODE'
#include <semaphore.h>
#include <stdio.h>
int main() {
    sem_t sem;
    if (sem_init(&sem, 0, 1) == -1) {
        perror("sem_init FAILED");
        return 1;
    }
    printf("Semaphore test PASSED!\n");
    sem_destroy(&sem);
    return 0;
}
CCODE

echo "Compiling semaphore test (x86-64)..."
"$FEX_BIN" -- gcc -o /tmp/sem_test /tmp/sem_test.c -lpthread

echo "Running semaphore test via FEX..."
"$FEX_BIN" -- /tmp/sem_test
EOF
chmod +x /usr/local/bin/fex-test-sem

echo ""
echo "========================================"
echo "  FEX-Emu Setup Complete!"
echo "========================================"
echo ""
echo "Commands available:"
echo "  fex-steam     - Launch Steam via FEX"
echo "  fex-shell     - Start x86-64 bash shell"
echo "  fex-test-sem  - Test semaphore support"
echo ""
echo "RootFS: $FEX_ROOTFS_DIR/$ROOTFS_NAME"
echo "Config: $FEX_CONFIG_DIR"
echo ""
echo "To test semaphores work (the main fix):"
echo "  fex-test-sem"
echo ""
