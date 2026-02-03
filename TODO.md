# Development TODO

## Current Status (2026-02-03)

### Completed
- [x] PRoot executor with proper environment (PROOT_TMP_DIR, LD_LIBRARY_PATH)
- [x] libtalloc.so.2 symlink fix
- [x] Ubuntu 22.04 arm64 rootfs download and extraction
- [x] Symlink handling in tar extraction (/bin -> /usr/bin)
- [x] Steam .deb download and extraction from Android side (OkHttp + Commons-Compress)
- [x] Container commands working (echo, uname, ls, bash via proot)
- [x] Git repository initialized
- [x] **Box64 compiled with BOX32 support** (v0.4.1 with ARM Dynarec, 75MB)
- [x] **box32 symlink created** (for 32-bit x86 Steam client)
- [x] 32-bit x86 libraries installed in `/lib/i386-linux-gnu/`
- [x] **PRoot patched for futex/semaphore support** (fixes "semaphore creation failed")
- [x] **Steam bootstrap extracted** (via Java XZ library, no external xz needed)
- [x] **Steam loads successfully via Box32** (verified via ADB test)

### What's Working
```
✅ PRoot with futex_time64 syscall handling
✅ Box64 v0.4.1 with Dynarec
✅ Box32 (symlink to Box64 with BOX32=ON)
✅ Steam binary loads without semaphore errors
✅ All wrapped libraries load (libdl, librt, libm, libpthread, libc, ld-linux)
✅ App is self-contained (XZ decompression via Java, no external binaries needed)
```

### Current Test Output (Success)
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

## Next Steps

### 1. Integrate X11 Server (HIGH PRIORITY)
Steam needs a display server to render its GUI. Options:
- **Termux:X11** - Use as external dependency (user installs separately)
- **Bundle Termux:X11 library** - Integrate into app for self-contained solution
- **Implement minimal X11 server** - Based on Lorie (already have stubs in code)

### 2. Test Steam GUI Rendering
Once X11 is available:
```bash
# Steam should connect to DISPLAY=:0
export DISPLAY=:0
box32 /home/user/.local/share/Steam/ubuntu12_32/steam
```

### 3. Audio Support
- PulseAudio server on Android side
- Or use `PULSE_SERVER=tcp:127.0.0.1:4713`

### 4. Input Handling
- Touch to mouse translation
- Virtual keyboard for text input
- Game controller support

## Known Issues

### lscpu not found (harmless warning)
```
sh: 1: lscpu: not found
```
Box64 tries to detect CPU but lscpu isn't in minimal rootfs. Doesn't affect functionality.

### libtalloc symlink breaks after APK reinstall
The symlink points to APK native lib path which changes on reinstall. Fix:
```bash
APP_PATH=$(adb shell "pm path com.mediatek.steamlauncher" | cut -d: -f2 | tr -d '\r\n')
NATIVELIB=$(dirname "$APP_PATH")/lib/arm64
adb shell "run-as com.mediatek.steamlauncher sh -c '
  ln -sf $NATIVELIB/libtalloc.so files/lib-override/libtalloc.so.2
'"
```

## Build Commands Reference

### Test Steam via ADB
```bash
adb shell "run-as com.mediatek.steamlauncher sh -c '
  export NATIVELIB=\$(dirname \$(pm path com.mediatek.steamlauncher | cut -d: -f2))/lib/arm64
  export LD_LIBRARY_PATH=files/lib-override:\$NATIVELIB
  export PROOT_LOADER=\$NATIVELIB/libproot-loader.so
  export PROOT_TMP_DIR=cache/proot-tmp
  export PROOT_NO_SECCOMP=1

  timeout 15 \$NATIVELIB/libproot.so --rootfs=files/rootfs \
    -b /dev -b /proc -b /sys -w /home/user \
    /bin/sh -c \"
      export HOME=/home/user DISPLAY=:0 BOX64_LOG=1 STEAM_RUNTIME=0
      export LD_LIBRARY_PATH=/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu
      /usr/local/bin/box32 /home/user/.local/share/Steam/ubuntu12_32/steam
    \"
'"
```

### Compile Box64 with BOX32 (for 64-bit only devices)
```bash
docker run --rm --platform linux/arm64 \
  -v /tmp/box64-build:/output \
  ubuntu:22.04 bash -c "
    apt-get update &&
    apt-get install -y git cmake build-essential python3 &&
    git clone --depth 1 https://github.com/ptitSeb/box64.git /box64 &&
    mkdir /box64/build && cd /box64/build &&
    cmake .. -DARM_DYNAREC=ON -DBOX32=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo &&
    make -j\$(nproc) &&
    cp box64 /output/
  "
```

### Compile PRoot with futex patch
See `/tmp/termux-proot/` for patched source. Key changes:
- `src/syscall/sysnums.list` - Added `futex_time64`
- `src/syscall/sysnums-arm.h` and `sysnums-arm64.h` - Added syscall 422 mapping
- `src/syscall/enter.c` - Added explicit futex passthrough handling

## Problem History

### "semaphore creation failed Function not implemented" (SOLVED)
- Steam uses futex syscalls for semaphores/mutexes
- PRoot didn't have explicit handling for `futex_time64` (syscall 422)
- **Solution:** Patched PRoot to pass through futex syscalls cleanly

### Device is 64-bit only (no ARM32 support) (SOLVED)
- `getprop ro.product.cpu.abilist32` returns empty
- Box86 (ARM32 binary) cannot run
- **Solution:** Use Box64 with `BOX32=ON` compile flag

### glibc version mismatch (SOLVED)
- Cross-compiling on host links against newer glibc
- **Solution:** Compile inside Docker with `--platform linux/arm64 ubuntu:22.04`

### xz command not found in rootfs (SOLVED)
- Steam bootstrap is `.tar.xz` format
- Minimal rootfs doesn't have xz utility
- **Solution:** Use Java's `XZCompressorInputStream` from Apache Commons Compress

## App Components

### Bundled in APK
- `libproot.so` - Patched PRoot with futex support (222KB)
- `libproot-loader.so` - PRoot loader for ARM64
- `libproot-loader32.so` - PRoot loader for ARM32
- `libtalloc.so` - Talloc library for PRoot
- `box64.xz` - Box64 v0.4.1 with BOX32 (11MB compressed)
- `x86-libs.tar.xz` - 32-bit x86 libraries (3MB compressed)

### Extracted at Runtime
- Ubuntu 22.04 arm64 rootfs (downloaded on first run)
- Steam client (downloaded and extracted via app)
- Box64/Box32 binaries (extracted from assets)
- x86 libraries (extracted from assets)
