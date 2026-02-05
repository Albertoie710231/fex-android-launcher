# Development TODO

## Current Status (2026-02-05)

### What's Working ✓
- [x] PRoot executor with proper environment
- [x] Ubuntu 22.04 arm64 rootfs
- [x] Box64 with BOX32 support installed
- [x] Steam binary starts loading
- [x] X11Server wrapper exists (libXlorie)
- [x] **Vortek Vulkan passthrough** - Mali GPU rendering works!
- [x] **vkcube runs** - Renders at 15-40 FPS on screen
- [x] **Framebuffer bridge** - HardwareBuffer → Surface display
- [x] **Headless Vulkan surface** - VK_EXT_headless_surface wrapper
- [x] **Choreographer vsync** - 60 FPS display loop

### What's NOT Working
- [ ] **Semaphores crash** - "semaphore creation failed Function not implemented"
- [ ] **Steam full launch** - Blocked by semaphore issue (Box64 limitation)

### Root Cause Analysis: Semaphore Issue

The semaphore error is NOT a PRoot issue - it's a **Box64 architecture limitation on Android**:

```
Steam (x86)
    → Box64 (wraps pthread to host)
        → Android Bionic (not glibc!)
            → semaphore syscall
                → Android kernel REJECTS (ENOSYS)
```

**Why Box64 fails on Android:**
1. Box64 wraps pthread/semaphore calls to the HOST system
2. On Android, the host is Bionic (not glibc)
3. Android kernel blocks SYSV semaphores (semget/semop)
4. Android 8+ Seccomp blocks additional syscalls
5. Even `libandroid-sysv-semaphore` crashes with "bad system call"

**Key insight:** This is unfixable by patching PRoot. The problem is Box64's pthread wrapping design.

---

## Solution: Migrate to FEX-Emu

### Why FEX Solves the Semaphore Problem

| Aspect | Box64 | FEX |
|--------|-------|-----|
| pthread handling | **Wraps to host Bionic** | **Emulates x86 glibc** |
| sem_init() | Calls Bionic → fails | Emulates glibc → uses futex → works |
| Thunked libs | pthread, GL, Vulkan | GL, Vulkan, X11 (NOT pthread!) |

FEX only thunks performance-critical libraries:
- Graphics: libGL, libEGL, libVulkan
- Audio: libasound
- Windowing: libX11, libwayland-client

**pthread/semaphores are FULLY EMULATED** → glibc implementation → futex syscall → Android supports this!

### Target Architecture (FEX + Vortek)

```
┌──────────────────────────────────────────────────────────────┐
│  Android App (Kotlin)                                        │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  VortekRenderer (libvortekrenderer.so)                 │  │
│  │  - Receives Vulkan commands via Unix socket            │  │
│  │  - Executes on Mali GPU (/vendor/lib64/hw/vulkan.mali) │  │
│  │  - Renders to HardwareBuffer                           │  │
│  └────────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  FramebufferBridge                                     │  │
│  │  - Blits HardwareBuffer → Android Surface              │  │
│  │  - Choreographer vsync (60 FPS)                        │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────┬────────────────────────────────────────┘
                      │ Unix socket (Vulkan IPC)
┌─────────────────────▼────────────────────────────────────────┐
│  PRoot Container (ARM64 Ubuntu)                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  libvulkan_vortek.so (ICD)                              │ │
│  │  - Serializes Vulkan API calls                          │ │
│  │  - Sends to VortekRenderer via socket                   │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  FEX-Emu (NEEDED - replaces Box64)                      │ │
│  │  - Emulates x86 glibc (semaphores work!)                │ │
│  │  - Thunks Vulkan to libvulkan_vortek.so                 │ │
│  └─────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  Steam (x86) + DXVK                                     │ │
│  │  - Direct3D → Vulkan → Vortek → Mali GPU                │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

### Mali GPU Support ✓ WORKING

**Problem solved:** Most FEX/Android projects use Turnip driver (Adreno-only). We use **Vortek** instead.

**Our Solution: Vortek IPC Passthrough**
```
Container (glibc)              Android (Bionic)
┌─────────────────┐            ┌─────────────────┐
│ libvulkan_vortek│───socket──→│ VortekRenderer  │
│ (serializes API)│            │ (executes on GPU)│
└─────────────────┘            └────────┬────────┘
                                        ↓
                               /vendor/lib64/hw/vulkan.mali.so
                                        ↓
                                   Mali G710/G720
```

**Why Vortek works on Mali:**
1. Bypasses glibc/Bionic incompatibility via IPC
2. VortekRenderer runs in Android process with Bionic
3. Directly calls Mali's vulkan.mali.so driver
4. No Mesa/Zink/Turnip needed

**Verified working:**
- vkcube renders at 15-40 FPS
- Vulkan commands serialize correctly
- HardwareBuffer GPU blitting works

---

## Migration Plan

### Phase 1: Vulkan Rendering ✓ COMPLETE
Vulkan passthrough to Mali GPU via Vortek IPC architecture.

**Implemented:**
- [x] Vortek ICD (`libvulkan_vortek.so`) - serializes Vulkan commands in container
- [x] VortekRenderer (`libvortekrenderer.so`) - executes on Mali GPU in Android
- [x] VK_EXT_headless_surface wrapper for windowless rendering
- [x] FramebufferBridge - HardwareBuffer GPU blitting to Surface
- [x] Choreographer-based vsync display (60 FPS)
- [x] vkcube renders successfully

### Phase 2: Replace Box64 with FEX ← IN PROGRESS
**Why:** Box64 wraps pthread to Android Bionic, which rejects semaphores. FEX emulates glibc.

**Key Difference:**
- Box64: Uses native ARM64 libs, wraps pthread → semaphores FAIL
- FEX: Uses x86 libs from x86 rootfs, emulates glibc → semaphores WORK

**Implementation:**
- [x] Created `setup_fex.sh` - installs FEX via Ubuntu PPA
- [x] Downloads x86-64 rootfs (~2GB) for FEX to use
- [x] Modified `SteamService.kt` - added `useFexEmulator` flag
- [x] Created wrapper scripts: `fex-steam`, `fex-shell`, `fex-test-sem`
- [ ] Test FEX installation in rootfs
- [ ] Test semaphore test (`fex-test-sem`)
- [ ] Test Steam launch via FEX

**To test in rootfs:**
```bash
# Install FEX
/assets/setup_fex.sh

# Test semaphores work
fex-test-sem

# Launch Steam
fex-steam
```

### Phase 3: Vortek + Mali Integration ✓ COMPLETE
**Note:** Using Vortek (IPC passthrough) instead of Zink (Mesa GL→VK translation).

**Implemented:**
- [x] Vortek IPC via Unix socket + ashmem ring buffers
- [x] Mali driver access via `/vendor/lib64/hw/vulkan.mali.so`
- [x] libhook_impl.so intercepts Mali driver calls
- [x] Mali-specific optimizations (gl_ClipDistance, BCn textures)

### Phase 4: Steam Testing
**Tasks:**
- [ ] Launch Steam with FEX + Vortek
- [ ] Verify no semaphore errors
- [ ] Test Steam GUI rendering (X11 or headless)
- [ ] Test game downloads/launches
- [ ] Test DXVK games (Direct3D → Vulkan → Vortek → Mali)

---

## Resources

### Vortek (Our GPU Solution)
- Source: Extracted from Winlator APK
- `libvortekrenderer.so` - Android-side Vulkan executor
- `libvulkan_vortek.so` - Container-side Vulkan ICD
- Mali-specific optimizations included

### FEX-Emu (Needed for Steam)
- Main repo: https://github.com/FEX-Emu/FEX
- Mali GPU discussion: https://github.com/FEX-Emu/FEX/discussions/4989
- Termux-FEX: https://github.com/DesMS/Termux-FEX
- FEXDroid: https://github.com/gamextra4u/FEXDroid

### Box64 (Current - Semaphore Issues)
- Main repo: https://github.com/ptitSeb/box64
- Problem: Wraps pthread to Android Bionic → semaphores fail

### Android Semaphore Limitations
- SYSV semaphores: NOT supported by Android kernel
- Named POSIX semaphores (sem_open): NOT supported
- Unnamed POSIX semaphores (sem_init + futex): SHOULD work
- Seccomp (Android 8+): Blocks additional syscalls
- **FEX solves this** by emulating glibc semaphores via futex

### Related Projects
- Winlator: https://github.com/brunodev85/winlator (Vortek source)
- androBox: https://github.com/Pipetto-crypto/androBox
- termux-box: https://github.com/olegos2/termux-box

---

## File Changes Needed for FEX Migration

### SteamService.kt
```kotlin
// OLD (Box64):
exec /usr/local/bin/box32 "$STEAM_BIN" "$@"

// NEW (FEX):
exec FEXInterpreter "$STEAM_BIN" "$@"
```

### setup.sh
```bash
# Install FEX (replaces Box64)
wget https://github.com/FEX-Emu/FEX/releases/... -O /opt/fex/FEXInterpreter
chmod +x /opt/fex/FEXInterpreter

# Vortek ICD already configured - no Zink needed
```

### Environment Variables
```bash
# FEX config
export FEX_ROOTFS=/path/to/x86_rootfs

# Vortek Vulkan (already set up)
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
```

---

## Architecture Comparison

### Current (Box64 + Vortek - GPU Works, Semaphores Broken)
```
Android App → PRoot → Ubuntu ARM64 → Box64 → Steam x86
     ↓                     ↓                    ↓
VortekRenderer      libvulkan_vortek.so   Wraps pthread to Bionic
     ↓                     ↓                    ↓
Mali GPU ←───────── Vulkan IPC ──────────  Kernel rejects semaphores
   ✓ WORKS                                      ✗ CRASH
```

### Proposed (FEX + Vortek - Should Fully Work)
```
Android App → PRoot → Ubuntu ARM64 → FEX → Steam x86
     ↓                     ↓                ↓
VortekRenderer      libvulkan_vortek.so  Emulates glibc
     ↓                     ↓                ↓
Mali GPU ←───────── Vulkan IPC ────── Semaphores via futex
   ✓ WORKS              ✓ WORKS           ✓ SHOULD WORK
```

---

## Quick Reference

### Test Vortek + Mali GPU ✓
```bash
# Inside proot - runs vkcube with headless surface
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
export LD_PRELOAD=/lib/libvulkan_headless.so
./vkcube --c 1000  # Renders 1000 frames to Android screen
```

### Test Vulkan Info
```bash
# Inside proot
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
vulkaninfo --summary  # Should show Mali GPU
```

### Test X11 Server
```bash
# Inside proot
export DISPLAY=:0
xterm  # or xclock, xeyes
```

### Test FEX (after migration)
```bash
# Inside proot
FEXInterpreter /path/to/x86/binary
```

### Check Semaphores
```bash
# Simple test program
cat > /tmp/sem_test.c << 'EOF'
#include <semaphore.h>
#include <stdio.h>
int main() {
    sem_t sem;
    if (sem_init(&sem, 0, 1) == -1) {
        perror("sem_init");
        return 1;
    }
    printf("Semaphore created successfully!\n");
    sem_destroy(&sem);
    return 0;
}
EOF
gcc /tmp/sem_test.c -o /tmp/sem_test -lpthread
/tmp/sem_test
```
