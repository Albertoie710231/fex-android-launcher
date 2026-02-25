# Steam Launcher for Android (MediaTek)

Run Windows/Steam games on Android via x86-64 emulation with GPU-accelerated Vulkan.

## Architecture

```
Windows Game (.exe)
  → Wine/Proton-GE 10-30 (Windows compatibility layer)
    → DXVK 2.6+ (DirectX 11 → Vulkan translation)
      → winevulkan (Wine's Vulkan dispatch)
        → x86-64 Ubuntu Vulkan loader (v1.3.204)
          → Implicit Layer: VK_LAYER_HEADLESS_surface (surfaces, swapchain, frame capture)
            → ICD: fex_thunk_icd.so (x86-64 shim, feature spoofing, handle wrapping)
              → FEX-Emu thunks (x86-64 → ARM64 bridge)
                → Vortek (glibc↔Bionic IPC, Vulkan command serialization)
                  → Mali-G720 Immortalis MC12 GPU
```

### Display Pipeline

Two separate pipelines work together for Wine/DXVK games:

| Pipeline | Purpose | Transport | Renderer |
|----------|---------|-----------|----------|
| **Vulkan/3D** | Game frames (DXVK) | Headless layer → TCP 19850 | FrameSocketServer → SurfaceView |
| **X11** | Window management, input, 2D | libXlorie (ARM64 native) → abstract socket | LorieView |

### Dual-Loader Architecture

Vulkan thunks are **disabled** for Wine (`"Vulkan": 0` in thunks.json). This forces Wine's
`dlopen("libvulkan.so.1")` to load the real x86-64 Ubuntu Vulkan loader (not the FEX thunk
overlay). The x86-64 loader supports `VK_KHR_xlib_surface` (compiled-in), which the ARM64
host loader filters out at compile time (`wsi_unsupported_instance_extension()` in
`Vulkan-Loader/loader/wsi.c`). No environment variable can bypass this ARM64 filter.

## What Works

- **Full x86-64 emulation** via FEX-Emu (FEX-2601, Ubuntu 22.04 rootfs)
- **Vulkan GPU passthrough** — vkcube at 118 FPS via FEX thunks → Vortek → Mali
- **32-bit Vulkan** — verified via test_vulkan32 (4 extensions)
- **Wine/Proton-GE 10-30** — boots with 15+ processes, services running
- **Wine Vulkan test** — all 7 stages pass (including multi-threaded ACB)
- **DXVK initialization** — device creation, pipeline compilation, 55k+ queue submits
- **Game rendering on-screen** — Ys IX menu visible at ~10 FPS (FEX emulation speed)
- **Frame capture pipeline** — headless layer → TCP → FrameSocketServer → SurfaceView
- **X11 windowing** — libXlorie handles text overlays, 2D UI, input
- **Live display** — 1:1 frame delivery via lockHardwareCanvas, R↔B swizzle, alpha=255 fix
- **dpkg/apt** inside rootfs (overlay filesystem + linkat fallback)
- **Interactive terminal** with Display/Terminal toggle

## Current Status: Exploded Vertices

The game menu (Ys IX) renders with scattered/exploded white triangles. Text overlays
render correctly (2D path works). 3D mesh geometry has wrong vertex positions.

**Ruled out** (tested, no improvement):
- BC format substitution vs passthrough — identical result either way
- Dynamic rendering (Vulkan 1.3) — DXVK hard-requires it
- Maintenance5 / inline shaders — DXVK hard-requires it
- Vulkan 1.2 API cap — breaks DXVK entirely

**Likely causes**:
- robustness2 spoofing (OOB reads returning garbage instead of 0 on Mali)
- Vertex/index buffer handle unwrapping in Cmd intercepts
- Push constant or descriptor set data corruption through thunk/IPC chain
- Vortek vertex input format handling

## Components

### Android App (Kotlin)

| File | Role |
|------|------|
| `FexExecutor.kt` | ld.so wrapper, FEXServer lifecycle, FEX config env vars |
| `ContainerManager.kt` | Rootfs download/setup, ICD JSON, headless layer deploy |
| `ProtonManager.kt` | Proton-GE download/extract, Wine env, DXVK config, game launch |
| `TerminalActivity.kt` | VortekRenderer, FrameSocketServer, X11Server (libXlorie) |
| `FrameSocketServer.kt` | TCP frame receiver, R↔B swizzle, alpha=255 fix, direct rendering |

### x86-64 Vulkan Components

| File | Role |
|------|------|
| `fex-emu/fex_thunk_icd.c` | ICD shim: handle wrappers, barrier v2→v1, feature spoofing, inline shader fixup, BC passthrough, cmd tracing |
| `app/src/main/assets/vulkan_headless_layer.c` | Implicit layer: surfaces, swapchain, frame capture → TCP 19850 |
| `fex-emu/test_wine_vulkan.c` | 7-stage Wine Vulkan pipeline validation test |

### Native Libraries (ARM64, in `jniLibs/arm64-v8a/`)

| Library | Role |
|---------|------|
| `libvortekrenderer.so` | Vortek server: receives serialized Vulkan, executes on Mali GPU |
| `libvulkan_vortek.so` | Vortek ICD client: serializes Vulkan commands to Unix socket |
| `libvulkan_loader.so` | glibc ARM64 Vulkan ICD loader (host-side thunk path) |
| `libvulkan-host.so` | FEX host Vulkan thunk (ARM64, for DT_NEEDED intercept path) |
| `libFEX.so` | FEX-Emu x86-64 emulator (custom build FEX-2601) |
| `libXlorie.so` | Termux:X11 native X server |

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
# Must use Ubuntu 22.04 toolchain (glibc ≤2.35)
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
- **CmdPipelineBarrier2→v1**: DXVK uses v2 (Vulkan 1.3); FEX thunks only support v1. ICD
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

## First Run Setup

1. Launch the app and grant permissions
2. Click **"Setup Container"** — downloads rootfs (~995MB) + configures FEX
3. Open **Terminal** and test: `uname -a` (should show x86_64)
4. For Vulkan test: `export DISPLAY=:0; vkcube` then press **Display** button

## Project Structure

```
app/src/main/
├── java/com/mediatek/steamlauncher/
│   ├── TerminalActivity.kt         # Terminal + Display, Vortek, X11
│   ├── ContainerManager.kt         # Rootfs setup, ICD/layer deploy
│   ├── FexExecutor.kt              # FEX invocation, env vars
│   ├── ProtonManager.kt            # Wine/Proton, DXVK config
│   ├── FrameSocketServer.kt        # TCP frame receiver, rendering
│   └── ...
├── assets/
│   ├── vulkan_headless_layer.c     # x86-64 headless layer source
│   ├── libfex_thunk_icd.so         # x86-64 ICD shim
│   └── ...
├── jniLibs/arm64-v8a/              # ARM64 native libs (SELinux exec)
└── cpp/                            # NDK build (unsquashfs, JNI)

fex-emu/
├── fex_thunk_icd.c                 # ICD shim source (main Vulkan interception)
├── test_wine_vulkan.c              # 7-stage Wine Vulkan test
├── build_fex_thunks.sh             # Docker FEX build
└── Vulkan-Loader/                  # Loader source (reference)
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
