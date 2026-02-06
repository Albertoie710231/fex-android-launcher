# MediaTek Steam Gaming App

A Kotlin Android app that runs native Linux Steam with Proton on MediaTek Dimensity tablets using Vortek Vulkan IPC passthrough and FEX-Emu x86-64 emulation.

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
│              FEX-Emu (Direct, no PRoot)                      │
│  ┌─────────────────────────────────────────────────────────┐│
│  │ FEXLoader (ARM64 native)                                ││
│  │  ├── x86-64 rootfs (Ubuntu 22.04 from fex-emu.gg)     ││
│  │  ├── libvulkan_vortek.so (Vulkan ICD)                  ││
│  │  │   └── Serializes Vulkan calls → sends via socket    ││
│  │  ├── Steam Client                                       ││
│  │  ├── DXVK (Direct3D → Vulkan)                          ││
│  │  └── Proton (Wine-based Windows compat)                 ││
│  └─────────────────────────────────────────────────────────┘│
├─────────────────────────────────────────────────────────────┤
│              Android GPU (Mali G710/G720)                    │
└─────────────────────────────────────────────────────────────┘
```

### Key Design: No PRoot

FEX-Emu runs directly via `ProcessBuilder` — no PRoot, no ptrace overhead:

```
App → ProcessBuilder → ld-linux-aarch64.so.1 → FEXLoader → x86-64 bash/Steam
```

FEX provides its own rootfs overlay that handles filesystem redirection:
- `/bin`, `/usr/lib`, etc. → redirected to FEX's x86-64 rootfs
- `/dev`, `/proc`, `/sys` → host devices directly (including GPU)
- `/tmp` → redirected to rootfs `/tmp/` (symlinks for Vortek/X11 sockets)

## Vortek: How Mali GPU Works

The key innovation is **Vortek IPC passthrough** - bridging glibc (container) and Bionic (Android):

```
FEX Container (glibc)          Android (Bionic)
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

1. Download vendor sources for the NDK-built `unsquashfs` tool:
   ```bash
   bash scripts/fetch_unsquashfs_source.sh
   ```

2. Build the project:
   ```bash
   ./gradlew assembleDebug
   ```

3. Install on device:
   ```bash
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   ```

## First Run Setup

1. Launch the app and grant storage permissions

2. Click **"Setup Container"** — this will:
   - Extract FEX ARM64 binaries from bundled `fex-bin.tgz`
   - Download x86-64 SquashFS rootfs (~995MB) from `rootfs.fex-emu.gg`
   - Extract rootfs with NDK-built `unsquashfs` (~2GB extracted)
   - Configure FEX (`Config.json`, thunks)
   - Setup Vortek Vulkan ICD in the rootfs
   - Download and extract Steam

3. Open Terminal and verify FEX works

4. Click **"Launch Steam"** to start Steam via FEX

## Testing FEX via ADB

### Verify FEXLoader runs

```bash
FEXDIR="/data/data/com.mediatek.steamlauncher/files/fex"
FEXHOME="/data/data/com.mediatek.steamlauncher/files/fex-home"

adb shell "run-as com.mediatek.steamlauncher sh -c '
  export HOME=$FEXHOME
  export USE_HEAP=1
  export FEX_DISABLETELEMETRY=1
  $FEXDIR/lib/ld-linux-aarch64.so.1 \
    --library-path $FEXDIR/lib:$FEXDIR/lib/aarch64-linux-gnu \
    $FEXDIR/bin/FEXLoader --version
'"
```

Expected output: `FEX-Emu (FEX-2506)`

### Start FEXServer and run x86-64 commands

FEXServer must be running for FEXLoader to execute guest binaries:

```bash
FEXDIR="/data/data/com.mediatek.steamlauncher/files/fex"
FEXHOME="/data/data/com.mediatek.steamlauncher/files/fex-home"
TMPDIR="/data/data/com.mediatek.steamlauncher/cache/tmp"

# Start FEXServer (persistent for 60s)
adb shell "run-as com.mediatek.steamlauncher sh -c '
  export HOME=$FEXHOME TMPDIR=$TMPDIR USE_HEAP=1 FEX_DISABLETELEMETRY=1
  mkdir -p $TMPDIR
  $FEXDIR/lib/ld-linux-aarch64.so.1 \
    --library-path $FEXDIR/lib:$FEXDIR/lib/aarch64-linux-gnu \
    $FEXDIR/bin/FEXServer -f -p 60 &
  sleep 2
  $FEXDIR/lib/ld-linux-aarch64.so.1 \
    --library-path $FEXDIR/lib:$FEXDIR/lib/aarch64-linux-gnu \
    $FEXDIR/bin/FEXLoader -- /bin/uname -a
'"
```

Expected output: `Linux ... x86_64 GNU/Linux`

### Test unsquashfs binary

```bash
NATIVELIB=$(dirname $(adb shell pm path com.mediatek.steamlauncher | cut -d: -f2 | tr -d '\r\n'))/lib/arm64

adb shell "run-as com.mediatek.steamlauncher $NATIVELIB/libunsquashfs.so -version"
```

Expected output: `unsquashfs version 4.6.1`

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
│   │   ├── SteamLauncherApp.kt     # Application class, path helpers
│   │   ├── MainActivity.kt          # Main launcher UI
│   │   ├── SteamService.kt          # Foreground service
│   │   ├── ContainerManager.kt      # FEX rootfs setup & management
│   │   ├── FexExecutor.kt           # FEXLoader process execution
│   │   ├── TerminalActivity.kt      # Interactive FEX terminal
│   │   ├── SettingsActivity.kt      # Settings & diagnostics
│   │   ├── VulkanBridge.kt          # Vulkan/Vortek ICD setup
│   │   ├── VortekRenderer.kt        # Vortek Vulkan wrapper
│   │   ├── FramebufferBridge.kt     # HardwareBuffer → Surface
│   │   ├── FrameSocketServer.kt     # TCP frame receiver
│   │   ├── X11Server.kt             # X11 server lifecycle
│   │   ├── X11SocketHelper.kt       # Unix socket helper
│   │   └── ...
│   ├── cpp/
│   │   ├── CMakeLists.txt           # NDK build (includes unsquashfs)
│   │   ├── steamlauncher.cpp        # JNI bridge
│   │   ├── framebuffer_bridge.cpp   # HardwareBuffer JNI
│   │   ├── x11_socket.cpp           # Unix socket JNI
│   │   └── vendor/                  # squashfs-tools & xz-utils source
│   ├── jniLibs/arm64-v8a/
│   │   ├── libvortekrenderer.so     # Android Vulkan executor
│   │   ├── libhook_impl.so         # Mali driver hook
│   │   └── ...
│   ├── assets/
│   │   ├── fex-bin.tgz             # FEX ARM64 binaries + glibc 2.38
│   │   ├── libvulkan_vortek.so      # Container Vulkan ICD
│   │   ├── libvulkan_headless.so    # Headless surface wrapper
│   │   └── vkcube, vulkaninfo       # Test binaries
│   └── res/
│       └── layout/                  # UI layouts
├── build.gradle.kts
└── scripts/
    └── fetch_unsquashfs_source.sh   # Downloads vendor source for NDK build
```

## Key Components

### FexExecutor
Runs FEXLoader directly via `ProcessBuilder`:
- Invokes FEXLoader through bundled `ld-linux-aarch64.so.1` (bypasses missing `/opt/fex/` on Android)
- Clears Android env pollution (LD_PRELOAD, ANDROID_*, etc.)
- Sets `USE_HEAP=1` to avoid SBRK allocation issues
- Creates symlinks in FEX rootfs `/tmp/` for Vortek and X11 sockets

### ContainerManager
Manages the FEX environment lifecycle:
- Extracts FEX ARM64 binaries from bundled `fex-bin.tgz`
- Downloads and extracts x86-64 SquashFS rootfs via NDK-built `unsquashfs`
- Writes FEX Config.json and thunks.json
- Sets up Vortek ICD and Steam

### SteamService
Foreground service that keeps the FEX container alive. Required for Android 12+ to prevent the system from killing background processes.

### VortekRenderer
Android-side Vulkan execution:
- `libvortekrenderer.so` - receives serialized Vulkan commands
- Executes on Mali GPU via `/vendor/lib64/hw/vulkan.mali.so`
- Renders to HardwareBuffer (GPU memory)

### FramebufferBridge
Zero-copy GPU frame display:
1. VortekRenderer renders to AHardwareBuffer (GPU memory)
2. FramebufferBridge wraps as Bitmap (GPU-backed)
3. Blits to Android Surface via Canvas
4. Choreographer syncs to 60 FPS vsync

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

## Troubleshooting

### FEXLoader: "Couldn't execute: FEXServer"
FEXServer must be started before FEXLoader can execute guest binaries. The app handles this automatically via `FexExecutor`, but for manual ADB testing, start FEXServer first (see testing section above).

### Vulkan/vkcube not rendering
1. Check Vortek ICD is configured:
   ```bash
   cat /usr/share/vulkan/icd.d/vortek_icd.json
   export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
   ```
2. Check VortekRenderer is running (Android logs)
3. Run `vulkaninfo --summary` - should show Mali GPU

### Black screen / no rendering
- For Vulkan apps: Check FramebufferBridge is receiving frames
- For X11 apps: Verify X11 socket exists `/tmp/.X11-unix/X0`
- Check DISPLAY is set: `echo $DISPLAY` should show `:0`

### App killed by Android
- Run the phantom process fix commands via ADB
- Ensure the foreground service notification is showing
- Check Settings > Battery > Steam Launcher is unrestricted

## License

MIT License

## Credits

- [Winlator](https://github.com/brunodev85/winlator) - Vortek Vulkan IPC passthrough
- [FEX-Emu](https://github.com/FEX-Emu/FEX) - x86-64 emulation with glibc emulation (FEX-2506)
- [Termux:X11](https://github.com/termux/termux-x11) - X11 server inspiration

## Technical Notes

### FEXLoader Invocation on Android

FEX binaries have `PT_INTERP=/opt/fex/lib/ld-linux-aarch64.so.1` hardcoded. On Android, `/opt/fex/` doesn't exist. We bypass this by invoking FEXLoader through the bundled dynamic linker directly:

```bash
${fexDir}/lib/ld-linux-aarch64.so.1 \
  --library-path ${fexDir}/lib:${fexDir}/lib/aarch64-linux-gnu \
  ${fexDir}/bin/FEXLoader \
  -- /bin/bash -c "command"
```

Where `fexDir = ${context.filesDir}/fex` (extracted from `fex-bin.tgz`).

### Vortek IPC Architecture
Vortek solves the fundamental problem that Android's Vulkan driver (vulkan.mali.so) uses Bionic libc, but container apps use glibc. You cannot load a Bionic library from glibc code.

**Solution:** Serialize Vulkan API calls in the container, send via Unix socket + ashmem ring buffers (4MB commands, 256KB results), deserialize and execute in Android process.

**Components:**
- `libvulkan_vortek.so` (glibc, container) - Vulkan ICD that serializes API calls
- `libvortekrenderer.so` (Bionic, Android) - Executes on real Mali GPU
- `libhook_impl.so` - Intercepts vulkan.mali.so loading

### Socket Access in FEX

FEX redirects `/tmp` to rootfs `/tmp/`. To make Vortek and X11 sockets accessible from the FEX environment, symlinks are created in the rootfs:

```
${fexRootfsDir}/tmp/.vortek/V0 → ${actualTmpDir}/.vortek/V0
${fexRootfsDir}/tmp/.X11-unix/X0 → ${actualX11SocketDir}/X0
```

### FEX-Emu Build Notes

- FEX binaries are bundled as `fex-bin.tgz` in assets (`.tgz` extension prevents AAPT decompression)
- Binaries have `RUNPATH=/opt/fex/lib` pointing to bundled glibc 2.38 libs
- `FEXInterpreter` is a binfmt handler — do NOT pass `--help`/`--version` to it
- Use `FEXLoader --version` for testing, `FEXLoader -- /path/to/x86/binary` for running x86 programs
- SquashFS rootfs from `rootfs.fex-emu.gg` is extracted by NDK-built `unsquashfs` (packaged as `libunsquashfs.so`)

### NDK-built unsquashfs

`unsquashfs` is compiled from squashfs-tools 4.6.1 source via Android NDK as part of the normal APK build. This eliminates the need for an ARM64 Ubuntu rootfs just to run `apt-get install squashfs-tools`. Sources are downloaded by `scripts/fetch_unsquashfs_source.sh` into `app/src/main/cpp/vendor/`.
