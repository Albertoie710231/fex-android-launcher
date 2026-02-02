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
│  │  ├── Box86 (x86 → ARM32 translation)                   ││
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
   - Box64 (x86_64 emulation)
   - Box86 (x86 emulation)
   - Required libraries

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
- Installs Box64 and Box86

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

## Troubleshooting

### Steam crashes on launch
- Ensure Box86 is installed (Steam bootstrapper is 32-bit)
- Check that all 32-bit libraries are installed
- Disable libmimalloc: `export LD_PRELOAD=""`

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

- [Box64](https://github.com/ptitSeb/box64) - x86_64 emulation
- [Box86](https://github.com/ptitSeb/box86) - x86 emulation
- [PRoot](https://github.com/proot-me/proot) - User-space chroot
- [Termux:X11](https://github.com/termux/termux-x11) - X11 server inspiration
