# Steam Launcher for Android (MediaTek)

Experimental Android app for running Windows / Steam games on a MediaTek
tablet. Two architectures live in the tree, at different stages of work:

1. **x86-64 everything via FEX-Emu** (branch `main`). Mature but blocked
   at Ys IX menu geometry corruption — see "State" below.
2. **Native ARM64 Bionic Wine + FEX WoW64** (branch
   `feat/native-arm64-pipeline`, WIP). Sidesteps the FEX Vulkan thunks
   suspected of causing the vertex corruption. Early stages — wine runs
   console PE binaries, GUI path starts but nothing renders yet.

Neither architecture currently runs a real game correctly. This README
describes what is observed on-device, not what the pipeline is supposed
to eventually deliver.

## x86-64 Architecture (main)

```
Android App (Kotlin)
  -> FEX-Emu (x86-64 -> ARM64 JIT)
    -> Ubuntu 22.04 rootfs overlay
      -> Steam Client (native Linux x86-64)
      -> Wine/Proton-GE 10-30 (Windows compatibility)
        -> DXVK 2.6+ (DirectX 11 -> Vulkan)
        -> vkd3d-proton (DirectX 12 -> Vulkan)
          -> Vulkan ICD chain -> Vortek -> Mali GPU
```

### Display Pipeline

Two separate pipelines work together for Wine/DXVK games:

| Pipeline | Purpose | Transport | Renderer |
|----------|---------|-----------|----------|
| **Vulkan/3D** | Game frames (DXVK) | Headless layer -> shared memory (`/tmp/headless_frames`) | FrameShmReader -> SurfaceView |
| **X11** | Window management, input, 2D | libXlorie (ARM64 native) -> abstract socket | LorieView |

### Vulkan Loader Chain

```
DXVK (PE) -> vulkan-1.dll (PE) -> winevulkan.so (Unix)
  -> dlopen("libvulkan.so.1") -> rootfs loader 1.3.204
  -> headless layer (surfaces, swapchain, frame capture)
  -> fex_thunk_icd.so (handle wrappers, features, barriers)
  -> libvulkan-guest.so (FEX thunk)
  -> host-side Khronos loader 1.3.283
  -> vortek_host_icd.json -> libvortek_icd_wrapper.so -> Vortek -> Mali GPU
```

Vulkan thunks are **disabled** for Wine (`"Vulkan": 0` in thunks.json). This forces Wine's
`dlopen("libvulkan.so.1")` to load the real x86-64 Ubuntu Vulkan loader (not the FEX thunk
overlay). The x86-64 loader supports `VK_KHR_xlib_surface` (compiled-in), which the ARM64
host loader filters out at compile time.

## State (2026-04-19)

This section describes what is actually verified on-device at the tip of
this branch. Nothing below is extrapolated from "the pipeline is wired up" —
if it's listed under Working it means I saw it work on the tablet; if it's
under Broken I saw it fail.

### x86-64 FEX pipeline (`main`)

**Working:**
- x86-64 emulation via FEX-Emu inside Ubuntu 22.04 rootfs overlay.
- Vulkan GPU passthrough for simple clients — `vkcube` runs at ~118 FPS via
  FEX thunks → Vortek → Mali.
- Wine / Proton-GE process startup, service tree, DXVK device creation.
- Native ARM64 X11 server (libXlorie) for window management / 2D.
- Frame capture layer → shared memory → `FrameShmReader` → `SurfaceView`.
- JavaSteam native ARM64 depot downloader for Steam AppID 228980.

**Partially working:**
- **Ys IX (2026-04-16 frame capture)**:
  - NIS America intro logo: renders correctly at ~58 FPS.
  - Menu text ("Load and continue a saved game."): renders correctly.
  - Vortek / FPS overlay: renders correctly.
  - **Menu background geometry**: **exploded vertices** — triangles blown
    to massive size, no recognizable layout. Colors are correct (suggests
    data reaches the GPU with right stride) but positions are wrong.
    Not "the menu renders"; the menu is corrupted.
  - Textures mostly fail (BC7 uploads producing solid colors). See
    `.claude/projects/-home-alberto-Documentos-fex-android-launcher/memory/project_current_state_20260415.md`
    for the vertex-debug session writeup.
  - Game crashes 59–77 seconds after ICD init.
- **Steam client**: logs in; UI boots. Launching a game via rungameid
  reaches `CreatingProcess`.

**Not working / not validated on this project's device:**
- **RE4 Remake**: the earlier commit `25899cd` claimed DRM verification,
  but the game does not actually run on this device under the current
  pipeline. That commit message was an overclaim inherited from
  MEDIATEK-DIRVERS-TEST and has been misleading debug effort since.
- **Sekiro**: the launch pipeline scaffolding exists, but a full
  playthrough has not been demonstrated; `steam_api64.dll` stub alone is
  not enough because Sekiro does its own Steam client IPC check.
- Any game past Ys IX's broken main menu.

### Native ARM64 Wine pipeline (`feat/native-arm64-pipeline`, WIP)

Pivot away from the x86-64-everything architecture toward GameNative's
model: native ARM64 Bionic Wine + DXVK ARM64 PE + FEX only for the game
binary via WoW64. Goal is to skip the FEX Vulkan thunks entirely (where
current Ys IX vertex corruption originates).

**Verified on-device at HEAD of the branch:**
- Pepelespooder's Bionic ARM64 `wine --version` returns `wine-10.0`.
- `wineserver` starts past its baked `/data/data/app.gamenative/…` NLS
  path via an `LD_PRELOAD` path-rewrite shim (`fex-emu/path_redirect.c`).
- PE DLLs (ntdll.dll, kernel32.dll, etc.) load with `PROT_EXEC` — the
  shim intercepts `mmap(fd, PROT_READ|WRITE, MAP_PRIVATE)` on PE files
  (detected by MZ magic) and returns `EPERM`, triggering Wine's own
  pread-into-anonymous-memory fallback so `mprotect(PROT_EXEC)` later
  succeeds via `execmem` (anonymous) instead of `execmod` (file-backed,
  blocked by Android's SELinux policy on `app_data_file`).
- `wine cmd /c ver` prints `Microsoft Windows 10.0.19043`, exit 0.
- `wine wineboot --init` populates the prefix (user.reg, drive_c tree,
  .update-timestamp). Exit 1 with only FreeType and /etc/machine-id
  warnings left; exit code is not yet checked to be "completion vs.
  partial".
- With `HKCU\Software\Wine\Drivers\Graphics=null` set, `wine notepad.exe`
  stays alive in an idle message pump for 6 s without the
  `nodrv_CreateWindow` error. This only proves the null driver code path
  runs; it does NOT prove the GUI is functional.

**Known-broken on the branch:**
- `libvulkan.so.1` does not load (`err:vulkan:vulkan_init_once Failed to
  load libvulkan.so.1`) despite the symlink in `proton11/lib/`. Until
  this is fixed nothing that uses Vulkan — including DXVK — can work.
- OLE / COM subsystem: `actxprxy.dll`, `uiautomationcore.dll`,
  `IUIAutomation` all fail to init during any GUI PE startup. Likely
  benign for most games but unverified.
- No game has been attempted on the native branch yet. No DXVK, no x86-64
  game binary under WoW64, no rendering.
- FreeType is not shipped, so anything that renders TrueType will fail
  or render blank.

## Components

### Android App (Kotlin)

| File | Role |
|------|------|
| `FexExecutor.kt` | ld.so wrapper, FEXServer lifecycle, FEX config env vars |
| `ContainerManager.kt` | Rootfs download/setup, ICD JSON, headless layer deploy |
| `ProtonManager.kt` | Proton-GE download/extract, Wine env, DXVK config, game launch |
| `TerminalActivity.kt` | VortekRenderer, FrameShmReader, X11Server (libXlorie), game buttons |
| `SteamContentDownloader.kt` | JavaSteam native depot downloader (228980 pre-download) |
| `FrameShmReader.kt` | Shared memory frame reader (polls /tmp/headless_frames at 8ms) |
| `FrameSocketServer.kt` | TCP frame receiver (legacy fallback) |

### x86-64 Vulkan Components

| File | Role |
|------|------|
| `fex-emu/fex_thunk_icd.c` | ICD shim: handle wrappers, barrier v2->v1, feature spoofing, inline shader fixup, BC passthrough, cmd tracing |
| `app/src/main/assets/vulkan_headless_layer.c` | Implicit layer: surfaces, swapchain, frame capture -> shared memory |
| `fex-emu/test_wine_vulkan.c` | 7-stage Wine Vulkan pipeline validation test |
| `fex-emu/steamwebhelper/` | SDL3, libdecor, pipewire stubs for steamwebhelper |
| `fex-emu/steam_api64_stub.c` | Native PE stub for steam_api64.dll (30+ exports, watchdog) |

### Native Libraries (ARM64, in `jniLibs/arm64-v8a/`)

| Library | Role |
|---------|------|
| `libvortekrenderer.so` | Vortek server: receives serialized Vulkan, executes on Mali GPU |
| `libvulkan_vortek.so` | Vortek ICD client: serializes Vulkan commands to Unix socket |
| `libvulkan_loader.so` | glibc ARM64 Vulkan ICD loader (host-side thunk path) |
| `libvulkan-host.so` | FEX host Vulkan thunk (ARM64, for DT_NEEDED intercept path) |
| `libFEX.so` | FEX-Emu x86-64 emulator (custom build FEX-2601) |
| `libXlorie.so` | Termux:X11 native X server |

## Steam Game Launch Pipeline

Games are launched via `steam://rungameid/<appid>`:

```
UnlockingH264 -> CheckShaderDepotManifest -> ProcessingInstallScript
  -> SynchronizingCloud -> SynchronizingStats -> ShowInterstitials
  -> ProcessingShaderCache -> SiteLicenseSeatCheckout -> DelayLaunch
  -> CreatingProcess -> Game process spawned
```

### Prerequisites for rungameid

1. **AppID 228980** (Steamworks Common Redistributables) must be installed with valid manifest (`StateFlags "4"`, real `buildid`, populated `InstalledDepots`). The app pre-downloads this via JavaSteam.
2. **Game files** must be in `~/.steam/debian-installation/steamapps/common/` (NOT via symlink -- FEX overlay can't read/write through symlinks for Steam's staging/validation).
3. **EULA** must be pre-accepted for games that require it (add `"<appid>_eula_0" "2"` to `localconfig.vdf`).
4. **Software OpenGL** for Steam UI (`LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`).
5. **No `-silent` flag** -- Steam must initialize UI pipeline to launch games.

## Building

### ICD (fex_thunk_icd.so)

```bash
cd fex-emu
x86_64-linux-gnu-gcc -shared -fPIC -O2 \
    -o fex_thunk_icd.so fex_thunk_icd.c \
    -ldl -lpthread -Wl,-soname,libfex_thunk_icd.so
cp fex_thunk_icd.so ../app/src/main/assets/libfex_thunk_icd.so
cp fex_thunk_icd.so ../app/src/main/assets/libfex_thunk_icd_x86_64.so
```

### Headless Layer

```bash
# Must use Ubuntu 22.04 toolchain (glibc <=2.35)
docker run --rm -v "$(pwd)/app/src/main/assets:/work" ubuntu:22.04 bash -c "
    apt-get update -qq &&
    apt-get install -y -qq gcc-x86-64-linux-gnu > /dev/null 2>&1 &&
    x86_64-linux-gnu-gcc -shared -fPIC -O2 \
        -o /work/libvulkan_headless_layer_x86_64.so \
        /work/vulkan_headless_layer.c \
        -ldl -lpthread
"
```

### APK

```bash
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## PC Test Scripts

- `test_re4.sh` -- Compare RE4 DRM/steam_api behavior between PC and device
- `test_sekiro.sh` -- Test Sekiro rungameid launch on PC

## ICD Feature Summary (fex_thunk_icd.c)

### Handle Wrappers
16-byte struct: offset 0 = loader dispatch (harmless writes by loader), offset 8 = real
Vortek handle (immutable, write-once). Replaces dispatch-swapping trampolines that had
race conditions with Wine's multi-threaded dispatch. All Cmd functions unwrap via
`mov rdi,[rdi+8]; jmp real_fn` trampolines.

### Feature Spoofing
| Feature | Real | Spoofed | Why |
|---------|------|---------|-----|
| `robustBufferAccess2` | 0 | 1 | DXVK hard-requires for adapter selection |
| `robustImageAccess2` | 0 | 1 | DXVK hard-requires for adapter selection |
| `nullDescriptor` | 0 | 1 | DXVK requires for unbound descriptors |
| `maintenance5` | 0 | 1 | DXVK uses inline shaders (ICD converts to real VkShaderModule) |
| `vertexPipelineStoresAndAtomics` | 0 | 1 | DXVK requires; stripped from CreateDevice |
| `logicOp` | 1 | 1 | Spoofed in features, stripped from CreateDevice, patched in pipelines |

### Extension Manipulation
| Action | Extension | Why |
|--------|-----------|-----|
| **Inject** | VK_EXT_robustness2 | Mali doesn't advertise; DXVK requires |
| **Inject** | VK_KHR_maintenance5 | Enables inline shader path in DXVK |
| **Inject** | VK_KHR_pipeline_library | DXVK pipeline compilation |
| **Hide** | VK_KHR_dynamic_rendering | Available via Vulkan 1.3 core; hiding avoids double-expose |
| **Hide** | VK_KHR_synchronization2 | Same — available via 1.3 core |

### Pipeline Fixups
- **Inline shader conversion**: DXVK embeds `VkShaderModuleCreateInfo` inline in pipeline
  stages when maintenance5 is available. ICD creates real `VkShaderModule` objects and strips
  `VkPipelineCreateFlags2CreateInfoKHR` from pNext.
- **CmdPipelineBarrier2->v1**: DXVK uses v2 (Vulkan 1.3); FEX thunks only support v1. ICD
  converts barrier structs on the fly.
- **QueueSubmit2 handle unwrapping**: Unwraps queue + command buffer HandleWrappers.
- **BC format passthrough**: Vortek handles BCn textures natively.

### Virtual Heap Split
Mali reports a single large DEVICE_LOCAL heap. ICD splits into:
- **Heap 0**: Textures (original size, DEVICE_LOCAL)
- **Heap 2**: Staging (512 MB, HOST_VISIBLE + HOST_COHERENT)
- **Type 4**: DEVICE_LOCAL-only (no HOST_VISIBLE) for texture allocations

### DXVK Dual Device Pattern
DXVK creates D1 (feature level 11_1 probe, destroyed) then D2 (real rendering). The ICD
shares a single real VkDevice across both CreateDevice calls (refcounted), avoiding
DEVICE_LOST from dual-device HOST state corruption.

## FEX Performance Tuning

| Setting | Effect |
|---------|--------|
| `TSOEnabled=0` | **THE key optimization.** 10 -> 60 FPS (6x). Removes x86 memory ordering barriers |
| `VectorTSOEnabled=0` | No barriers on SSE/AVX |
| `MemcpySetTSOEnabled=0` | No barriers on REP MOVS/STOS |
| `SilentLog=1` | Suppress FEX log I/O overhead |
| `X87ReducedPrecision=1` | 64-bit instead of 80-bit x87 |
| `Multiblock=1, MaxInst=5000` | Multi-block JIT, large blocks |

## Debugging

### ICD Debug Log
```bash
adb shell "run-as com.mediatek.steamlauncher cat files/fex-rootfs/Ubuntu_22_04/tmp/icd_debug.txt"
```

### Logcat
```bash
adb logcat -s FrameSocketServer VortekRenderer fex_thunk_icd
```

### Running FEX Commands via adb
FEXServer must be running (launch app first). See `gotchas.md` for the full template.

## Key Technical Challenges Solved

| Challenge | Solution |
|-----------|----------|
| Android seccomp blocks FEX syscalls | Binary-patch glibc `svc #0` → `movn x0, #37` + SIGSYS handler |
| Vortek ICD not standard-compliant | `vortek_icd_wrapper.so` calls `vortekInitOnce()` |
| ARM64 loader filters xlib surface | Disable Vulkan thunks for Wine; use x86-64 loader |
| LD_PRELOAD blocked by AT_SECURE | Deploy as Vulkan implicit layer instead |
| FEX child processes lose config | Set FEX_ROOTFS/FEX_THUNK* env vars |
| Dispatch trampoline races | HandleWrapper (16-byte struct) with immutable real_handle |
| Black frames (zero alpha) | Force alpha=255 in FrameSocketServer before rendering |
| DEVICE_LOST from shared device | Refcounted single VkDevice, reject second CreateDevice |
| Xvnc/Xvfb crash in FEX | Use libXlorie (ARM64 native X11 server) |
| Stale paths after APK install | `refreshNativeLibPaths()` auto-updates on launch |
| Steam 228980 dependency broken | JavaSteam native downloader + real manifest from PC |
| Steam EULA blocks invisible launch | Pre-accept via localconfig.vdf entry |
| Symlinks in steamapps don't work | Move game files directly into debian-installation path |
| Steam needs OpenGL for UI | Force llvmpipe via LIBGL_ALWAYS_SOFTWARE=1 |
| VK_ERROR_INCOMPATIBLE_DRIVER (-9) | Add vortek_host_icd.json (real path) to VK_ICD_FILENAMES |
| Ys IX "Hanabi shader failed" / "mapwarp.tbb not found" — the game builds `unix/home/user/...` asset paths from `GetModuleFileName`, treats them as CWD-relative, and fopens them through a `unix` symlink in the game dir | Point the `unix` symlink at the absolute host rootfs path (`$fexRootfsDir`), not `/`. Done at game launch in `ProtonManager.kt`. Game no longer fails on asset lookup; menu text becomes visible. Geometry still exploded, see "State" above. |
| Ys IX black swapchain (DXVK renders only empty begin/end passes) | Spoof `vertexAttributeInstanceRateDivisor` + `vertexAttributeInstanceRateZeroDivisor` in `headless_GetPhysicalDeviceFeatures2`, declare `VK_EXT_vertex_attribute_divisor` in the layer JSON, strip both before `CreateDevice`. Swapchain goes from all-black to receiving draw calls. Draw output is still wrong (exploded vertices). |
| BC texture uploads producing black/garbage pixels | Removed the BC→R8G8B8A8 substitution in `fex_thunk_icd.c:trace_CreateImage`. Vortek handles BCn natively; substituting created an RGBA image that then received BC-sized byte uploads. Textures still largely fail but via a different code path; see vertex-debug memory. |
| Dump-mode PPM capture always showing black | `HEADLESS_DUMP_DELAY` env var (default 60s) in the headless layer gates captures until the game leaves the loading phase. Set by `ProtonManager.getDumpModeLaunchCommand`. Fixes diagnostic tooling, not the underlying rendering issue. |

## First Run Setup

1. Launch the app and grant permissions
2. Click **"Setup Container"** — downloads rootfs (~995MB) + configures FEX
3. Open **Terminal** and test: `uname -a` (should show x86_64)
4. For Vulkan test: `export DISPLAY=:0; vkcube` then press **Display** button

## Project Structure

```
app/src/main/
├── java/com/mediatek/steamlauncher/
│   ├── TerminalActivity.kt         # Terminal + Display, Vortek, X11, game buttons
│   ├── ContainerManager.kt         # Rootfs setup, ICD/layer deploy
│   ├── FexExecutor.kt              # FEX invocation, env vars
│   ├── ProtonManager.kt            # Wine/Proton, DXVK config, game launch
│   ├── SteamContentDownloader.kt   # JavaSteam depot downloader (228980)
│   ├── FrameShmReader.kt           # Shared memory frame reader
│   ├── FrameSocketServer.kt        # TCP frame receiver (legacy)
│   └── ...
├── assets/
│   ├── vulkan_headless_layer.c     # x86-64 headless layer source
│   ├── libfex_thunk_icd.so         # x86-64 ICD shim
│   └── ...
├── jniLibs/arm64-v8a/              # ARM64 native libs (SELinux exec)
└── cpp/                            # NDK build (unsquashfs, JNI)

fex-emu/
├── fex_thunk_icd.c                 # ICD shim source (main Vulkan interception)
├── steam_api64_stub.c              # Native PE steam_api64.dll stub
├── test_wine_vulkan.c              # 7-stage Wine Vulkan test
├── steamwebhelper/                 # SDL3, libdecor, pipewire stubs
├── build_fex_thunks.sh             # Docker FEX build
└── Vulkan-Loader/                  # Loader source (reference)

test_re4.sh                         # PC test: RE4 DRM behavior comparison
test_sekiro.sh                      # PC test: Sekiro rungameid launch
```

## Device

- Samsung tablet, MediaTek Dimensity 9300+ (MT6989), Android 14
- GPU: Mali-G720-Immortalis MC12 (Vulkan 1.3.128)
- Package: `com.mediatek.steamlauncher`

## Credits

- [FEX-Emu](https://github.com/FEX-Emu/FEX) — x86-64 emulation (custom FEX-2601 build)
- [Winlator](https://github.com/brunodev85/winlator) — Vortek Vulkan IPC passthrough
- [Termux:X11](https://github.com/termux/termux-x11) — X11 server (libXlorie)

## License

MIT License
