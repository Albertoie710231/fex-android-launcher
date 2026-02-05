# MediaTek Steam Gaming App

A Kotlin Android app that runs native Linux Steam with Proton on MediaTek Dimensity tablets using Vortek Vulkan IPC passthrough, proot isolation, and x86 emulation.

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
│                 Vortek Renderer (Android)                    │
│  ┌─────────────────────────────────────────────────────────┐│
│  │ libvortekrenderer.so                                    ││
│  │  - Receives Vulkan commands via Unix socket             ││
│  │  - Executes on Mali GPU (/vendor/lib64/hw/vulkan.mali)  ││
│  │  - Renders to HardwareBuffer                            ││
│  └─────────────────────────────────────────────────────────┘│
│  ┌─────────────────────────────────────────────────────────┐│
│  │ FramebufferBridge                                       ││
│  │  - Blits HardwareBuffer → Android Surface               ││
│  │  - Choreographer vsync (60 FPS)                         ││
│  └─────────────────────────────────────────────────────────┘│
├─────────────────────────────────────────────────────────────┤
│                    PRoot Container                           │
│  ┌─────────────────────────────────────────────────────────┐│
│  │ Ubuntu 22.04 RootFS                                     ││
│  │  ├── libvulkan_vortek.so (Vulkan ICD)                  ││
│  │  │   └── Serializes Vulkan calls → sends via socket    ││
│  │  ├── Box64/FEX (x86 → ARM64 translation)               ││
│  │  ├── Steam Client                                       ││
│  │  ├── DXVK (Direct3D → Vulkan)                          ││
│  │  └── Proton (Wine-based Windows compat)                 ││
│  └─────────────────────────────────────────────────────────┘│
├─────────────────────────────────────────────────────────────┤
│              Android GPU (Mali G710/G720)                    │
└─────────────────────────────────────────────────────────────┘
```

## Vortek: How Mali GPU Works

The key innovation is **Vortek IPC passthrough** - bridging glibc (container) and Bionic (Android):

```
Container (glibc)              Android (Bionic)
┌─────────────────┐            ┌─────────────────┐
│ Game/Steam      │            │                 │
│      ↓          │            │                 │
│ DXVK (D3D→VK)   │            │                 │
│      ↓          │            │                 │
│ libvulkan_vortek│───socket──→│ VortekRenderer  │
│ (serialize API) │  + ashmem  │ (execute on GPU)│
└─────────────────┘            └────────┬────────┘
                                        ↓
                               vulkan.mali.so
                                        ↓
                                   Mali GPU
```

This bypasses the glibc/Bionic binary incompatibility that prevents loading Android's Vulkan driver directly from the container.

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
│   │   ├── SteamLauncherApp.kt     # Application class
│   │   ├── MainActivity.kt          # Main launcher UI
│   │   ├── SteamService.kt          # Foreground service
│   │   ├── ContainerManager.kt      # RootFS management
│   │   ├── ProotExecutor.kt         # PRoot JNI wrapper
│   │   ├── VortekRenderer.kt        # Vortek Vulkan wrapper
│   │   ├── FramebufferBridge.kt     # HardwareBuffer → Surface
│   │   ├── FrameSocketServer.kt     # TCP frame receiver
│   │   ├── X11Server.kt             # X11 server lifecycle
│   │   ├── X11SocketHelper.kt       # Unix socket helper
│   │   └── ...
│   ├── cpp/
│   │   ├── steamlauncher.cpp        # JNI bridge
│   │   ├── framebuffer_bridge.cpp   # HardwareBuffer JNI
│   │   ├── x11_socket.cpp           # Unix socket JNI
│   │   ├── vulkan_headless_wrapper.c # Headless surface wrapper
│   │   └── lorie/                   # X11 server implementation
│   ├── jniLibs/arm64-v8a/
│   │   ├── libvortekrenderer.so     # Android Vulkan executor
│   │   ├── libhook_impl.so          # Mali driver hook
│   │   └── ...
│   ├── assets/
│   │   ├── libvulkan_vortek.so      # Container Vulkan ICD
│   │   ├── libvulkan_headless.so    # Headless surface wrapper
│   │   ├── setup.sh                 # Container setup script
│   │   └── vkcube, vulkaninfo       # Test binaries
│   └── res/
│       └── layout/                  # UI layouts
└── build.gradle.kts
```

## Key Components

### SteamService
Foreground service that keeps the proot container alive. Required for Android 12+ to prevent the system from killing background processes.

### ContainerManager
Manages the Linux rootfs lifecycle:
- Downloads Ubuntu 22.04 arm64 base
- Configures X11, Vulkan, and Steam dependencies
- Installs x86 emulator (Box64/FEX)
- Extracts Steam bootstrap from .deb

### VortekRenderer
Android-side Vulkan execution:
- `libvortekrenderer.so` - receives serialized Vulkan commands
- Executes on Mali GPU via `/vendor/lib64/hw/vulkan.mali.so`
- Renders to HardwareBuffer (GPU memory)

### FramebufferBridge
GPU frame delivery to screen:
- Wraps HardwareBuffer as Bitmap (GPU-backed, zero-copy)
- Blits to Android Surface via Canvas
- Choreographer-based vsync (60 FPS target)

### libvulkan_vortek.so (Container ICD)
Container-side Vulkan serialization:
- Implements full Vulkan API for container apps
- Serializes all Vulkan commands
- Sends via Unix socket to VortekRenderer

### X11Server / LorieView
Provides X11 display server functionality:
- Renders X11 content to Android Surface
- Translates Android touch events to X11 mouse events
- Handles keyboard input

### ProotExecutor
Wraps proot execution:
- Binds /dev for device access
- Binds X11 and Vortek sockets
- Configures environment for x86 emulation

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

### Steam crashes with "semaphore creation failed"
This is a **Box64 limitation on Android** - Box64 wraps pthread to Bionic, which rejects semaphores.
- **Solution:** Migrate to FEX-Emu (see TODO.md Phase 2)
- This is NOT fixable by patching PRoot

### Vulkan/vkcube not rendering
1. Check Vortek ICD is configured:
   ```bash
   cat /usr/share/vulkan/icd.d/vortek_icd.json
   export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
   ```
2. Check VortekRenderer is running (Android logs)
3. Run `vulkaninfo --summary` - should show Mali GPU
4. For headless apps, ensure `LD_PRELOAD=/lib/libvulkan_headless.so`

### Black screen / no rendering
- For Vulkan apps: Check FramebufferBridge is receiving frames
- For X11 apps: Verify X11 socket exists `/tmp/.X11-unix/X0`
- Check DISPLAY is set: `echo $DISPLAY` should show `:0`

### Low FPS / stuttering
- vkcube renders at 15-40 FPS (variable render time in PRoot)
- Display runs at 60 FPS via Choreographer
- Frame pacing optimization is WIP

### App killed by Android
- Run the phantom process fix commands via ADB
- Ensure the foreground service notification is showing
- Check Settings > Battery > Steam Launcher is unrestricted

## License

MIT License

## Credits

- [Winlator](https://github.com/brunodev85/winlator) - Vortek Vulkan IPC passthrough
- [Box64](https://github.com/ptitSeb/box64) - x86_64 emulation (with BOX32 support)
- [FEX-Emu](https://github.com/FEX-Emu/FEX) - x86 emulation with glibc emulation (planned)
- [PRoot](https://github.com/termux/proot) - User-space chroot (Termux fork)
- [Termux:X11](https://github.com/termux/termux-x11) - X11 server inspiration

## Technical Notes

### Vortek IPC Architecture
Vortek solves the fundamental problem that Android's Vulkan driver (vulkan.mali.so) uses Bionic libc, but container apps use glibc. You cannot load a Bionic library from glibc code.

**Solution:** Serialize Vulkan API calls in the container, send via Unix socket + ashmem ring buffers (4MB commands, 256KB results), deserialize and execute in Android process.

**Components:**
- `libvulkan_vortek.so` (glibc, container) - Vulkan ICD that serializes API calls
- `libvortekrenderer.so` (Bionic, Android) - Executes on real Mali GPU
- `libhook_impl.so` - Intercepts vulkan.mali.so loading

### VK_EXT_headless_surface
The `vulkan_headless_wrapper.c` adds headless surface support for apps that don't need X11/XCB:
- Wraps libvulkan_vortek.so
- Adds VK_EXT_headless_surface extension
- Used by vkcube for windowless rendering

### FramebufferBridge
Zero-copy GPU frame display:
1. VortekRenderer renders to AHardwareBuffer (GPU memory)
2. FramebufferBridge wraps as Bitmap (GPU-backed)
3. Blits to Android Surface via Canvas
4. Choreographer syncs to 60 FPS vsync

### x86 Emulation (Current Blocker)
**Box64** wraps pthread to Android Bionic → semaphores fail on Android kernel.

**FEX-Emu** (planned) emulates x86 glibc → semaphores use futex → Android supports this.
