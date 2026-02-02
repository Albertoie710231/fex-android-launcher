#!/bin/bash
# Steam Launcher Container Setup Script
# This script runs inside the proot container on first launch

set -e

echo "============================================"
echo "  Steam Launcher Container Setup"
echo "============================================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running in proot
if [ ! -f /proc/version ]; then
    log_error "This script must be run inside the proot container"
    exit 1
fi

# Create necessary directories
log_info "Creating directories..."
mkdir -p /home/user/.steam
mkdir -p /home/user/.local/share/Steam
mkdir -p /opt/steam
mkdir -p /opt/box64
mkdir -p /opt/box86
mkdir -p /tmp/.X11-unix

# Set up DNS
log_info "Configuring DNS..."
echo "nameserver 8.8.8.8" > /etc/resolv.conf
echo "nameserver 8.8.4.4" >> /etc/resolv.conf

# Update package lists
log_info "Updating package lists..."
apt-get update || log_warn "apt-get update failed, continuing..."

# Install base dependencies
log_info "Installing base dependencies..."
apt-get install -y --no-install-recommends \
    ca-certificates \
    wget \
    curl \
    gnupg \
    xz-utils \
    tar \
    || log_warn "Some packages failed to install"

# Enable multiarch for 32-bit support
log_info "Enabling multiarch..."
dpkg --add-architecture armhf 2>/dev/null || true
dpkg --add-architecture i386 2>/dev/null || true
apt-get update || true

# Install X11 libraries
log_info "Installing X11 libraries..."
apt-get install -y --no-install-recommends \
    libx11-6 \
    libx11-xcb1 \
    libxcb1 \
    libxext6 \
    libxrender1 \
    libxrandr2 \
    libxfixes3 \
    libxcursor1 \
    libxcomposite1 \
    libxdamage1 \
    libxi6 \
    libxtst6 \
    libxss1 \
    libxkbfile1 \
    libxinerama1 \
    x11-utils \
    || log_warn "Some X11 packages failed"

# Install graphics libraries
log_info "Installing graphics libraries..."
apt-get install -y --no-install-recommends \
    libgl1 \
    libglu1-mesa \
    libegl1 \
    libgles2 \
    libvulkan1 \
    mesa-vulkan-drivers \
    || log_warn "Some graphics packages failed"

# Install audio libraries
log_info "Installing audio libraries..."
apt-get install -y --no-install-recommends \
    libasound2 \
    libpulse0 \
    || log_warn "Audio packages failed"

# Install 32-bit libraries for Box86
log_info "Installing 32-bit libraries..."
apt-get install -y --no-install-recommends \
    libc6:armhf \
    libstdc++6:armhf \
    2>/dev/null || log_warn "32-bit ARM libraries not available"

# Install fonts
log_info "Installing fonts..."
apt-get install -y --no-install-recommends \
    fonts-liberation \
    fonts-dejavu-core \
    fontconfig \
    || log_warn "Font packages failed"

# Install additional libraries for Steam
log_info "Installing Steam dependencies..."
apt-get install -y --no-install-recommends \
    libglib2.0-0 \
    libgtk-3-0 \
    libcairo2 \
    libpango-1.0-0 \
    libnss3 \
    libnspr4 \
    libdbus-1-3 \
    libatk1.0-0 \
    libgdk-pixbuf2.0-0 \
    libcurl4 \
    libssl3 \
    zlib1g \
    || log_warn "Some Steam dependencies failed"

# Create user .bashrc
log_info "Creating user configuration..."
cat > /home/user/.bashrc << 'EOF'
# Steam Launcher User Configuration

export HOME=/home/user
export USER=user
export DISPLAY=:0
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
export PATH=/usr/local/bin:/usr/bin:/bin:/opt/box64:/opt/box86:$PATH

# Box64/Box86 configuration
export BOX64_LOG=1
export BOX86_LOG=1
export BOX64_LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu
export BOX86_LD_LIBRARY_PATH=/usr/lib/i386-linux-gnu:/lib/i386-linux-gnu
export BOX64_DYNAREC=1
export BOX86_DYNAREC=1

# Vulkan configuration
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/android_icd.json
export MESA_VK_WSI_PRESENT_MODE=fifo

# Steam configuration
export STEAM_RUNTIME=1
export STEAM_RUNTIME_PREFER_HOST_LIBRARIES=0
export STEAM_DISABLE_BROWSER_SANDBOXING=1
export LD_PRELOAD=""

# Aliases
alias ll='ls -la'
alias steam='box86 /opt/steam/steam.sh'
alias steam64='box64 ~/.steam/steam/ubuntu12_64/steamwebhelper'

echo "Welcome to Steam Launcher Container"
echo "Run 'steam' to launch Steam"
EOF

# Set up Vulkan ICD
log_info "Configuring Vulkan..."
mkdir -p /usr/share/vulkan/icd.d
cat > /usr/share/vulkan/icd.d/android_icd.json << 'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "/system/lib64/libvulkan.so",
        "api_version": "1.3.0"
    }
}
EOF

# Create Steam launch wrapper
log_info "Creating Steam launcher..."
cat > /usr/local/bin/steam << 'EOF'
#!/bin/bash
# Steam Launch Wrapper

export STEAM_RUNTIME=1
export STEAM_RUNTIME_PREFER_HOST_LIBRARIES=0
export STEAM_DISABLE_BROWSER_SANDBOXING=1
export STEAM_CHROMIUM_GPU_RENDERING=1
export LD_PRELOAD=""

# Check if Steam is installed
if [ -f /opt/steam/steam.sh ]; then
    exec box86 /opt/steam/steam.sh "$@"
elif [ -f ~/.steam/steam/steam.sh ]; then
    exec box86 ~/.steam/steam/steam.sh "$@"
else
    echo "Steam not installed. Run /opt/scripts/install_steam.sh first"
    exit 1
fi
EOF
chmod +x /usr/local/bin/steam

# Create Box64/Box86 check script
log_info "Creating diagnostic scripts..."
cat > /usr/local/bin/check-box << 'EOF'
#!/bin/bash
echo "=== Box64/Box86 Status ==="
echo ""
echo "Box64:"
if command -v box64 &> /dev/null; then
    box64 --version 2>&1 | head -1
    echo "Location: $(which box64)"
else
    echo "NOT INSTALLED"
fi
echo ""
echo "Box86:"
if command -v box86 &> /dev/null; then
    box86 --version 2>&1 | head -1
    echo "Location: $(which box86)"
else
    echo "NOT INSTALLED"
fi
echo ""
echo "=== Environment ==="
echo "DISPLAY: $DISPLAY"
echo "BOX64_DYNAREC: $BOX64_DYNAREC"
echo "BOX86_DYNAREC: $BOX86_DYNAREC"
EOF
chmod +x /usr/local/bin/check-box

# Create Vulkan test script
cat > /usr/local/bin/check-vulkan << 'EOF'
#!/bin/bash
echo "=== Vulkan Status ==="
echo ""
echo "ICD Files:"
ls -la /usr/share/vulkan/icd.d/ 2>/dev/null || echo "No ICD directory"
echo ""
echo "Environment:"
echo "VK_ICD_FILENAMES: $VK_ICD_FILENAMES"
echo ""
if command -v vulkaninfo &> /dev/null; then
    echo "vulkaninfo output:"
    vulkaninfo --summary 2>&1
else
    echo "vulkaninfo not installed"
    echo "Install with: apt install vulkan-tools"
fi
EOF
chmod +x /usr/local/bin/check-vulkan

# Set permissions
log_info "Setting permissions..."
chown -R 1000:1000 /home/user 2>/dev/null || true
chmod 755 /home/user

# Generate locales
log_info "Generating locales..."
if [ -f /etc/locale.gen ]; then
    echo "en_US.UTF-8 UTF-8" >> /etc/locale.gen
    locale-gen 2>/dev/null || log_warn "locale-gen failed"
fi

# Cleanup
log_info "Cleaning up..."
apt-get clean 2>/dev/null || true
rm -rf /var/lib/apt/lists/* 2>/dev/null || true

# Mark setup as complete
touch /opt/.setup_complete

echo ""
echo "============================================"
echo "  Setup Complete!"
echo "============================================"
echo ""
echo "Next steps:"
echo "1. Install Box64: Run /opt/scripts/install_box64.sh"
echo "2. Install Box86: Run /opt/scripts/install_box86.sh"
echo "3. Install Steam: Run /opt/scripts/install_steam.sh"
echo ""
echo "Or run 'steam' after installation to launch Steam"
