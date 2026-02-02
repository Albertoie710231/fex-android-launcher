# Development TODO

## Current Status (2026-02-02)

### Completed
- [x] PRoot executor with proper environment (PROOT_TMP_DIR, LD_LIBRARY_PATH)
- [x] libtalloc.so.2 symlink fix
- [x] Ubuntu 22.04 arm64 rootfs download and extraction
- [x] Symlink handling in tar extraction (/bin -> /usr/bin)
- [x] Steam .deb download and extraction from Android side (OkHttp + Commons-Compress)
- [x] Container commands working (echo, uname, ls, bash via proot)
- [x] Git repository initialized
- [x] **Box64 compiled in Docker** (v0.4.1 with ARM Dynarec)
- [x] **Box64 with BOX32 support** (75MB, handles both 32-bit and 64-bit x86)
- [x] **box32 symlink created** (for 32-bit x86 Steam client)
- [x] 32-bit ARM libraries (armhf) installed in rootfs

### Device Limitation Discovered
- Device is **64-bit only** (`ro.product.cpu.abilist32` is empty)
- Cannot run ARM32 binaries natively (Box86 won't work)
- **Solution:** Box64 compiled with `BOX32=ON` flag handles 32-bit x86 directly

### Current Binaries Installed
```
/usr/local/bin/box64   - 75MB (Box64 v0.4.1 with BOX32 support)
/usr/local/bin/box32   - symlink to box64 (for 32-bit x86 apps)
/usr/local/bin/box86   - 24MB (ARM32 - won't run on this device)
```

### Working Commands
```bash
# Test Box64 via proot
adb shell "run-as com.mediatek.steamlauncher sh -c '
  export NATIVELIB=/data/app/~~O4vt_CLkfLU0ZSxV13AhUA==/com.mediatek.steamlauncher-WFKPZA0TZsNpqEG-BmrL6Q==/lib/arm64 &&
  export LD_LIBRARY_PATH=files/lib-override:\$NATIVELIB &&
  export PROOT_LOADER=\$NATIVELIB/libproot-loader.so &&
  export PROOT_TMP_DIR=cache/proot-tmp &&
  export PROOT_NO_SECCOMP=1 &&
  \$NATIVELIB/libproot.so --rootfs=files/rootfs -0 /usr/local/bin/box64 --version'"

# Output: Box64 arm64 v0.4.1 b321083 with Dynarec
```

## Next Steps

### 1. Install Steam in Container
The rootfs is minimal (no apt/dpkg). Options:
- Download Steam .deb and extract manually
- Use a more complete rootfs with package manager
- Use Steam bootstrap script

### 2. Install x86/x86_64 Libraries
Steam needs x86 Linux libraries. Need to add:
- `/lib/i386-linux-gnu/` - 32-bit x86 libs
- `/lib/x86_64-linux-gnu/` - 64-bit x86 libs
- `/usr/lib/i386-linux-gnu/` - additional 32-bit libs

### 3. Test Steam Launch
```bash
# Inside proot container
export BOX64_LOG=1
export BOX86_LOG=1
box32 /opt/steam/steam.sh
```

## Build Commands Reference

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

### Push to device
```bash
adb push /tmp/box64-build/box64 /data/local/tmp/
adb shell "run-as com.mediatek.steamlauncher cp /data/local/tmp/box64 files/rootfs/usr/local/bin/"
adb shell "run-as com.mediatek.steamlauncher chmod +x files/rootfs/usr/local/bin/box64"
adb shell "run-as com.mediatek.steamlauncher ln -sf box64 files/rootfs/usr/local/bin/box32"
```

## Problem History

### Device is 64-bit only (no ARM32 support)
- `getprop ro.product.cpu.abilist` returns only `arm64-v8a`
- `getprop ro.product.cpu.abilist32` returns empty
- Box86 (ARM32 binary) cannot run
- **Solution:** Use Box64 with BOX32=ON compile flag

### glibc version mismatch
- Cross-compiling Box64 on Arch Linux (GCC 15.1.0) links against glibc 2.38/2.39
- Ubuntu 22.04 rootfs has glibc 2.35
- Error: `GLIBC_2.38 not found`
- **Solution:** Compile inside Docker with `--platform linux/arm64 ubuntu:22.04`

### Static linking failed
- Tried `-DSTATICBUILD=ON` but Box64 hooks malloc/free/dlsym
- Got "multiple definition" errors
- Static linking is not viable for Box64

## Files Modified
- `app/src/main/java/com/mediatek/steamlauncher/ProotExecutor.kt`
- `app/src/main/java/com/mediatek/steamlauncher/ContainerManager.kt`
- `app/src/main/java/com/mediatek/steamlauncher/SettingsActivity.kt`
- `app/build.gradle.kts` (added XZ, Zstd dependencies)
- `.gitignore` (created)
