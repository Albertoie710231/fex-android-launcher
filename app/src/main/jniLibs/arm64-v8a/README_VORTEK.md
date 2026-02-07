# Vortek Vulkan Passthrough Libraries

This directory requires `libvortekrenderer.so` for Vulkan passthrough to work.

## The Problem

Android's `libvulkan.so` is compiled against Bionic (Android's C library).
The container uses glibc (via Box64/proot). You cannot load a Bionic library
from glibc code - they are binary incompatible.

## The Solution: Vortek

Vortek solves this via inter-process communication:

```
┌─────────────────────────────────────────────────────┐
│  Container (glibc/Box64)                            │
│  ┌──────────────────────────────────────┐           │
│  │ Game/Steam → DXVK → libvulkan_vortek.so          │
│  │ (serializes Vulkan commands)                     │
│  └─────────────────┬────────────────────┘           │
└────────────────────┼────────────────────────────────┘
                     │ Unix socket + ashmem ring buffers
┌────────────────────┼────────────────────────────────┐
│  Android (Bionic)  ▼                                │
│  ┌──────────────────────────────────────┐           │
│  │ VortekRenderer → Mali libvulkan.so    │           │
│  │ (executes real Vulkan calls)         │           │
│  └──────────────────────────────────────┘           │
└─────────────────────────────────────────────────────┘
```

## Required Libraries

### 1. libvortekrenderer.so (Android/Bionic, arm64-v8a)

**Location:** `app/src/main/jniLibs/arm64-v8a/libvortekrenderer.so`

This is the Android-side JNI library that:
- Receives serialized Vulkan commands from the container
- Executes them on the real Mali GPU using Android's Vulkan driver
- Returns results back to the container

### 2. libvulkan_vortek.so (Linux/glibc, x86_64)

**Location:** `app/src/main/assets/libvulkan_vortek.so`

This is a Vulkan ICD (Installable Client Driver) for the container that:
- Implements the Vulkan API for x86_64 Linux
- Serializes Vulkan commands
- Sends them to VortekRenderer via Unix socket

## How to Obtain These Libraries

### Option A: Extract from Winlator APK

1. Download Winlator APK from:
   - Official site: https://winlator.com
   - GitHub releases: https://github.com/winebox64/winlator/releases

2. Rename the .apk to .zip and extract it

3. Find the libraries:
   - `lib/arm64-v8a/libvortekrenderer.so` → copy to `app/src/main/jniLibs/arm64-v8a/`
   - `lib/x86_64/libvulkan_vortek.so` → copy to `app/src/main/assets/`

### Option B: Build from Source (Advanced)

The bionic-vulkan-wrapper project provides an open-source alternative:
https://github.com/nicolarevelant/bionic-vulkan-wrapper

## Verification

After adding the libraries:

1. Build and install the app

2. Start Steam or Terminal

3. Check logcat for:
   ```
   VortekRenderer: Vortek renderer library loaded successfully
   SteamService: Vortek renderer available for Vulkan passthrough
   SteamService: Vortek server started successfully
   ```

4. In the container, run:
   ```bash
   source /opt/scripts/setup_vortek.sh
   vulkaninfo --summary
   ```

## Notes

- Vortek has Mali-specific optimizations (gl_ClipDistance emulation, BCn texture support)
- Use DXVK 1.5.5 if newer versions have issues on Mali
- The ashmem ring buffers are 4MB (commands) + 256KB (results)

## References

- [Vortek Internals](https://leegao.github.io/winlator-internals/2025/06/01/Vortek1.html)
- [Winlator Mali](https://github.com/Fcharan/WinlatorMali)
