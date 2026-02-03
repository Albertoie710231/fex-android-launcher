# MediaTek Steam Gaming App

A Kotlin Android app that runs native Linux Steam with Proton on MediaTek Dimensity tablets using direct Vulkan passthrough, proot isolation, and Box64/Box86 for x86_64/x86 translation.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Android App (Kotlin)                      │
├─────────────────────────────────────────────────────────────┤
│  MainActivity          │  SteamService (Foreground)         │
│  - Game launcher UI    │  - Process management              │
│  - Settings            │  - Session lifecycle               │
│  - Container mgmt      │  - Phantom process workaround      │
├─────────────────────────────────────────────────────────────┤
│                    X11 Server (Lorie)                        │
│  - LorieView (SurfaceView) renders X11 to Android           │
│  - Touch/keyboard input translation                          │
├─────────────────────────────────────────────────────────────┤
│                    PRoot Container                           │
│  ┌─────────────────────────────────────────────────────────┐│
│  │ Ubuntu 22.04 RootFS                                     ││
│  │  ├── Box64 (x86_64 → ARM64 translation)                ││
│  │  ├── Box32 (x86 → ARM64 via Box64 with BOX32=ON)       ││
│  │  ├── Steam Client                                       ││
│  │  ├── Proton (Wine-based Windows compat)                 ││
│  │  └── Mesa + Vulkan libraries                            ││
│  └─────────────────────────────────────────────────────────┘│
├─────────────────────────────────────────────────────────────┤
│                 Vulkan Passthrough Layer                     │
│  - Direct Android Vulkan → PRoot                            │
│  - Mali GPU optimizations                                    │
├─────────────────────────────────────────────────────────────┤
│              Android GPU (Mali G710/G720)                    │
└─────────────────────────────────────────────────────────────┘
```

## Requirements

- Android device with:
  - MediaTek Dimensity SoC (9000/9200/9300 series recommended)
  - Mali G710 or newer GPU
  - Vulkan 1.1+ support
  - 8GB+ RAM recommended
  - Android 8.0+ (API 26+)
- Android Studio with:
  - Android SDK 34
  - Android NDK (latest)
  - CMake 3.22.1+

## Building

1. Open the project in Android Studio

2. Sync Gradle files

3. Build the project:
   ```bash
   ./gradlew assembleDebug
   ```

4. Install on device:
   ```bash
   ./gradlew installDebug
   ```

## First Run Setup

1. Launch the app and grant storage permissions

2. Click "Setup Container" to download and configure the Linux rootfs

3. After setup, the app will automatically install:
   - Box64 with BOX32 support (x86_64 and x86 emulation on ARM64-only devices)
   - Required x86 libraries
   - Patched PRoot with futex/semaphore support

4. Go to Settings > "Install Steam" to install Steam

5. Click "Launch Steam" to start

## Android 12+ Phantom Process Fix

Android 12 aggressively kills background processes. Run these ADB commands to prevent this:

```bash
# Disable phantom process monitoring
adb shell "settings put global settings_enable_monitor_phantom_procs false"

# Or set max phantom processes to unlimited
adb shell "/system/bin/device_config set_sync_disabled_for_tests persistent"
adb shell "/system/bin/device_config put activity_manager max_phantom_processes 2147483647"

# Verify
adb shell "/system/bin/dumpsys activity settings | grep max_phantom"
```

## Project Structure

```
app/
├── src/main/
│   ├── java/com/mediatek/steamlauncher/
│   │   ├── SteamLauncherApp.kt    # Application class
│   │   ├── MainActivity.kt         # Main launcher UI
│   │   ├── SteamService.kt         # Foreground service
│   │   ├── ContainerManager.kt     # RootFS management
│   │   ├── ProotExecutor.kt        # PRoot JNI wrapper
│   │   ├── X11Server.kt            # X11 server lifecycle
│   │   ├── LorieView.kt            # X11 rendering surface
│   │   ├── VulkanBridge.kt         # Vulkan passthrough
│   │   ├── InputHandler.kt         # Input handling
│   │   ├── GameActivity.kt         # Game display activity
│   │   └── SettingsActivity.kt     # Settings UI
│   ├── cpp/
│   │   ├── steamlauncher.cpp       # JNI bridge
│   │   ├── lorie/                  # X11 server implementation
│   │   └── vulkan_bridge/          # Vulkan configuration
│   ├── assets/
│   │   └── setup.sh                # Container setup script
│   └── res/
│       └── layout/                 # UI layouts
└── build.gradle.kts
```

## Key Components

### SteamService
Foreground service that keeps the proot container alive. Required for Android 12+ to prevent the system from killing background processes.

### ContainerManager
Manages the Linux rootfs lifecycle:
- Downloads Ubuntu 22.04 arm64 base
- Configures X11, Vulkan, and Steam dependencies
- Installs Box64 with BOX32 support (handles both 32-bit and 64-bit x86)
- Extracts Steam bootstrap from .deb (uses Java XZ library, no external xz binary needed)

### X11Server / LorieView
Provides X11 display server functionality:
- Renders X11 content to Android Surface via OpenGL ES
- Translates Android touch events to X11 mouse events
- Handles keyboard input

### VulkanBridge
Configures Vulkan passthrough:
- Creates ICD configuration for Android Vulkan driver
- Sets up Mali-specific workarounds
- Configures DXVK environment for Proton

### ProotExecutor
Wraps proot execution:
- Binds /dev/dri for GPU access
- Binds X11 socket
- Configures environment for Box64/Box86

## Testing Steam via ADB

To verify Steam runs correctly through the patched PRoot and Box32, use this command:

```bash
adb shell "run-as com.mediatek.steamlauncher sh -c '
  export NATIVELIB=\$(dirname \$(pm path com.mediatek.steamlauncher | cut -d: -f2))/lib/arm64
  export LD_LIBRARY_PATH=files/lib-override:\$NATIVELIB
  export PROOT_LOADER=\$NATIVELIB/libproot-loader.so
  export PROOT_TMP_DIR=cache/proot-tmp
  export PROOT_NO_SECCOMP=1

  timeout 15 \$NATIVELIB/libproot.so --rootfs=files/rootfs \
    -b /dev -b /proc -b /sys \
    -w /home/user \
    /bin/sh -c \"
      export HOME=/home/user
      export DISPLAY=:0
      export BOX64_LOG=1
      export BOX64_LD_LIBRARY_PATH=/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu
      export LD_LIBRARY_PATH=/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu
      export STEAM_RUNTIME=0
      /usr/local/bin/box32 /home/user/.local/share/Steam/ubuntu12_32/steam 2>&1
    \"
'"
```

Expected output (success):
```
[BOX32] Personality set to 32bits
[BOX32] Using Box32 to load 32bits elf
[BOX32] Rename process to "steam"
[BOX32] Using native(wrapped) libdl.so.2
[BOX32] Using native(wrapped) librt.so.1
[BOX32] Using native(wrapped) libm.so.6
[BOX32] Using native(wrapped) libpthread.so.0
[BOX32] Using native(wrapped) libc.so.6
[BOX32] Using native(wrapped) ld-linux.so.2
```

If you see the above output without "semaphore creation failed" errors, the patched PRoot is working correctly. Steam will wait for an X11 display server to continue.

### Fix libtalloc symlink after reinstall

After reinstalling the APK, the libtalloc symlink may break. Fix it with:

```bash
APP_PATH=$(adb shell "pm path com.mediatek.steamlauncher" | cut -d: -f2 | tr -d '\r\n')
NATIVELIB=$(dirname "$APP_PATH")/lib/arm64

adb shell "run-as com.mediatek.steamlauncher sh -c '
  rm -f files/lib-override/libtalloc.so.2
  mkdir -p files/lib-override
  ln -sf $NATIVELIB/libtalloc.so files/lib-override/libtalloc.so.2
'"
```

## Troubleshooting

### Steam crashes on launch
- Ensure Box32 symlink exists (points to Box64 with BOX32 support)
- Check that all 32-bit x86 libraries are installed in `/lib/i386-linux-gnu/`
- Disable libmimalloc: `export LD_PRELOAD=""`
- Check for "semaphore creation failed" - if present, the patched PRoot is not being used

### Black screen / no rendering
- Verify X11 socket exists: `/tmp/.X11-unix/X0`
- Check DISPLAY is set: `echo $DISPLAY` should show `:0`
- Test with `xeyes` or `glxgears`

### Vulkan not working
- Run `vulkaninfo --summary` in the container
- Check VK_ICD_FILENAMES is set correctly
- Verify /dev/dri is bound in proot

### App killed by Android
- Run the phantom process fix commands via ADB
- Ensure the foreground service notification is showing
- Check Settings > Battery > Steam Launcher is unrestricted

## License

MIT License

## Credits

- [Box64](https://github.com/ptitSeb/box64) - x86_64 emulation (with BOX32 support for 32-bit x86)
- [PRoot](https://github.com/termux/proot) - User-space chroot (Termux fork, patched for futex/semaphore support)
- [Termux:X11](https://github.com/termux/termux-x11) - X11 server inspiration

## Technical Notes

### PRoot Futex Patch
The bundled `libproot.so` includes patches for `futex_time64` syscall handling, required for Steam's semaphore operations on Android. Without this patch, Steam fails with "semaphore creation failed Function not implemented".

### Box64 with BOX32
Since many Android devices are ARM64-only (no 32-bit ARM support), we use Box64 compiled with `BOX32=ON` flag. This allows Box64 to handle both 32-bit x86 (via `box32` symlink) and 64-bit x86_64 binaries on ARM64-only devices.
