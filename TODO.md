# Development TODO

## Current Status (2026-02-06)

### Architecture: FEX-Direct (No PRoot)

```
App → ProcessBuilder → ld.so → FEXLoader → x86-64 bash/Steam
```

PRoot eliminated. FEX binaries in `jniLibs/arm64-v8a/` (nativeLibDir) for SELinux exec
permission. Invoked via ld.so wrapper. Child re-exec via FEX_SELF_LDSO/FEX_SELF_LIBPATH.

### What's Working

- [x] **FEX-Emu (FEX-2506)** runs on Android via ld.so wrapper from nativeLibDir
- [x] **FEXServer management** - auto-started, persistent 5 minutes, stale lock cleanup
- [x] **Child process exec** - FEX_SELF_LDSO/FEX_SELF_LIBPATH for re-exec via ld.so
- [x] **LD_LIBRARY_PATH** set explicitly (RPATH alone insufficient on Android)
- [x] **Container setup via app UI** - full flow works end-to-end
- [x] **FEX rootfs download** (993MB SquashFS, zstd-compressed)
- [x] **NDK-built unsquashfs** (squashfs-tools 4.6.1 + xz 5.4.5 + zstd 1.5.5)
- [x] **Hardlink fallback** - unsquashfs patched to copy when link() fails on Android
- [x] **ETXTBSY handling** - skip busy files during FEX binary extraction
- [x] **x86-64 execution** - `uname -a` → x86_64, `cat /etc/os-release` → Ubuntu 22.04.5 LTS
- [x] **Child exec inside bash** - `$(uname -m)` = x86_64, `/usr/bin/uname -m` from bash = x86_64
- [x] **Semaphores** - sem_init/sem_wait/sem_post all PASS via FEX glibc futex emulation
- [x] **Vortek Vulkan passthrough** - Mali GPU rendering via Unix socket IPC
- [x] **vkcube renders** at 15-40 FPS on screen
- [x] **Interactive terminal** (TerminalActivity)
- [x] **Steam download & installation** - .deb extracted into FEX rootfs + bootstrap extracted
- [x] **Steam binary loads via FEX** - x86 (i386) binary emulated successfully
- [x] **DNS resolution** - fixed by writing /etc/resolv.conf into rootfs
- [x] **Seccomp bypass** - glibc binary-patched for set_robust_list/rseq (return -ENOSYS)
- [x] **FEXServer stale lock cleanup** - detects and kills orphaned FEXServers

### Previous Blocker: FEX mkdir/rmdir/unlink Not Redirected (SOLVED)

Steam failed with a **fatal assertion** in breakpad: `mkdir("/tmp/dumps")` failed.

**Root cause found (2026-02-06):** FEX's `mkdir`, `rmdir`, `unlink`, `link`, `symlink`,
`rename` syscalls were **NOT going through the rootfs overlay** at all. They called native
`::mkdir(pathname)` with the raw guest path (e.g., `/tmp/dumps`). On Android, `/tmp`
doesn't exist → `ENOENT`. Meanwhile, `open()`, `stat()`, `access()` etc. DID go through
`FileManager::GetEmulatedFDPath()` for rootfs path translation.

**Fix: Custom FEX build** — Added `Mkdir`, `Mkdirat`, `Rmdir`, `Unlink`, `Unlinkat`,
`Symlink`, `Symlinkat`, `Link`, `Linkat`, `Rename`, `Renameat`, `Renameat2` methods to
`FileManager` (same pattern as `Open`/`Mknod`: try rootfs path first via
`GetEmulatedFDPath()`, fall back to native). Updated `FS.cpp` and `Passthrough.cpp`.

**Winlator investigation:** Winlator uses PRoot/Box64 with a fully writable extracted
rootfs — they never use FEX's overlay, so they never face this issue.

---

### Previous Blocker: Android Seccomp (SOLVED)

FEX died with **exit code 159** (signal 31 = SIGSYS) when launched from the app.
App processes inherit strict seccomp filter from zygote (`SECCOMP_RET_KILL_PROCESS`).

**Blocked syscalls identified** (via `libseccomp_test.so` from app):
- `set_robust_list` (99) — glibc `__tls_init_tp()` during ld.so early init
- `rseq` (293) — glibc `__libc_early_init()` during ld.so early init
- Both called BEFORE any constructors → signal handlers can't catch them

**Fix: Binary-patch glibc** (`scripts/patch_glibc_seccomp.py`):
- Replaces `svc #0` with `movn x0, #37` (return -ENOSYS) for blocked syscalls
- glibc handles -ENOSYS gracefully for both syscalls
- Patched: `libld_linux_aarch64.so` (jniLibs) + `libc.so.6` (fex-bin.tgz)

### Previous Blocker: FEXServer Exit 255 / Stale Lock (SOLVED)

FEXServer exited silently with code 255 after the seccomp fix was applied.

**Root cause:** Orphaned FEXServer (from previous adb test) held read lock on
`$HOME/.fex-emu/Server/Server.lock`. New FEXServer couldn't acquire write lock →
`InitializeServerPipe()` returned false → silent exit -1. The `return -1` paths in
Main.cpp have NO log messages, making this completely silent even in foreground mode.

**Fix:** `cleanupStaleLockFiles()` in `FexExecutor.kt`:
- Checks if Server.lock is held (via `flock -n`)
- If locked: scans /proc to find holder PID, kills it
- Force-removes stale Server.lock and RootFS.lock

**Diagnostic tool:** `libfexserver_diag.so` — tests each FEXServer init step
(lock folder, lock file, abstract socket, filesystem socket, epoll, eventfd, setsid).
Run via "FEX Diag" button in Terminal.

### Current: Needs App Context Testing

All adb tests pass (FEXServer + FEXLoader + child exec). Need to verify from app
(with seccomp filter active) that the glibc patches and lock cleanup work correctly.
Test via Terminal: "FEX Diag" button, then `uname -a`.

---

## Remaining Tasks

### Phase 1: Core Infrastructure — DONE

1. ~~**Solve the /tmp/dumps issue**~~ — Custom FEX build with rootfs-redirected mkdir/rmdir/unlink
2. ~~**Fix seccomp kills**~~ — Binary-patch glibc (set_robust_list, rseq → -ENOSYS)
3. ~~**Fix FEXServer stale locks**~~ — cleanupStaleLockFiles() in FexExecutor.kt
4. **Verify from app context** — test Terminal buttons with seccomp active

**Custom FEX build details:**
- Cross-compiled from x86_64 host using `build_fex_cross.sh` (avoids QEMU segfaults)
- Clang + lld with Ubuntu multiarch ARM64 sysroot
- Packaged via `package_fex.sh` → `fex-bin.tgz`
- glibc binary-patched via `scripts/patch_glibc_seccomp.py`

### Phase 2: Steam Launch

3. **Steam bootstrap update** — Steam will try to update itself on first run
4. **X11 display connection** — DISPLAY=:0, X11 socket symlinks
5. **Vulkan passthrough for Steam** — VK_ICD_FILENAMES + Vortek socket
6. **Steam login** — keyboard input via X11

### Phase 3: Game Testing

7. **Install small game** via Steam
8. **DXVK rendering** — Direct3D → Vulkan → Vortek → Mali
9. **Performance** — frame rates, memory usage
10. **Input** — touch → X11 mouse events, keyboard

### Phase 4: Polish

11. **ContainerManager.downloadAndExtractSteam()** — integrate Steam install into setup flow
12. **Fix DNS in setup** — auto-write resolv.conf during container setup
13. **Fix rootfs /lib symlink protection** — prevent tar from replacing /lib → usr/lib
14. **Android 12+ phantom process fix** — verify foreground service
15. **Error handling** — graceful recovery from FEXServer crashes

---

## Technical Notes

### FEXLoader Invocation on Android

FEX binaries live in `nativeLibDir` (jniLibs/arm64-v8a/) for SELinux exec permission.
Invoked via bundled ld.so wrapper:
```bash
export LD_LIBRARY_PATH=${nativeLibDir}:${fexDir}/lib:${fexDir}/lib/aarch64-linux-gnu
export FEX_SELF_LDSO=${nativeLibDir}/libld_linux_aarch64.so
export FEX_SELF_LIBPATH="${LD_LIBRARY_PATH}"
${nativeLibDir}/libld_linux_aarch64.so --library-path ${LD_LIBRARY_PATH} \
    ${nativeLibDir}/libFEX.so /bin/bash -c "command"
```

Note: Do NOT use `--` before `/bin/bash` — FEXLoader treats `--` as end of its options,
but the shell may interpret it as a command name.

### PATH Must Be Set Inside Guest Bash

Without explicit PATH, guest bash resolves tools to `/system/bin/` (ARM64 Android binaries)
which fail with "Operation not permitted":
```bash
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
```
The app's `FexExecutor.kt` does this automatically via `buildBaseEnvironment()`.

### Steam Installation (from host side)

FEX rootfs is read-only from guest. Steam .deb must be extracted from Android/host side:
```bash
# On host: extract steam.deb
ar x steam.deb && tar xJf data.tar.xz -C rootfs/
# Extract bootstrap
tar xJf rootfs/usr/lib/steam/bootstraplinux_ubuntu12_32.tar.xz -C fex-home/.local/share/Steam/
```

### Rootfs /lib Symlink Warning

Ubuntu 22.04 rootfs has `/lib → usr/lib` symlink. Extracting tar archives with `./lib/` entries
into the rootfs will **replace the symlink with a real directory**, breaking the rootfs.
Always check after extraction:
```bash
ls -la rootfs/lib  # Should show: lib -> usr/lib
```

### DNS in FEX Rootfs

FEX rootfs doesn't have working DNS by default. Fix:
```bash
echo "nameserver 8.8.8.8" > rootfs/etc/resolv.conf
```
