# Development TODO

## Current Status (2026-02-05)

### Architecture: FEX-Direct (No PRoot)

```
App → ProcessBuilder → ld-linux-aarch64.so.1 → FEXLoader → x86-64 bash/Steam
```

PRoot has been completely eliminated. FEX-Emu runs directly via ProcessBuilder:
- FEXLoader is an ARM64 native binary that JIT-compiles x86-64 → ARM64
- FEX provides its own x86-64 rootfs overlay (Ubuntu 22.04)
- Vortek IPC passthrough bridges glibc↔Bionic for Mali GPU access

### What's Working
- [x] **FEX-Emu (FEX-2506)** runs directly on Android via bundled ld-linux-aarch64.so.1
- [x] **FEXServer management** - auto-started before guest execution, persistent 5 minutes
- [x] **FEXLoader --version** verified on device: `FEX-Emu (FEX-2506)`
- [x] **NDK-built unsquashfs** - compiles from source (squashfs-tools 4.6.1 + xz-utils 5.4.5)
- [x] **FEX binary extraction** from bundled `fex-bin.tgz` asset
- [x] **Vortek Vulkan passthrough** - Mali GPU rendering via Unix socket IPC
- [x] **vkcube renders** at 15-40 FPS on screen
- [x] **FramebufferBridge** - HardwareBuffer → Surface display with Choreographer vsync
- [x] **Interactive terminal** (TerminalActivity) for running FEX commands
- [x] **APK builds and installs** cleanly (no PRoot artifacts)
- [x] **App launches** without crashes, shows container status

### What's NOT Yet Tested (Needs FEX Rootfs)
- [ ] **Container setup via app UI** - download + extract FEX x86-64 rootfs
- [ ] **x86-64 command execution** - needs rootfs extracted first (`/bin/uname` etc.)
- [ ] **Semaphore test** - FEX emulates glibc semaphores via futex (should work)
- [ ] **Vortek ICD in FEX rootfs** - `libvulkan_vortek.so` + ICD JSON config
- [ ] **Steam launch via FEX** - `FEXLoader -- /bin/bash -c "steam"`
- [ ] **DXVK game rendering** - Direct3D → Vulkan → Vortek → Mali GPU
- [ ] **X11 display** - X11 socket symlinks in FEX rootfs `/tmp/.X11-unix/`

---

## Remaining Tasks

### Phase 1: Container Setup (Next)

1. **Trigger container setup on device**
   - Press "Setup Container" in app
   - Verify: FEX binary extraction from `fex-bin.tgz`
   - Verify: SquashFS rootfs download (~995MB) from `rootfs.fex-emu.gg`
   - Verify: `unsquashfs` extraction (~2GB) into `fex-rootfs/Ubuntu_22_04/`
   - Verify: FEX Config.json + thunks.json written
   - Verify: Vortek ICD installed in rootfs

2. **Test basic x86-64 execution**
   - Open terminal, run `uname -a` → expect `x86_64 GNU/Linux`
   - Run `cat /etc/os-release` → expect Ubuntu 22.04
   - Run `ls /bin/` → expect full x86-64 userland

3. **Test semaphores**
   ```bash
   # Compile and run inside FEX x86-64 environment
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

### Phase 2: Vulkan in FEX

4. **Verify Vortek ICD setup**
   ```bash
   export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
   vulkaninfo --summary  # Should show Mali GPU
   ```

5. **Test vkcube via FEX**
   ```bash
   export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
   export LD_PRELOAD=/lib/libvulkan_headless.so
   ./vkcube --c 1000
   ```

### Phase 3: Steam

6. **Download and install Steam**
   - ContainerManager downloads Steam bootstrap
   - Extract to `${fexHomeDir}/.local/share/Steam/`

7. **Launch Steam via FEX**
   ```bash
   FEXLoader -- /bin/bash -c "
     export DISPLAY=:0
     export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
     steam
   "
   ```

8. **Test game with DXVK**
   - Install small game via Steam
   - Verify DXVK → Vulkan → Vortek → Mali rendering

### Phase 4: Polish

9. **Android 12+ phantom process fix** - verify foreground service keeps FEX alive
10. **Touch/keyboard input** via X11 server
11. **Performance optimization** - frame latency, memory usage
12. **Error handling** - graceful recovery from FEXServer crashes

---

## Technical Notes

### FEXLoader Invocation on Android

FEX binaries have `PT_INTERP=/opt/fex/lib/ld-linux-aarch64.so.1` hardcoded. On Android, `/opt/fex/` doesn't exist. Bypass via bundled dynamic linker:

```bash
${fexDir}/lib/ld-linux-aarch64.so.1 \
  --library-path ${fexDir}/lib:${fexDir}/lib/aarch64-linux-gnu \
  ${fexDir}/bin/FEXLoader \
  -- /bin/bash -c "command"
```

### FEXServer Requirement

FEXServer must be running before FEXLoader can execute guest binaries. It provides rootfs overlay mounting services. `FexExecutor` auto-starts it with `-f -p 300` (foreground, persistent 5 min).

The server socket is created at `$TMPDIR/<uid>.FEXServer.Socket`, so TMPDIR must be consistent between FEXServer and FEXLoader.

### Socket Access in FEX

FEX redirects `/tmp` → `${fexRootfsDir}/tmp/`. Symlinks in rootfs `/tmp/` point to actual Android sockets:
```
${fexRootfsDir}/tmp/.vortek/V0 → ${actualTmpDir}/.vortek/V0
${fexRootfsDir}/tmp/.X11-unix/X0 → ${actualX11SocketDir}/X0
```

### NDK-Built unsquashfs

squashfs-tools 4.6.1 compiled with Android NDK as `libunsquashfs.so`:
- Requires vendored xz-utils 5.4.5 source with hand-crafted `config.h`
- Android Bionic quirks: no `-lpthread` (built into libc), no `lutimes()`, needs `_GNU_SOURCE`
- Sources downloaded via `scripts/fetch_unsquashfs_source.sh`

### Why FEX Instead of Box64

| Aspect | Box64 | FEX |
|--------|-------|-----|
| pthread handling | Wraps to host Bionic → semaphores FAIL | Emulates x86 glibc → futex → WORKS |
| Rootfs | Needs ARM64 rootfs + x86 libs | Provides own x86-64 rootfs |
| Disk usage | ~1.5GB (ARM64 + FEX rootfs) | ~1GB (FEX rootfs only) |
| Syscall overhead | PRoot ptrace on every syscall | Native ARM64, no ptrace |

---

## Resources

- [FEX-Emu](https://github.com/FEX-Emu/FEX) - x86-64 emulation with glibc emulation
- [Winlator](https://github.com/brunodev85/winlator) - Vortek Vulkan IPC passthrough source
- [FEX rootfs](https://rootfs.fex-emu.gg) - x86-64 SquashFS rootfs downloads
- [Termux:X11](https://github.com/termux/termux-x11) - X11 server inspiration
