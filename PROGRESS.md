# Steam Launcher Progress Report

## Session: 2026-02-02

### What We Accomplished

#### 1. Box64 with BOX32 Support
- **Problem**: Device is 64-bit ARM only (`ro.product.cpu.abilist32` is empty), cannot run Box86 (ARM32)
- **Solution**: Compiled Box64 v0.4.1 with `-DBOX32=ON` flag which handles 32-bit x86 directly on ARM64
- **Result**: Box64 (75MB) successfully runs both 32-bit and 64-bit x86 code

#### 2. Compiled Binaries (via Docker)
```bash
# Box64 with BOX32 support (for 64-bit only ARM devices)
docker run --rm --platform linux/arm64 -v /tmp/box64-build:/output ubuntu:22.04 bash -c "
  apt-get update &&
  apt-get install -y git cmake build-essential python3 &&
  git clone --depth 1 https://github.com/ptitSeb/box64.git /box64 &&
  mkdir /box64/build && cd /box64/build &&
  cmake .. -DARM_DYNAREC=ON -DBOX32=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo &&
  make -j\$(nproc) &&
  cp box64 /output/
"
```

#### 3. App Assets Added
- `app/src/main/assets/box64.xz` - 11MB (Box64 v0.4.1 with BOX32)
- `app/src/main/assets/x86-libs.tar.xz` - 3MB (32-bit x86 libraries: libc, libstdc++, etc.)

#### 4. Code Changes Made
- **ContainerManager.kt**:
  - Extract Box64 from assets instead of compiling
  - Extract x86 libraries from assets
  - Create box32 symlink to box64
- **SteamService.kt**:
  - Updated to use `box32` instead of `box86`
  - Fixed Steam path detection (searches multiple locations)
- **SettingsActivity.kt**: Added "Test Box64" button
- **activity_settings.xml**: Added Test Box64 button to UI

#### 5. Test Results
- Box64 v0.4.1 with Dynarec: **WORKING**
- Box32 (symlink): **WORKING**
- Steam installed at: `/home/user/usr/lib/steam/bin_steam.sh`

### Current Issue: Rootfs Has Commands But PATH Issue

The rootfs actually has all utilities in `/bin/` (ls, cat, bash, env, etc.) but when running via proot, some commands aren't being found.

Last test showed `/bin/` contains: bash, ls, cat, env, head, nproc, lscpu, and many more.

The error was:
```
Box64 Error: Reading elf header of bin_steam.sh, Try to launch natively instead
/usr/bin/env: 'bash': No such file or directory
```

This means:
1. Box64 correctly identifies the script as not an ELF
2. Falls back to running natively
3. But `/usr/bin/env bash` fails - likely `/usr/bin/env` doesn't exist or PATH issue

### Next Steps To Try

1. **Check /usr/bin/env exists**:
```bash
adb shell "run-as com.mediatek.steamlauncher ls -la files/rootfs/usr/bin/env"
```

2. **Create /usr/bin/env symlink if missing**:
```bash
adb shell "run-as com.mediatek.steamlauncher ln -sf /bin/env files/rootfs/usr/bin/env"
```

3. **Test Steam script directly with bash**:
```bash
# Via proot:
/bin/bash /home/user/usr/lib/steam/bin_steam.sh --help
```

4. **Check if Steam bootstrap needs to download more files**:
The `bootstraplinux_ubuntu12_32.tar.xz` file exists - Steam may need to extract this first.

### Files to Commit

```bash
git add app/src/main/assets/x86-libs.tar.xz \
        app/src/main/java/com/mediatek/steamlauncher/ContainerManager.kt \
        app/src/main/java/com/mediatek/steamlauncher/SteamService.kt \
        PROGRESS.md

git commit -m "Add x86 libraries and fix Steam path detection"
```

### Key Paths on Device

- App data: `/data/data/com.mediatek.steamlauncher/`
- Rootfs: `files/rootfs/`
- Box64: `files/rootfs/usr/local/bin/box64`
- Box32: `files/rootfs/usr/local/bin/box32` (symlink to box64)
- Steam: `files/rootfs/home/user/usr/lib/steam/bin_steam.sh`
- x86 libs: `files/rootfs/lib/i386-linux-gnu/`
- libtalloc symlink: `files/lib-override/libtalloc.so.2`

### Important: libtalloc Symlink Issue

After each app reinstall, the libtalloc symlink breaks because the APK path changes. Fix with:
```bash
# Get current app path
adb shell "pm path com.mediatek.steamlauncher"
# Returns something like: /data/app/~~XXX==/com.mediatek.steamlauncher-YYY==/base.apk

# Fix symlink (replace path accordingly)
adb shell "run-as com.mediatek.steamlauncher sh -c '
  rm -f files/lib-override/libtalloc.so.2
  ln -sf /data/app/~~XXX==/com.mediatek.steamlauncher-YYY==/lib/arm64/libtalloc.so files/lib-override/libtalloc.so.2
'"
```

### Testing Commands

```bash
# Test Box64
adb shell "run-as com.mediatek.steamlauncher sh -c '
  export NATIVELIB=\$(dirname \$(pm path com.mediatek.steamlauncher | cut -d: -f2))/lib/arm64
  export LD_LIBRARY_PATH=files/lib-override:\$NATIVELIB
  export PROOT_LOADER=\$NATIVELIB/libproot-loader.so
  export PROOT_TMP_DIR=cache/proot-tmp
  export PROOT_NO_SECCOMP=1
  \$NATIVELIB/libproot.so --rootfs=files/rootfs -0 /usr/local/bin/box64 --version
'"
```

### APK Contents
- Total size: ~26MB
- box64.xz: 11MB
- x86-libs.tar.xz: 3MB
- App code + native libs: ~12MB
