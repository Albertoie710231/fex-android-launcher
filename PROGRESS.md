# Steam Launcher Progress Report

## Session: 2026-02-05 (Part 2) — PRoot Eliminated, FEX-Direct Architecture

### Major Milestone: Complete PRoot Removal

Eliminated PRoot entirely from the project. The app now runs FEX-Emu directly via ProcessBuilder with no ptrace overhead.

**Old architecture:** `App → PRoot → ARM64 Ubuntu rootfs → FEX → x86-64`
**New architecture:** `App → ProcessBuilder → ld-linux-aarch64.so.1 → FEXLoader → x86-64`

### What Was Done

#### 1. Created `FexExecutor.kt` (replaces ProotExecutor.kt)
- Runs FEXLoader directly via ProcessBuilder — no PRoot, no ptrace
- Invokes FEXLoader through bundled `ld-linux-aarch64.so.1` (bypasses missing `/opt/fex/` on Android)
- Clears Android env pollution (LD_PRELOAD, ANDROID_*, BOOTCLASSPATH, etc.)
- Sets `USE_HEAP=1`, `FEX_DISABLETELEMETRY=1`
- Creates symlinks in FEX rootfs `/tmp/` for Vortek and X11 sockets
- Added FEXServer lifecycle management (auto-start with `-f -p 300`)

#### 2. Simplified `ContainerManager.kt`
- Removed all ARM64 rootfs download/extraction (no more 150MB download)
- Removed Box64/Box86 installation, setupSteamDependencies, configureBaseSystem
- New setup flow: extract FEX binaries → download SquashFS → extract with unsquashfs → configure FEX → setup Vortek ICD → download Steam
- Uses NDK-built `unsquashfs` (no PRoot needed to extract rootfs)

#### 3. NDK-Built unsquashfs
- Compiled squashfs-tools 4.6.1 + xz-utils 5.4.5 from source via Android NDK
- Hand-crafted `vendor/xz-config/config.h` for Android cross-compilation
- Fixed: Bionic has no `-lpthread` (pthreads in libc), no `lutimes()`, needs `_GNU_SOURCE`
- Fixed: Explicit source file list (GLOB_RECURSE picked up tablegen utilities)
- Result: `libunsquashfs.so` packaged in APK, runs natively on device

#### 4. Updated All Kotlin Files
- `SteamLauncherApp.kt`: `fexExecutor` replaces `prootExecutor`, added path helpers
- `TerminalActivity.kt`: Uses FexExecutor, updated welcome message and quick buttons
- `SteamService.kt`: Uses FexExecutor for Steam launch
- `SettingsActivity.kt`: FEX version display, removed Box64/PRoot tests
- `VulkanBridge.kt`: Updated to use FexExecutor
- `MainActivity.kt`: Already clean (no PRoot references)

#### 5. Deleted PRoot Artifacts
- Deleted `ProotExecutor.kt`
- Removed `keepDebugSymbols += "**/libproot.so"` from build.gradle.kts
- PRoot native libs (libproot.so, libproot-loader*.so, libtalloc.so) no longer in APK

#### 6. Rewrote README.md
- Complete rewrite with new FEX-direct architecture
- Added ADB testing commands for FEXLoader, FEXServer, unsquashfs
- Updated project structure, component descriptions, troubleshooting

### Device Testing Results

| Test | Result |
|------|--------|
| APK builds | PASS |
| App installs & launches | PASS - no crashes |
| libunsquashfs.so runs on device | PASS - gzip + xz support |
| FEX binaries extract from asset | PASS |
| FEXLoader --version | PASS - `FEX-Emu (FEX-2506)` |
| FEXServer starts | PASS - socket created |
| FEXLoader connects to FEXServer | PASS |
| FEXLoader -- /bin/uname | Expected fail - rootfs not yet downloaded |

### Next Steps
1. Test container setup via app UI (download + extract FEX rootfs)
2. Run x86-64 commands through FEX with rootfs
3. Test Vortek Vulkan via FEX
4. Launch Steam

---

## Session: 2026-02-05 (Part 1) — FEX Integration Planning

### What We Accomplished

#### 1. FEX-Emu Setup Inside PRoot (Proved Concept)
- Installed FEX-Emu (FEX-2506) at `/opt/fex/` with bundled glibc 2.38 libraries
- Created `setup_fex.sh` that extracts FEX binaries and downloads x86-64 rootfs
- Verified via ADB:
  - `FEXLoader --version` → `FEX-Emu (FEX-2506)`
  - `FEXLoader -- /bin/uname -a` → `x86_64 GNU/Linux`
  - x86-64 SquashFS rootfs downloaded and extracted from `rootfs.fex-emu.gg`

#### 2. Discovered FEX Eliminates PRoot
- FEX provides its own rootfs overlay (filesystem redirection)
- FEX emulates glibc (semaphores work via futex)
- PRoot's only purpose was filesystem redirection — FEX does this natively
- Decision: eliminate PRoot entirely

#### 3. Created Migration Plan
- Detailed plan to replace ProotExecutor with FexExecutor
- Identified FEXLoader PT_INTERP bypass (bundled ld.so invocation)
- Identified socket access strategy (symlinks in FEX rootfs /tmp/)
- Identified unsquashfs compilation approach (NDK cross-compile)

### Key Technical Discoveries
- FEX binaries have hardcoded `PT_INTERP=/opt/fex/lib/ld-linux-aarch64.so.1` — need to invoke via bundled dynamic linker
- FEXServer must be running before FEXLoader can execute guest binaries
- AAPT silently decompresses `.tar.gz` files — use `.tgz` extension + `noCompress`
- `FEXInterpreter` is a binfmt handler (crashes with --help), use `FEXLoader` for testing

---

## Session: 2026-02-04 — Vortek Vulkan Rendering

### What We Accomplished

#### 1. Vortek IPC Vulkan Passthrough Working
- Integrated Vortek from Winlator for Mali GPU access
- `libvulkan_vortek.so` (container side) serializes Vulkan API calls
- `libvortekrenderer.so` (Android side) executes on real Mali GPU
- Bypasses glibc/Bionic incompatibility via Unix socket + ashmem

#### 2. vkcube Rendering on Screen
- vkcube renders at 15-40 FPS through Vortek pipeline
- FramebufferBridge: HardwareBuffer → Android Surface via Canvas
- Choreographer-based vsync for 60 FPS display loop
- VK_EXT_headless_surface wrapper for windowless rendering

### Components Added
- `libvortekrenderer.so` - Android Vulkan executor
- `libvulkan_vortek.so` - Container Vulkan ICD
- `libhook_impl.so` - Mali driver hook
- `FramebufferBridge.kt` + `framebuffer_bridge.cpp` - GPU frame display
- `VortekRenderer.kt` - Vortek lifecycle management

---

## Session: 2026-02-03 — PRoot Futex Patch + Steam Loading

### What We Accomplished

#### 1. Patched PRoot for Futex/Semaphore Support
- Added `futex_time64` (syscall 422) passthrough to PRoot
- Built patched `libproot.so` (222KB) for ARM64

#### 2. Steam Binary Loading via Box32
- Steam binary loads via Box32 without semaphore errors
- But: Box64/Box32 wraps pthread to Android Bionic → semaphores still fail at runtime
- Root cause: unfixable without emulating glibc (which is what FEX does)

### Key Insight
Box64 wraps pthread/semaphore calls to the HOST system (Bionic), which rejects SYSV semaphores. FEX-Emu emulates x86 glibc entirely, so semaphores use futex syscalls that Android supports. This is why we migrated to FEX.

---

## Session: 2026-02-02 — Box64 with BOX32 + Initial Setup

### What We Accomplished

#### 1. Box64 v0.4.1 with BOX32 Support
- Device has no 32-bit ABI — compiled Box64 with `-DBOX32=ON` for ARM64-only devices
- Box64 (75MB) handles both 32-bit and 64-bit x86 code

#### 2. ARM64 Ubuntu Rootfs
- Downloaded ARM64 Ubuntu 22.04 rootfs
- Configured PRoot executor for filesystem virtualization
- Installed x86 libraries for Steam

#### 3. App Foundation
- Created Android app with Kotlin
- PRoot executor, container management, settings, terminal
- X11 server wrapper (libXlorie from Termux:X11)

### Outcome
Box64 worked for basic x86 execution but failed on Steam's semaphore requirements due to Bionic wrapping. This led to the FEX-Emu investigation in later sessions.
