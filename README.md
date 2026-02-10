# FEX Android Launcher

Run x86-64 Linux applications on Android with Vulkan GPU passthrough at 120 FPS.

An Android app that uses [FEX-Emu](https://github.com/FEX-Emu/FEX) for x86-64 emulation and [Vortek](https://github.com/brunodev85/winlator) for Vulkan IPC passthrough to the ARM Mali GPU. Tested on MediaTek Dimensity 9300+ with Mali-G720-Immortalis MC12.

## What Works

- **Full x86-64 emulation** via FEX-Emu (Ubuntu 22.04 rootfs)
- **Vulkan GPU passthrough** — x86-64 Vulkan calls → FEX thunks → Vortek → Mali GPU
- **Live display at 120 FPS** — 1:1 frame delivery via non-blocking TCP streaming
- **vulkaninfo** detects Vortek (Mali-G720-Immortalis MC12), Vulkan 1.3.128
- **vkcube** renders the LunarG spinning cube with correct colors at 118 FPS
- **dpkg/apt** work inside the FEX rootfs (overlay filesystem fixes applied)
- **Interactive terminal** with Display/Terminal toggle for Vulkan output

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                   Android App (Kotlin)                        │
├──────────────────────────────────────────────────────────────┤
│  TerminalActivity              │  ContainerManager            │
│  - Interactive FEX terminal    │  - Rootfs download & setup   │
│  - Display/Terminal toggle     │  - Config.json, ICD JSON     │
│  - SurfaceView (120Hz)         │  - Auto-refresh stale paths  │
├──────────────────────────────────────────────────────────────┤
│              Frame Display Pipeline (118 FPS)                 │
│  ┌──────────────────────────────────────────────────────────┐│
│  │ FrameSocketServer                                        ││
│  │  - TCP 19850 receiver (non-blocking sender)              ││
│  │  - Direct lockHardwareCanvas rendering (no Choreographer)││
│  │  - R↔B color swizzle via GPU ColorMatrix                 ││
│  │  - 1:1 frame delivery (recv = display = 118 FPS)         ││
│  └──────────────────────────────────────────────────────────┘│
├──────────────────────────────────────────────────────────────┤
│              Vortek Renderer (Android-side)                   │
│  ┌──────────────────────────────────────────────────────────┐│
│  │ VortekRendererComponent (libvortekrenderer.so)           ││
│  │  - Receives Vulkan commands via Unix socket              ││
│  │  - Executes on Mali GPU (/vendor/lib64/hw/vulkan.mali)   ││
│  └──────────────────────────────────────────────────────────┘│
├──────────────────────────────────────────────────────────────┤
│              FEX-Emu (x86-64 emulation, no PRoot)            │
│  ┌──────────────────────────────────────────────────────────┐│
│  │ FEXLoader (ARM64 native, invoked via ld.so wrapper)      ││
│  │  ├── x86-64 rootfs (Ubuntu 22.04)                        ││
│  │  ├── Vulkan ICD loader → vortek_icd_wrapper.so           ││
│  │  │   └── Calls vortekInitOnce() + maps vk→vt_call_*     ││
│  │  ├── FEX thunks (guest x86-64 ↔ host ARM64 Vulkan)      ││
│  │  └── LD_PRELOAD headless wrapper (frame capture)         ││
│  └──────────────────────────────────────────────────────────┘│
├──────────────────────────────────────────────────────────────┤
│              Mali-G720-Immortalis MC12 (Vulkan 1.3.128)      │
└──────────────────────────────────────────────────────────────┘
```

### Vulkan Pipeline (Full Chain)

```
x86-64 app (vkcube)
    │  LD_PRELOAD: libvulkan_headless.so (fake swapchain + frame capture)
    ↓
FEX thunks (x86-64 guest → ARM64 host Vulkan calls)
    ↓
Vulkan ICD loader (libvulkan.so.1 → libvulkan_loader.so)
    ↓
vortek_icd_wrapper.so (vortekInitOnce + vk→vt_call_ mapping)
    ↓
libvulkan_vortek.so (serializes Vulkan → Unix socket + ashmem)
    ↓
VortekRendererComponent (deserializes → Mali GPU)
    ↓
Mali-G720 GPU (renders frames)
    ↓
headless wrapper: vkMapMemory → TCP 19850 (non-blocking, frame dropping)
    ↓
FrameSocketServer → lockHardwareCanvas → SurfaceView (120Hz)
```

## Requirements

- Android device with:
  - ARM64 SoC with Mali GPU (tested on MediaTek Dimensity 9300+)
  - Vulkan 1.1+ support
  - 8GB+ RAM recommended
  - Android 8.0+ (API 26+)
- Build tools:
  - Android Studio with SDK 34, NDK, CMake 3.22.1+
  - Docker (for cross-compiling x86-64 headless wrapper)

## Building

```bash
# Build the APK
./gradlew assembleDebug

# Install on device
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### Cross-compile Scripts (Docker)

```bash
# Headless Vulkan wrapper (x86-64 .so, deployed to rootfs)
fex-emu/build_vulkan_headless.sh

# Vulkan ICD loader (ARM64 glibc)
fex-emu/build_vulkan_loader.sh

# Vortek ICD wrapper (ARM64 glibc)
fex-emu/build_vortek_wrapper.sh

# FEX-Emu with thunks
fex-emu/build_fex_thunks.sh
```

## First Run Setup

1. Launch the app and grant permissions
2. Click **"Setup Container"** — downloads and extracts:
   - FEX ARM64 binaries from bundled `fex-bin.tgz`
   - x86-64 SquashFS rootfs (~995MB) from `rootfs.fex-emu.gg`
   - Configures FEX (Config.json, thunks, Vortek ICD)
3. Open **Terminal** and test:
   ```
   uname -a    # Should show x86_64
   vulkaninfo --summary    # Should show Vortek (Mali-G720)
   ```
4. Run vkcube with display:
   ```
   export LD_PRELOAD=/usr/lib/libvulkan_headless.so DISPLAY=:0; vkcube
   ```
   Press **Display** button to see the live rendering

## Key Technical Challenges Solved

### Android Seccomp Blocking FEX Syscalls
Android's seccomp filter kills processes using blocked syscalls. Two-layer fix:
- **Layer 1**: Binary-patch glibc to replace `svc #0` with `movn x0, #37` for uncatchable `SECCOMP_RET_KILL`
- **Layer 2**: SIGSYS handler in FEXCore redirects catchable syscalls (`accept→accept4`, `openat2→openat`)

### Vortek ICD Not Standard-Compliant
Winlator's `libvulkan_vortek.so` returns NULL from `vk_icdGetInstanceProcAddr` because `vortekInitOnce()` is never called during standard ICD loader protocol. Fix: `vortek_icd_wrapper.so` — thin wrapper that calls `vortekInitOnce()` and maps `vk→vt_call_*`.

### FEX Rootfs Overlay Fallback Bug
FEX's FileManager tried raw host paths when overlay operations failed, returning wrong errno on Android (e.g., `ENOENT` instead of `EEXIST`). Fix: return overlay result directly when path resolves, don't fall through.

### Hard Links Blocked by SELinux
Android SELinux blocks `linkat` in app data dirs. dpkg needs backup links. Fix: `LinkatWithCopyFallback` — tries `linkat`, falls back to `sendfile` copy on `EPERM`.

### Stale Paths After APK Reinstall
Every `adb install` changes `nativeLibDir` (random hash), breaking 3 paths. Fix: `refreshNativeLibPaths()` auto-updates Config.json, libvulkan.so.1 symlink, and ICD JSON on every launch.

### Frame Streaming Optimization
Evolved through multiple iterations:
1. **Choreographer + double-buffer** → 50 FPS display (missed vsync deadlines)
2. **Blocking TCP** → throttled vkcube to 65-90 FPS (variable, caused visual "acceleration")
3. **Non-blocking TCP + vsync emulation + direct rendering** → 118 FPS 1:1 delivery (current)

## Android 12+ Phantom Process Fix

```bash
adb shell "settings put global settings_enable_monitor_phantom_procs false"
adb shell "/system/bin/device_config set_sync_disabled_for_tests persistent"
adb shell "/system/bin/device_config put activity_manager max_phantom_processes 2147483647"
```

## Project Structure

```
app/src/main/
├── java/com/mediatek/steamlauncher/
│   ├── SteamLauncherApp.kt         # Application class, path helpers
│   ├── MainActivity.kt             # Main launcher UI
│   ├── TerminalActivity.kt         # Terminal + Display toggle, 120Hz, Vortek setup
│   ├── GameActivity.kt             # Game launcher
│   ├── SettingsActivity.kt         # Settings & diagnostics
│   ├── SteamService.kt             # Foreground service (keeps FEX alive)
│   ├── ContainerManager.kt         # Rootfs setup, Config.json, refreshNativeLibPaths()
│   ├── FexExecutor.kt              # ld.so wrapper, FEXServer lifecycle, env setup
│   ├── FrameSocketServer.kt        # TCP frame receiver, direct lockHardwareCanvas render
│   ├── FramebufferBridge.kt        # HardwareBuffer → Surface blitting
│   ├── VortekRenderer.kt           # Vortek component wrapper
│   ├── VortekSurfaceView.kt        # Vortek rendering surface
│   ├── VulkanBridge.kt             # Vulkan/Vortek ICD setup
│   ├── X11Server.kt                # X11 server lifecycle
│   ├── X11SocketHelper.kt          # Unix socket helper
│   └── X11SocketServer.kt          # X11 socket server
├── assets/
│   ├── vulkan_headless.c           # x86-64 headless Vulkan wrapper (LD_PRELOAD)
│   ├── libvulkan_headless.so       # Compiled headless wrapper
│   ├── fex-bin.tgz                 # FEX ARM64 binaries + glibc 2.38
│   └── libvulkan_vortek.so         # Container Vulkan ICD (from Winlator)
├── jniLibs/arm64-v8a/
│   ├── libFEX*.so                  # FEX binaries (in nativeLibDir for SELinux exec)
│   ├── libvortekrenderer.so        # Android Vulkan executor
│   ├── libvortek_icd_wrapper.so    # ICD wrapper (vortekInitOnce + vk→vt_call_)
│   ├── libvulkan_loader.so         # Cross-compiled Vulkan ICD loader
│   ├── libhook_impl.so             # Mali driver hook
│   └── ...
├── cpp/
│   ├── CMakeLists.txt              # NDK build (includes unsquashfs)
│   ├── steamlauncher.cpp           # JNI bridge
│   ├── framebuffer_bridge.cpp      # HardwareBuffer JNI
│   ├── x11_socket.cpp              # Unix socket JNI
│   └── vendor/                     # squashfs-tools & xz-utils source
└── res/layout/                     # UI layouts

fex-emu/
├── build_vulkan_headless.sh        # Cross-compile headless wrapper
├── build_vulkan_loader.sh          # Cross-compile Vulkan ICD loader
├── build_vortek_wrapper.sh         # Cross-compile vortek_icd_wrapper.so
├── build_fex_thunks.sh             # Full FEX build with thunks
├── vortek_icd_wrapper.c            # ICD wrapper source
└── ...

scripts/
├── patch_glibc_seccomp.py          # Binary-patch glibc for seccomp bypass
├── patch_vortek_socket_path.py     # Binary-patch Vortek socket path + RUNPATH
└── fetch_unsquashfs_source.sh      # Downloads vendor source for NDK build
```

## Device Tested

- Samsung tablet, MediaTek Dimensity 9300+ (MT6989)
- GPU: Mali-G720-Immortalis MC12
- Android 14
- 120Hz display

## Testing FEX via ADB

```bash
# Quick version check
adb shell run-as com.mediatek.steamlauncher env \
  LD_LIBRARY_PATH=/data/data/com.mediatek.steamlauncher/files/fex/lib \
  /data/data/com.mediatek.steamlauncher/files/fex/bin/FEXLoader --version
# Expected: FEX-Emu (FEX-2506)
```

For detailed testing, push a script (multiline `sh -c` breaks over ADB):

```bash
cat > /tmp/fex_test.sh << 'EOF'
#!/system/bin/sh
FEXDIR=/data/data/com.mediatek.steamlauncher/files/fex
TMPDIR=/data/data/com.mediatek.steamlauncher/files/tmp
HOME=/data/data/com.mediatek.steamlauncher/files/fex-home
export TMPDIR HOME USE_HEAP=1 FEX_DISABLETELEMETRY=1
export LD_LIBRARY_PATH=$FEXDIR/lib:$FEXDIR/lib/aarch64-linux-gnu
unset LD_PRELOAD BOOTCLASSPATH ANDROID_ART_ROOT ANDROID_I18N_ROOT
unset ANDROID_TZDATA_ROOT COLORTERM DEX2OATBOOTCLASSPATH ANDROID_DATA

$FEXDIR/bin/FEXServer -f -p 300 &
sleep 3

echo "=== FEXLoader version ==="
$FEXDIR/bin/FEXLoader --version

echo "=== uname ==="
$FEXDIR/bin/FEXLoader -- /bin/uname -a

echo "=== os-release ==="
$FEXDIR/bin/FEXLoader -- /bin/bash -c "export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin; head -4 /etc/os-release"
EOF

adb push /tmp/fex_test.sh /data/local/tmp/fex_test.sh
adb shell run-as com.mediatek.steamlauncher sh /data/local/tmp/fex_test.sh
```

## Technical Notes

### FEXLoader Invocation on Android

FEX binaries are stored in `jniLibs/arm64-v8a/` (installed to `nativeLibDir`) for SELinux exec permission. They are invoked via the bundled `ld-linux-aarch64.so.1`:

```bash
ld.so --library-path <nativeLibDir>:<fexDir>/lib libFEX.so /bin/bash -c "command"
```

`LD_LIBRARY_PATH` must be set explicitly — RPATH alone isn't sufficient on Android's dynamic linker.

### Vortek IPC Architecture

Vortek solves the fundamental problem that Android's Vulkan driver (`vulkan.mali.so`) uses Bionic libc, but container apps use glibc. You cannot load a Bionic library from glibc code.

**Solution:** Serialize Vulkan API calls in the container, send via Unix socket + ashmem ring buffers (4MB commands, 256KB results), deserialize and execute in the Android process.

### Socket Access in FEX

FEX redirects `/tmp` to rootfs `/tmp/`. Symlinks bridge Vortek and X11 sockets:

```
${fexRootfsDir}/tmp/.vortek/V0 → ${actualTmpDir}/.vortek/V0
${fexRootfsDir}/tmp/.X11-unix/X0 → ${actualX11SocketDir}/X0
```

### NDK-built unsquashfs

`unsquashfs` is compiled from squashfs-tools 4.6.1 source via Android NDK as part of the APK build (packaged as `libunsquashfs.so`). Sources are downloaded by `scripts/fetch_unsquashfs_source.sh` into `app/src/main/cpp/vendor/`.

## Credits

- [FEX-Emu](https://github.com/FEX-Emu/FEX) — x86-64 emulation (custom FEX-2506 build)
- [Winlator](https://github.com/brunodev85/winlator) — Vortek Vulkan IPC passthrough
- [Termux:X11](https://github.com/termux/termux-x11) — X11 server inspiration

## License

MIT License
