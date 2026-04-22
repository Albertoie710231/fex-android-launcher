package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Launcher for the GameNative-style native ARM64 Wine pipeline.
 *
 * Runs Pepelespooder's Bionic Wine binary (shipped in jniLibs as libwine_native.so)
 * directly from the app process, mirroring GameNative's BionicProgramLauncherComponent.
 *
 * First-iteration scope: wineVersion(). No prefix, no X11, no wineserver — just
 * the wine binary printing its compiled-in version. If this works, we scaffold
 * forward to wineserver + notepad.
 */
class NativeWinePipeline(private val context: Context) {

    companion object {
        private const val TAG = "NativeWinePipeline"
        private const val WINE_LIB = "libwine_native.so"
        private const val WINESERVER_LIB = "libwineserver_native.so"
        private const val REDIRECT_LIB = "libpathredirect.so"

        /** Prefix baked into Pepelespooder's wineserver binary. Strings pulled
         *  confirm paths like:
         *    /data/data/app.gamenative/files/imagefs/usr/lib
         *    /data/data/app.gamenative/files/imagefs/opt/proton-10.0.99-arm64ec/...
         *  All share this common root. */
        private const val BAKED_ROOT = "/data/data/app.gamenative/files/imagefs"

        /** Baked prefix inside GameNative's proton-9.0-arm64ec wineserver ELF.
         *  Used when useProton9 = true. */
        private const val BAKED_ROOT_P9 = "/data/data/com.winlator.cmod/files/imagefs"
    }

    private val nativeLibDir: String
        get() = context.applicationInfo.nativeLibraryDir

    private val dataDir: String
        get() = context.filesDir.absolutePath

    /** App cache dir — TerminalActivity.startVortekRenderer creates the Vortek
     *  Unix socket under $cacheDir/tmp/vortek.sock. */
    private val cacheDir: String
        get() = context.cacheDir.absolutePath

    private val winePath: String
        get() = "$nativeLibDir/$WINE_LIB"

    private val wineServerPath: String
        get() = "$nativeLibDir/$WINESERVER_LIB"

    private val redirectLib: String
        get() = "$nativeLibDir/$REDIRECT_LIB"

    /** Target of the path redirect — where the baked paths should resolve to. */
    private val imageFsMirror: String
        get() = "$dataDir/proton11/imagefs"

    data class Result(val exitCode: Int, val stdout: String, val stderr: String) {
        override fun toString() = "exit=$exitCode\n--- stdout ---\n$stdout--- stderr ---\n$stderr"
    }

    fun wineBinaryExists(): Boolean = File(winePath).exists()

    /**
     * Run `wine --version`. Does not need a WINEPREFIX or wineserver — wine
     * prints its compiled-in version string and exits.
     */
    fun wineVersion(): Result {
        if (!wineBinaryExists()) {
            return Result(-1, "", "wine binary missing: $winePath")
        }

        val env = buildEnv()
        Log.i(TAG, "Running $winePath --version with env keys: ${env.keys}")

        val pb = ProcessBuilder(winePath, "--version")
            .directory(File(dataDir))
            .redirectErrorStream(false)
        pb.environment().clear()
        pb.environment().putAll(env)

        return try {
            val proc = pb.start()
            val out = proc.inputStream.bufferedReader().readText()
            val err = proc.errorStream.bufferedReader().readText()
            val code = proc.waitFor()
            Result(code, out, err)
        } catch (t: Throwable) {
            Log.e(TAG, "wine --version failed", t)
            Result(-2, "", t.stackTraceToString())
        }
    }

    /**
     * Refresh the bin/ symlinks (wine, wine64, wine-preloader, wineserver)
     * to point at the CURRENT nativeLibDir. After every APK reinstall the
     * nativeLibDir's random hash changes and any stale symlinks cause
     * "wine: could not exec the wine loader" on execve.
     */
    fun refreshBinSymlinks(): Boolean {
        return try {
            val binDir = File("$dataDir/proton11/bin")
            binDir.mkdirs()
            val binTargets = mapOf(
                "wine" to "$nativeLibDir/$WINE_LIB",
                "wine64" to "$nativeLibDir/$WINE_LIB",
                "wine-preloader" to "$nativeLibDir/libwine_preloader.so",
                "wine64-preloader" to "$nativeLibDir/libwine_preloader.so",
                "wineserver" to "$nativeLibDir/$WINESERVER_LIB",
                WINE_LIB to "$nativeLibDir/$WINE_LIB",
                "libwine_preloader.so" to "$nativeLibDir/libwine_preloader.so",
                WINESERVER_LIB to "$nativeLibDir/$WINESERVER_LIB",
            )
            for ((link, target) in binTargets) {
                recreateSymlink(File(binDir, link), target)
            }
            // Vulkan loader: wine does dlopen("libvulkan.so.1"). Symlink the
            // current-install libvulkan_loader.so in proton11/lib/ so
            // LD_LIBRARY_PATH picks it up. Stale across APK reinstalls if
            // not refreshed each launch.
            val libDir = File("$dataDir/proton11/lib")
            libDir.mkdirs()
            recreateSymlink(File(libDir, "libvulkan.so.1"), "$nativeLibDir/libvulkan_loader.so")

            // Proton-9.0-arm64ec bin symlinks for untrusted_app exec path.
            // The ELFs live in nativeLibDir (exec-allowed); the symlinks in
            // proton9/bin/ let wine self-find its wine-preloader sibling.
            val p9BinDir = File("$dataDir/proton9/bin")
            if (p9BinDir.exists()) {
                val p9Targets = mapOf(
                    "wine" to "$nativeLibDir/libwine_proton9_native.so",
                    "wine64" to "$nativeLibDir/libwine_proton9_native.so",
                    "wine-preloader" to "$nativeLibDir/libwine_proton9_preloader.so",
                    "wine64-preloader" to "$nativeLibDir/libwine_proton9_preloader.so",
                    "wineserver" to "$nativeLibDir/libwineserver_proton9_native.so",
                )
                for ((link, target) in p9Targets) {
                    recreateSymlink(File(p9BinDir, link), target)
                }
            }
            true
        } catch (t: Throwable) {
            Log.e(TAG, "refreshBinSymlinks failed", t)
            false
        }
    }

    private fun recreateSymlink(link: File, target: String) {
        if (link.exists() || java.nio.file.Files.isSymbolicLink(link.toPath())) {
            link.delete()
        }
        java.nio.file.Files.createSymbolicLink(link.toPath(), java.nio.file.Paths.get(target))
    }

    /**
     * Create the directory layout the baked wineserver expects, using
     * symlinks into our real proton11 tree so no file is duplicated.
     *
     * After this runs, these paths resolve (via the LD_PRELOAD redirect):
     *   /data/data/app.gamenative/files/imagefs/usr/lib
     *     → proton11/imagefs/usr → proton11/lib  (contains aarch64-unix, etc.)
     *   /data/data/app.gamenative/files/imagefs/opt/proton-10.0.99-arm64ec/share/wine/nls
     *     → proton11/imagefs/opt/proton-10.0.99-arm64ec/share → proton11/share
     *   /data/data/app.gamenative/files/imagefs/opt/proton-10.0.99-arm64ec/bin
     *     → proton11/imagefs/opt/proton-10.0.99-arm64ec/bin → proton11/bin
     */
    fun ensureImageFsMirror(): Boolean {
        return try {
            val mirror = File(imageFsMirror)
            val proton11 = File("$dataDir/proton11")
            if (!proton11.exists()) {
                Log.e(TAG, "proton11 tree not staged at ${proton11.path}")
                return false
            }
            // /imagefs/usr → ../lib  (wineserver expects /imagefs/usr/lib)
            val usrDir = File(mirror, "usr")
            if (!usrDir.exists()) {
                File(mirror, "usr").parentFile?.mkdirs()
                // Need imagefs/usr/lib to point at proton11/lib.
                // Easier: make imagefs/usr a directory, imagefs/usr/lib a symlink.
                usrDir.mkdirs()
                val usrLib = File(usrDir, "lib")
                if (!usrLib.exists() && !java.nio.file.Files.isSymbolicLink(usrLib.toPath())) {
                    java.nio.file.Files.createSymbolicLink(
                        usrLib.toPath(),
                        java.nio.file.Paths.get("$dataDir/proton11/lib"),
                    )
                }
            }
            // /imagefs/opt/proton-10.0.99-arm64ec/{share,bin}
            val protonRoot = File(mirror, "opt/proton-10.0.99-arm64ec")
            protonRoot.mkdirs()
            val shareLink = File(protonRoot, "share")
            if (!shareLink.exists() && !java.nio.file.Files.isSymbolicLink(shareLink.toPath())) {
                java.nio.file.Files.createSymbolicLink(
                    shareLink.toPath(),
                    java.nio.file.Paths.get("$dataDir/proton11/share"),
                )
            }
            val binLink = File(protonRoot, "bin")
            if (!binLink.exists() && !java.nio.file.Files.isSymbolicLink(binLink.toPath())) {
                java.nio.file.Files.createSymbolicLink(
                    binLink.toPath(),
                    java.nio.file.Paths.get("$dataDir/proton11/bin"),
                )
            }
            val libLink = File(protonRoot, "lib")
            if (!libLink.exists() && !java.nio.file.Files.isSymbolicLink(libLink.toPath())) {
                java.nio.file.Files.createSymbolicLink(
                    libLink.toPath(),
                    java.nio.file.Paths.get("$dataDir/proton11/lib"),
                )
            }
            File("$dataDir/tmp").mkdirs()
            File("$dataDir/dxvk_logs").mkdirs()
            // Dirs GameNative env vars reference (sysvshm / pulse / dxvk
            // config / xuser home). Most should already exist from the
            // imagefs_bionic extract, but mkdirs() is idempotent.
            File("$dataDir/imagefs_bionic/tmp/.sysvshm").mkdirs()
            File("$dataDir/imagefs_bionic/tmp/.sound").mkdirs()
            File("$dataDir/imagefs_bionic/usr/tmp").mkdirs()
            File("$dataDir/imagefs_bionic/home/xuser/.config").mkdirs()
            File("$dataDir/imagefs_bionic/home/xuser/.cache").mkdirs()
            // libevshim (GameNative's input-event shim) looks for a
            // memory-mapped file at imagefs/tmp/gamepad.mem to read
            // gamepad state from. GameNative's Java side creates + mmaps
            // it at runtime (WinHandler.start, 64 bytes). Without it,
            // every wine thread that loads libevshim retries forever
            // ("Failed to open memory file ..."), stalling game startup
            // before YamanekoCoreSystem Init. Since we don't inject
            // gamepad state (no physical controller wiring here), just
            // create the backing files as 64-byte zero files — libevshim
            // mmaps them fine and reads an all-zeros "no buttons pressed"
            // state forever, which is what a stubbed gamepad should be.
            for (name in listOf("gamepad.mem", "gamepad1.mem", "gamepad2.mem", "gamepad3.mem")) {
                val f = File("$dataDir/imagefs_bionic/tmp/$name")
                if (!f.exists() || f.length() != 64L) {
                    f.writeBytes(ByteArray(64))
                }
            }
            // Pre-create the drive_c skeleton. Wine's wineboot expects
            // C:\windows to be SetCurrentDirectory-able; if drive_c doesn't
            // exist the dosdevices/c: symlink (../drive_c) dangles and
            // SetCurrentDirectoryW fails with ENOENT (error 2).
            val driveC = File("$dataDir/proton11/prefix/.wine/drive_c")
            driveC.mkdirs()
            File(driveC, "windows").mkdirs()
            val system32 = File(driveC, "windows/system32")
            system32.mkdirs()
            File(driveC, "users").mkdirs()

            // Symlink a few core wine builtin exes into drive_c/windows/system32.
            // Wine's ShellExecuteEx resolves argv[1] by looking in system32
            // first; without cmd.exe/notepad.exe here ShellExecute returns
            // "File not found" even though the builtins live under
            // lib/wine/aarch64-windows/.
            val wineBuiltinDir = "$dataDir/proton11/lib/wine/aarch64-windows"
            val builtinExes = listOf(
                "cmd.exe", "notepad.exe", "wineboot.exe", "winemenubuilder.exe",
                "reg.exe", "rpcss.exe", "services.exe", "svchost.exe",
                "wineconsole.exe", "explorer.exe",
            )
            for (exe in builtinExes) {
                val src = File(wineBuiltinDir, exe)
                val dst = File(system32, exe)
                if (!src.exists()) continue
                if (dst.exists() || java.nio.file.Files.isSymbolicLink(dst.toPath())) continue
                try {
                    java.nio.file.Files.createSymbolicLink(
                        dst.toPath(),
                        java.nio.file.Paths.get(src.absolutePath),
                    )
                } catch (e: Exception) {
                    Log.w(TAG, "symlink $exe to system32 failed: ${e.message}")
                }
            }
            // Dosdevices: refresh Z: to point at the Android root — baked
            // value pointed at /data/data/app.gamenative/files/imagefs/
            val zDev = File("$dataDir/proton11/prefix/.wine/dosdevices/z:")
            if (java.nio.file.Files.isSymbolicLink(zDev.toPath())) {
                val target = java.nio.file.Files.readSymbolicLink(zDev.toPath()).toString()
                if (target.contains("app.gamenative")) {
                    zDev.delete()
                    java.nio.file.Files.createSymbolicLink(zDev.toPath(), java.nio.file.Paths.get("/"))
                }
            }

            // Vulkan ICD + layer config pointing at our native ARM64 libs.
            // Wine's Vulkan loader reads VK_ICD_FILENAMES and VK_LAYER_PATH
            // to find driver + layers. The paths here point directly at
            // nativeLibDir (app_native_lib SELinux context — exec OK).
            val vkConfigDir = File("$dataDir/proton11/vk")
            vkConfigDir.mkdirs()
            // Point at the ICD WRAPPER, not the raw Vortek client. The wrapper
            // (libvortek_icd_wrapper.so) implements vk_icdGetInstanceProcAddr
            // and calls vortekInitOnce() on the real libvulkan_vortek.so before
            // the loader asks for vkCreateInstance. The raw client returns NULL
            // from its own vk_icdGetInstanceProcAddr because Vortek was
            // originally designed to be loaded directly by Winlator, not by a
            // Khronos ICD loader.
            File(vkConfigDir, "vortek_icd.json").writeText(
                """
                {
                    "file_format_version": "1.0.0",
                    "ICD": {
                        "library_path": "$nativeLibDir/libvortek_icd_wrapper.so",
                        "api_version": "1.3.128"
                    }
                }
                """.trimIndent()
            )
            // The Bionic Vulkan loader only discovers IMPLICIT layers via
            // XDG_DATA_DIRS/vulkan/implicit_layer.d/*.json (VK_LAYER_PATH
            // only covers explicit layers). Write our manifest into the
            // imagefs_bionic implicit_layer.d dir alongside MangoHud,
            // libbcn_layer, feature_spoof, etc.
            val implicitLayerDir = File(
                "$dataDir/imagefs_bionic/usr/share/vulkan/implicit_layer.d"
            )
            implicitLayerDir.mkdirs()
            File(implicitLayerDir, "VK_LAYER_HEADLESS_surface.json").writeText(
                """
                {
                    "file_format_version": "1.2.0",
                    "layer": {
                        "name": "VK_LAYER_HEADLESS_surface",
                        "type": "GLOBAL",
                        "library_path": "$nativeLibDir/libvulkan_headless_layer.so",
                        "api_version": "1.3.0",
                        "implementation_version": "1",
                        "description": "Headless surface bridge for DXVK on native ARM64 wine",
                        "enable_environment": { "ENABLE_HEADLESS_LAYER": "1" },
                        "disable_environment": { "DISABLE_HEADLESS_LAYER": "1" }
                    }
                }
                """.trimIndent()
            )

            ensureYs9Stubs()
            ensureNullAlsaConfig()
            ensurePatchedWineBinaries()

            true
        } catch (t: Throwable) {
            Log.e(TAG, "ensureImageFsMirror failed", t)
            false
        }
    }

    /**
     * Deploy game-specific stub DLLs into both the game's exe-dir and the
     * prefix's system32. Each stub replaces a real DLL that otherwise crashes
     * or hangs on this pipeline:
     *   - xaudio2_7.dll: wine's builtin NULL-derefs at +0x33364 when ALSA can't
     *     open a backend. Stub returns S_OK / mock IXAudio2 for every call.
     *   - Galaxy64.dll: real DLL's GOG Galaxy IPC blocks main thread before
     *     the render loop starts (old FEX pipeline's ProtonManager.kt line 933
     *     explicitly calls this out).
     *   - steam_api64.dll: real DLL blocks on Steam runtime IPC.
     *   - GFSDK_SSAO_D3D11.win64.dll: NVIDIA GameWorks SSAO hits paths DXVK
     *     doesn't implement identically.
     *
     * The stubs are x86_64 PE — wine runs ys9.exe in experimental ARM64EC
     * mode which loads x86_64 via emulation. A pure ARM64 stub fails with
     * STATUS_INVALID_IMAGE_FORMAT.
     *
     * Original files are backed up as `.orig` on first deploy so a user can
     * revert if a real Steam runtime ever becomes available.
     */
    private fun ensureYs9Stubs(): Boolean {
        try {
            val sys32 = "$dataDir/proton11/prefix/.wine/drive_c/windows/system32"
            val gameDir = "$dataDir/fex-rootfs/Ubuntu_22_04/home/user/Steam/steamapps/common/Ys IX Monstrum Nox"

            val stubs = listOf(
                "xaudio2_7.dll",
                "Galaxy64.dll",
                "steam_api64.dll",
                "GFSDK_SSAO_D3D11.win64.dll",
            )

            // Sentinel file so we don't re-deploy on every launch.
            val sentinel = File("$dataDir/.ys9_stubs_deployed_v1")
            if (sentinel.exists()) {
                return true
            }

            File(sys32).mkdirs()

            for (name in stubs) {
                // system32: overwrite whatever is there (wine's builtin
                // re-populates system32 from lib/wine/aarch64-windows on every
                // wineboot, so we must re-overwrite every time the sentinel
                // is cleared).
                context.assets.open(name).use { input ->
                    File(sys32, name).outputStream().use { out -> input.copyTo(out) }
                }

                // Game dir: only deploy if the publisher's DLL is present,
                // and back it up once as `.orig`.
                val gameFile = File(gameDir, name)
                if (gameFile.exists()) {
                    val backup = File(gameDir, "$name.orig")
                    if (!backup.exists()) {
                        gameFile.copyTo(backup, overwrite = false)
                    }
                    context.assets.open(name).use { input ->
                        gameFile.outputStream().use { out -> input.copyTo(out) }
                    }
                }
            }

            // Proton 9 (wine-9)'s CoCreateInstance for IXAudio2 goes through
            // xapofx1_5.dll (DirectX Audio FX Pack) before xaudio2_7. Wine-9's
            // builtin xapofx1_5 doesn't export DllGetClassObject, so the
            // COM lookup fails and audio init exits without signaling. Reuse
            // the xaudio2_7 stub as xapofx1_5 — same mock-IXAudio2 exports.
            context.assets.open("xaudio2_7.dll").use { input ->
                File(sys32, "xapofx1_5.dll").outputStream().use { out -> input.copyTo(out) }
            }

            // steam_api64 needs steamclient.dll/steamclient64.dll in
            // C:\steamclient per the Valve registry entries, even though the
            // stub doesn't actually load them. We reuse the steam_api64 stub
            // as a placeholder — its Steamworks-shaped exports satisfy wine's
            // delay-load resolver.
            val steamclientDir = File("$dataDir/proton11/prefix/.wine/drive_c/steamclient")
            steamclientDir.mkdirs()
            for (fn in listOf("steamclient.dll", "steamclient64.dll")) {
                context.assets.open("steam_api64.dll").use { input ->
                    File(steamclientDir, fn).outputStream().use { out -> input.copyTo(out) }
                }
            }

            // steam_appid.txt — some DRM checks read this from the game dir.
            // 993940 is what the game's existing appid file had pre-deploy.
            File(gameDir).mkdirs()
            File(gameDir, "steam_appid.txt").writeText("993940\n")

            // steam.pipe — Steam IPC handshake dummy. Path is wherever the
            // game stat()s for it; placing one under files/.steam covers the
            // most common location.
            File("$dataDir/.steam").mkdirs()
            File("$dataDir/.steam/steam.pipe").writeText("1\n")

            sentinel.writeText("deployed ${System.currentTimeMillis()}")
            Log.i(TAG, "ensureYs9Stubs deployed ${stubs.size} stubs + steamclient")
            return true
        } catch (t: Throwable) {
            Log.e(TAG, "ensureYs9Stubs failed", t)
            return false
        }
    }

    /**
     * Deploy pre-patched services.exe + explorer.exe into the proton tree.
     * Both wine-built PEs have a pre-fix `DebugInfo->Spare[0] = __FILE__`
     * write that dereferences the `(void *)-1` sentinel wine-10 now assigns
     * when not allocating RTL_CRITICAL_SECTION_DEBUG — instant page fault.
     *
     * The NOP patch was generated offline via
     * `fex-emu/patch_wine_debuginfo_spare.py`. We bundle the patched PEs as
     * assets rather than running python at runtime.
     *
     * Runs unconditionally (no sentinel) because wineboot re-copies the
     * lib/wine/aarch64-windows originals into the prefix system32 on every
     * boot. If `useProton9=true` was ever tested in-between, the lib/wine
     * tree gets reset too, so we always write the patched versions back.
     */
    private fun ensurePatchedWineBinaries(): Boolean {
        try {
            val wineWinDir = "$dataDir/proton11/lib/wine/aarch64-windows"
            File(wineWinDir).mkdirs()
            for (name in listOf("services.exe", "explorer.exe")) {
                context.assets.open(name).use { input ->
                    File(wineWinDir, name).outputStream().use { out -> input.copyTo(out) }
                }
            }
            // Wipe system32 copies so wineboot is forced to re-copy from
            // our patched lib/wine tree. Without this wineboot sees the
            // existing prefix copies (possibly corrupted by a prior
            // useProton9=true test) and reuses them.
            val sys32 = "$dataDir/proton11/prefix/.wine/drive_c/windows/system32"
            for (name in listOf("services.exe", "explorer.exe")) {
                File(sys32, name).delete()
            }
            Log.i(TAG, "ensurePatchedWineBinaries deployed services.exe + explorer.exe")
            return true
        } catch (t: Throwable) {
            Log.e(TAG, "ensurePatchedWineBinaries failed", t)
            return false
        }
    }

    /**
     * Overwrite imagefs_bionic's ALSA config to use a null PCM as the
     * default device. The shipped config points the default PCM at the
     * `android_aserver` ALSA plugin which needs an `aserverd` daemon we
     * don't run — so PCM open fails, winealsa returns no-devices, and wine's
     * builtin xaudio2_7 NULL-derefs. With a null PCM, ALSA opens a silent
     * device (it still tries /dev/full but wine handles that case), and our
     * xaudio2_7 stub is never asked to do audio anyway.
     *
     * Three files to patch — ALSA loads /usr/share/alsa/alsa.conf which in
     * turn loads android_aserver.conf from both /usr/share/alsa/ and
     * /usr/etc/alsa/conf.d/.
     */
    private fun ensureNullAlsaConfig(): Boolean {
        try {
            val sentinel = File("$dataDir/.alsa_null_pcm_v1")
            if (sentinel.exists()) {
                return true
            }

            val alsaConf = """
                pcm.!default {
                    type null
                }
                ctl.!default {
                    type null
                }
                pcm.null {
                    type null
                }
                ctl.null {
                    type null
                }
            """.trimIndent()

            val androidAserverConf = """
                pcm.android_aserver {
                    type null
                }
                ctl.android_aserver {
                    type null
                }
                pcm.!default {
                    type null
                }
                ctl.!default {
                    type null
                }
            """.trimIndent()

            val shareAlsa = "$dataDir/imagefs_bionic/usr/share/alsa"
            val confD = "$dataDir/imagefs_bionic/usr/etc/alsa/conf.d"
            File(shareAlsa).mkdirs()
            File(confD).mkdirs()

            // Back up originals once
            for (path in listOf(
                "$shareAlsa/alsa.conf",
                "$shareAlsa/android_aserver.conf",
                "$confD/android_aserver.conf",
            )) {
                val f = File(path)
                val backup = File("$path.orig")
                if (f.exists() && !backup.exists()) {
                    f.copyTo(backup, overwrite = false)
                }
            }

            File("$shareAlsa/alsa.conf").writeText(alsaConf)
            File("$shareAlsa/android_aserver.conf").writeText(androidAserverConf)
            File("$confD/android_aserver.conf").writeText(androidAserverConf)

            sentinel.writeText("patched ${System.currentTimeMillis()}")
            Log.i(TAG, "ensureNullAlsaConfig patched 3 files")
            return true
        } catch (t: Throwable) {
            Log.e(TAG, "ensureNullAlsaConfig failed", t)
            return false
        }
    }

    /**
     * Launch wineserver in foreground (`-f`) with the path-redirect shim
     * preloaded, wait a short time, and report whether it stayed alive.
     *
     * Pass criterion: process still running after the timeout, or exited with
     * 0. Failure signature we expect if the redirect does NOT work:
     * "wineserver: failed to load l_intl.nls" on stderr followed by exit.
     */
    fun wineServerSmoke(timeoutMs: Long = 2500): Result {
        if (!File(wineServerPath).exists()) {
            return Result(-1, "", "wineserver binary missing: $wineServerPath")
        }
        if (!File(redirectLib).exists()) {
            return Result(-1, "", "redirect shim missing: $redirectLib")
        }
        if (!refreshBinSymlinks()) {
            return Result(-1, "", "failed to refresh bin symlinks")
        }
        if (!ensureImageFsMirror()) {
            return Result(-1, "", "failed to build imagefs mirror")
        }

        val env = buildEnv().toMutableMap().apply {
            put("LD_PRELOAD", redirectLib)
            put("REDIRECT_FROM", BAKED_ROOT)
            put("REDIRECT_TO", imageFsMirror)
            put("REDIRECT_DEBUG", "1")
            put("WINEPREFIX", "$dataDir/proton11/prefix/.wine")
        }
        Log.i(TAG, "Launching wineserver smoke: $wineServerPath -f")

        val pb = ProcessBuilder(wineServerPath, "-f", "-d1")
            .directory(File(dataDir))
            .redirectErrorStream(false)
        pb.environment().clear()
        pb.environment().putAll(env)

        return try {
            val proc = pb.start()
            val stdoutReader = Thread {
                try { proc.inputStream.bufferedReader().readText() } catch (_: Throwable) {}
            }.apply { start() }
            val errBuf = StringBuilder()
            val stderrReader = Thread {
                try {
                    val r = proc.errorStream.bufferedReader()
                    while (true) {
                        val line = r.readLine() ?: break
                        synchronized(errBuf) { errBuf.append(line).append('\n') }
                    }
                } catch (_: Throwable) {}
            }.apply { start() }

            val alive = !proc.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
            val err = synchronized(errBuf) { errBuf.toString() }
            if (alive) {
                proc.destroyForcibly()
                Result(0, "wineserver still alive after ${timeoutMs}ms (good)", err)
            } else {
                Result(proc.exitValue(), "wineserver exited early", err)
            }
        } catch (t: Throwable) {
            Log.e(TAG, "wineserver smoke failed", t)
            Result(-2, "", t.stackTraceToString())
        }
    }

    /**
     * Run an arbitrary wine argv with the path-redirect shim. Returns when
     * wine exits or timeout expires. Extra env entries override the defaults
     * (used to set DISPLAY for GUI apps, WINEDEBUG channels, etc.).
     */
    /** useProton9 = true runs GameNative's downloaded proton-9.0-arm64ec
     *  ELF binary from files/proton9/bin/ instead of Pepelespooder's
     *  libwine_native.so. Needed for ARM64EC x86-64 Windows games whose
     *  DirectX feature level negotiation behaves differently between
     *  wine 9 and wine 10. */
    fun wineRun(
        args: List<String>,
        timeoutMs: Long = 30000,
        extraEnv: Map<String, String> = emptyMap(),
        useProton9: Boolean = false,
    ): Result {
        val wineBinary = if (useProton9) "$dataDir/proton9/bin/wine" else winePath
        val wineTreeDir = if (useProton9) "$dataDir/proton9" else "$dataDir/proton11"
        // Check the real ELF in nativeLibDir — proton9/bin/wine is a symlink
        // that refreshBinSymlinks creates below.
        val wineExecCheck =
            if (useProton9) "$nativeLibDir/libwine_proton9_native.so" else wineBinary
        if (!File(wineExecCheck).exists()) {
            return Result(-1, "", "wine binary missing: $wineExecCheck")
        }
        if (!File(redirectLib).exists()) {
            return Result(-1, "", "redirect shim missing: $redirectLib")
        }
        if (!refreshBinSymlinks()) {
            return Result(-1, "", "failed to refresh bin symlinks")
        }
        if (!ensureImageFsMirror()) {
            return Result(-1, "", "failed to build imagefs mirror")
        }
        val env = buildEnv(useProton9).toMutableMap().apply {
            // Match GameNative's LD_PRELOAD chain so the game gets the shims
            // it expects for SYSV shared memory (libandroid-sysvshm) and
            // input event routing (libevshim). These are already in our
            // imagefs_bionic mirror — just never loaded. libpathredirect
            // sits last (wine-facing path rewrites) to keep our redirect
            // semantics. Order matters: sysvshm must hook before libc is
            // used by wine, evshim before the game opens input.
            val sysvshm = "$dataDir/imagefs_bionic/usr/lib/libandroid-sysvshm.so"
            val evshim  = "$dataDir/imagefs_bionic/usr/lib/libevshim.so"
            // libredirect-bionic.so — Winlator/GameNative's path-rewrite shim.
            // Hooks openat/fstatat/ioctl/read to rewrite hardcoded
            // com.winlator.cmod paths that evshim/sysvshm embed via dlsym
            // (bypassing our LD_PRELOAD). 2026-04-22: pulled from GameNative
            // and added to match their LD_PRELOAD chain exactly.
            val redirectBionic = "$dataDir/imagefs_bionic/usr/lib/libredirect-bionic.so"
            put("LD_PRELOAD", "$sysvshm:$evshim:$redirectBionic:$redirectLib")
            put("WINE_LD_PRELOAD", "$sysvshm:$evshim:$redirectBionic:$redirectLib")
            // GameNative provides these as runtime-configurable paths. Use
            // the same subpaths under imagefs_bionic/tmp so anything hard-
            // coded to /data/data/com.winlator/files/imagefs/tmp resolves
            // via REDIRECT_FROM3.
            put("ANDROID_SYSVSHM_SERVER", "$dataDir/imagefs_bionic/tmp/.sysvshm/SM0")
            put("EVSHIM_MAX_PLAYERS", "1")
            put("EVSHIM_SHM_ID", "1")
            put("EVSHIM_SHM_NAME", "controller-shm0")
            // PulseAudio + ALSA paths — even though we stub xaudio2, the
            // libasound init-time lookup still probes these and fails
            // weirdly if unset. Pointing them at non-existent sockets is
            // fine; libasound returns a clean error.
            put("PULSE_LATENCY_MSEC", "144")
            put("PULSE_SERVER", "$dataDir/imagefs_bionic/tmp/.sound/PS0")
            put("ALSA_CONFIG_PATH",
                "$dataDir/imagefs_bionic/usr/share/alsa/alsa.conf:" +
                "$dataDir/imagefs_bionic/usr/etc/alsa/conf.d/android_aserver.conf")
            put("ALSA_PLUGIN_DIR", "$dataDir/imagefs_bionic/usr/lib/alsa-lib")
            // SDL input hints GameNative sets. Several games (Ys IX included)
            // bring SDL2 as a sub-DLL and query these on startup.
            put("SDL_ALLOW_TOPMOST", "0")
            put("SDL_DIRECTINPUT_ENABLED", "1")
            put("SDL_HINT_FORCE_RAISEWINDOW", "0")
            put("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")
            put("SDL_JOYSTICK_HIDAPI", "1")
            put("SDL_JOYSTICK_RAWINPUT", "0")
            put("SDL_JOYSTICK_WGI", "0")
            put("SDL_MOUSE_FOCUS_CLICKTHROUGH", "1")
            put("SDL_XINPUT_ENABLED", "1")
            // User identity matching GameNative's container (user=xuser).
            // Some games stat HOME-relative paths on startup.
            put("USER", "xuser")
            put("HOME", "$dataDir/imagefs_bionic/home/xuser")
            put("TMPDIR", "$dataDir/imagefs_bionic/usr/tmp")
            // Fontconfig + SSL — fontconfig is needed for wine's GDI+, SSL
            // for any curl/openssl the game links transitively.
            put("FONTCONFIG_PATH", "$dataDir/imagefs_bionic/usr/etc/fonts")
            put("OPENSSL_CONF", "$dataDir/imagefs_bionic/usr/etc/tls/openssl.cnf")
            put("SSL_CERT_DIR", "$dataDir/imagefs_bionic/usr/etc/tls/certs")
            put("SSL_CERT_FILE", "$dataDir/imagefs_bionic/usr/etc/tls/cert.pem")
            // Locale / DNS / gstreamer — GameNative sets these unconditionally.
            put("LC_ALL", "en_US.utf8")
            put("ANDROID_RESOLV_DNS", "192.168.200.1")
            put("GST_PLUGIN_PATH", "$dataDir/imagefs_bionic/usr/lib/gstreamer-1.0")
            put("GST_PLUGIN_FEATURE_RANK", "ximagesink:3000")
            // DXVK frame rate cap + config-file base. Matches GameNative.
            put("DXVK_FRAME_RATE", "60")
            put("DXVK_CONFIG_FILE", "$dataDir/imagefs_bionic/home/xuser/.config/dxvk.conf")
            // DXVK_ASYNC=1 enables the async-pipeline-compilation fork's
            // background dxvk-pcompiler worker threads. GameNative sets this
            // and has 5 dxvk-pcompiler threads; without it our DXVK does
            // synchronous compilation on the calling thread. Ys IX may wait
            // on a pipeline-ready event that async compilation would signal.
            // (2026-04-22 diagnosed: MainThread parks at futex wait @0x73a4365110;
            // audio/Steam/Galaxy all ruled out; pcompiler thread profile gap
            // is the last major DXVK-level diff vs GameNative.)
            put("DXVK_ASYNC", "1")
            put("REDIRECT_FROM", if (useProton9) BAKED_ROOT_P9 else BAKED_ROOT)
            // proton-9 was built expecting GameNative-style imagefs layout
            // (/usr/lib, /opt/wine/...). Our $dataDir/imagefs_bionic IS that
            // layout (from GameNative's imagefs_bionic.txz). For Pepelespooder
            // keep the old proton11/imagefs mirror that was shaped for its
            // different baked paths.
            put("REDIRECT_TO",
                if (useProton9) "$dataDir/imagefs_bionic" else imageFsMirror)
            // GameNative's shim libraries (libevshim.so, libandroid-sysvshm.so)
            // have paths under /data/data/com.winlator.cmod/... baked in (the
            // original Winlator package name). Redirect that namespace too so
            // evshim can find gamepad.mem etc. under our imagefs_bionic mirror.
            put("REDIRECT_FROM2", "/data/data/com.winlator.cmod/files/imagefs")
            put("REDIRECT_TO2", "$dataDir/imagefs_bionic")
            // libasound (bundled with imagefs_bionic) has the bare
            // /data/data/com.winlator/files/imagefs/... path baked in for
            // alsa.conf — different from the .cmod-suffixed path above. If
            // it can't load alsa.conf, xaudio2_7.dll dereferences a null and
            // faults at +0x33364; Wine SEH catches it but the audio thread
            // is left wedged, blocking the game right after the first present.
            put("REDIRECT_FROM3", "/data/data/com.winlator/files/imagefs")
            put("REDIRECT_TO3", "$dataDir/imagefs_bionic")
            put("REDIRECT_DEBUG", "1")
            // Keep using proton11/prefix/.wine (has DXVK DLLs + FEX DLLs
            // in drive_c/windows/system32 already). wine 9/10 share prefix
            // format in most respects; if incompatibilities show up we'll
            // stand up a separate prefix.
            put("WINEPREFIX", "$dataDir/proton11/prefix/.wine")
            // NOTE: previously set WINEBOOTSTRAPMODE=1 here. Removed because
            // GameNative does not set it, and leaving it seems to put wine
            // services.exe into a partial-init state where it page-faults
            // and explorer then can't start.
            put("WINEDEBUG", "-all")
            // NOTE: WINEDLLOVERRIDES is set per-invocation in TerminalActivity
            // because the Ys IX launch needs d3d11/dxgi forwarded to DXVK plus
            // xaudio2_7 + alsa/pulse disabled (audio-init crash blocker).
            // Bionic Vulkan stack from GameNative's imagefs_bionic (extracted
            // to $dataDir/imagefs_bionic). libvulkan.so.1 is the Bionic-built
            // Khronos loader; libvulkan_wrapper.so is the ICD that forwards
            // Vulkan calls over a unix socket to VortekRenderer (which uses
            // Android's native Vulkan -> Mali HAL). This is the path
            // GameNative uses for Bionic-wine on Mali.
            // Mirror GameNative's working Wrapper+System path for Mali: the
            // wrapper ICD uses adrenotools to load Android's system libvulkan.so
            // (routes to Mali HAL). Vortek socket path is unused in this mode
            // but keep VORTEK_SERVER_PATH set harmlessly.
            put("VK_ICD_FILENAMES",
                "$dataDir/imagefs_bionic/usr/share/vulkan/icd.d/wrapper_icd.aarch64.json")
            put("VK_LAYER_PATH",
                "$dataDir/imagefs_bionic/usr/share/vulkan/implicit_layer.d:" +
                "$dataDir/imagefs_bionic/usr/share/vulkan/explicit_layer.d")
            // VK_LAYER_HEADLESS_surface manifest is written into
            // imagefs_bionic/usr/share/vulkan/implicit_layer.d (see
            // ensureImageFsMirror) so the Bionic Vulkan loader picks it up
            // via XDG_DATA_DIRS. It intercepts DXVK's vkQueuePresentKHR and
            // streams frames to FrameSocketServer on TCP 19850.
            put("ENABLE_HEADLESS_LAYER", "1")
            put("VORTEK_SERVER_PATH", "$cacheDir/tmp/vortek.sock")
            // Exact env set GameNative uses for a working Ys IX run on this
            // Mali tablet (captured from /proc/<ys9-pid>/environ — see
            // reference_gamenative_working_env_2026_04_19.md).
            // WRAPPER_EMULATE_BCN=3 is the leading suspect for unblocking
            // DXVK's FL11_0 textureCompressionBC check; the other WRAPPER_*
            // values round out wrapper-side behavior; Mesa/Zink/TU tune the
            // GL-on-Vulkan path zink uses; WINEESYNC + WINEPRELOADRESERVE
            // are wine-side correctness.
            put("GALLIUM_DRIVER", "zink")
            put("LIBGL_KOPPER_DISABLE", "true")
            put("WRAPPER_VK_VERSION", "1.3.0")           // GN uses .0, not .128
            put("WRAPPER_EMULATE_BCN", "3")              // leading FL11_0 fix candidate
            put("WRAPPER_USE_BCN_CACHE", "0")
            put("WRAPPER_MAX_IMAGE_COUNT", "0")
            put("WRAPPER_RESOURCE_TYPE", "auto")
            // PRESENT_WAIT disabled: with it enabled, DXVK asks our headless
            // layer to block until present N completes via VK_KHR_present_wait,
            // which we don't implement. The game's PH3_DrawThreadR was seen
            // spinning forever after the first present while main + dxvk-submit
            // were idle — classic present-wait-never-returns signature.
            put("WRAPPER_DISABLE_PRESENT_WAIT", "1")
            put("WRAPPER_EXTENSION_BLACKLIST",
                "VK_KHR_present_wait,VK_KHR_present_id")
            put("ENABLE_BCN_COMPUTE", "1")
            put("ENABLE_UTIL_LAYER", "1")
            put("BCN_COMPUTE_AUTO", "1")
            // Custom implicit Vulkan layer that forces dualSrcBlend,
            // logicOp, shaderStorageImageExtendedFormats to VK_TRUE in
            // vkGetPhysicalDeviceFeatures. Mali Valhall reports those as
            // FALSE which blocks DXVK's FL11_0 check.
            put("SPOOF_FEATURES", "1")
            put("DXVK_STATE_CACHE_PATH", "$dataDir/imagefs_bionic/home/xuser/.cache")
            put("DXVK_LOG_LEVEL", "info")                // verbose enough to see feature level decision
            // DXVK_LOG_PATH is a DIRECTORY (DXVK writes <exe>_<dll>.log
            // inside it, e.g. ys9_dxgi.log + ys9_d3d11.log). Previously
            // we set this to a file-name-looking path and DXVK silently
            // fell back to cwd-relative, leaving us with no fresh logs.
            put("DXVK_LOG_PATH", "$dataDir/dxvk_logs")
            // Disable DXVK's OpenVR/OpenXR extension providers. On this
            // pipeline they fail "Failed to locate module" (no openvr_api
            // / openxr_loader on device) and, right after enumeration,
            // a function-pointer call in their integration path sends rip
            // into unmapped FEX-cache memory (DEP exec violation at
            // 0x6FCE... with no loaded module).
            put("DXVK_CONFIG",
                "dxvk.enableOpenVR=False;dxvk.enableOpenXR=False")
            put("BOX64_MMAP32", "0")
            // Mesa/Zink
            put("MESA_DEBUG", "silent")
            put("mesa_glthread", "true")
            put("MESA_NO_ERROR", "1")
            put("MESA_VK_WSI_PRESENT_MODE", "mailbox")
            put("ZINK_DEBUG", "compact")
            put("ZINK_DESCRIPTORS", "lazy")
            put("TU_DEBUG", "noconform")
            // Wine tuning
            put("WINEESYNC", "1")
            put("WINEPRELOADRESERVE", "140000000-140936000")
            put("WINE_X11FORCEGLX", "1")
            put("WINE_GST_NO_GL", "1")
            put("WINE_DISABLE_FULLSCREEN_HACK", "1")
            put("WINE_NEW_NDIS", "1")
            put("WINE_NO_DUPLICATE_EXPLORER", "1")
            put("HODLL", "libwow64fex.dll")
            // FEX tuning (from fexcore_env_vars.json defaults)
            put("FEX_TSOENABLED", "1")
            put("FEX_HALFBARRIERTSOENABLED", "1")
            put("FEX_MULTIBLOCK", "1")
            put("FEX_X87REDUCEDPRECISION", "1")
            // XDG so the Bionic Khronos loader picks up implicit layers in
            // imagefs_bionic/usr/share/vulkan/implicit_layer.d/.
            put("XDG_DATA_DIRS", "$dataDir/imagefs_bionic/usr/share")
            put("XDG_CONFIG_DIRS", "$dataDir/imagefs_bionic/etc/xdg")

            // NOTE: Full GameNative env (HOME/USER/TMPDIR/PATH/
            // ANDROID_SYSVSHM_SERVER/FONTCONFIG_PATH/EVSHIM_*/LD_PRELOAD
            // chain, etc.) NOT set here. Each time we've added the full
            // set, libevshim.so (in the LD_PRELOAD chain) has tried to
            // open a hardcoded /data/data/com.winlator.cmod/...gamepad.mem
            // via dlsym-of-libc (bypassing our LD_PRELOAD hook), failed,
            // and the run has either hung or regressed. Until we have a
            // way to redirect libevshim's dlsym-resolved open(), leave
            // those env vars off and use only the single-lib LD_PRELOAD
            // redirect shim we control.

            putAll(extraEnv)
        }
        val argv = mutableListOf(wineBinary)
        argv.addAll(args)
        Log.i(TAG, "wineRun: ${argv.joinToString(" ")}")
        val pb = ProcessBuilder(argv)
            .directory(File(dataDir))
            .redirectErrorStream(false)
        pb.environment().clear()
        pb.environment().putAll(env)
        return try {
            val proc = pb.start()
            val outBuf = StringBuilder()
            val errBuf = StringBuilder()
            val o = Thread {
                try {
                    val r = proc.inputStream.bufferedReader()
                    while (true) {
                        val l = r.readLine() ?: break
                        synchronized(outBuf) { outBuf.append(l).append('\n') }
                    }
                } catch (_: Throwable) {}
            }.apply { start() }
            val e = Thread {
                try {
                    val r = proc.errorStream.bufferedReader()
                    while (true) {
                        val l = r.readLine() ?: break
                        synchronized(errBuf) { errBuf.append(l).append('\n') }
                    }
                } catch (_: Throwable) {}
            }.apply { start() }
            val finished = proc.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
            val code = if (finished) proc.exitValue() else { proc.destroyForcibly(); -99 }
            o.join(500); e.join(500)
            Result(code, synchronized(outBuf) { outBuf.toString() }, synchronized(errBuf) { errBuf.toString() })
        } catch (t: Throwable) {
            Log.e(TAG, "wineRun failed", t)
            Result(-2, "", t.stackTraceToString())
        }
    }

    /**
     * Run `wine wineboot --init` with the path-redirect shim. Creates/updates
     * the wine prefix under files/proton11/prefix/.wine. First time takes a
     * few seconds (registry init, fake Windows tree, etc.). Wineserver will
     * auto-start as a child, so the shim must be set up for both wine and
     * wineserver — same env vars applied.
     */
    fun wineBootInit(timeoutMs: Long = 180000): Result {
        if (!wineBinaryExists()) {
            return Result(-1, "", "wine binary missing: $winePath")
        }
        if (!File(redirectLib).exists()) {
            return Result(-1, "", "redirect shim missing: $redirectLib")
        }
        if (!refreshBinSymlinks()) {
            return Result(-1, "", "failed to refresh bin symlinks")
        }
        if (!ensureImageFsMirror()) {
            return Result(-1, "", "failed to build imagefs mirror")
        }

        val env = buildEnv().toMutableMap().apply {
            put("LD_PRELOAD", redirectLib)
            put("REDIRECT_FROM", BAKED_ROOT)
            put("REDIRECT_TO", imageFsMirror)
            put("REDIRECT_DEBUG", "1")
            put("WINEPREFIX", "$dataDir/proton11/prefix/.wine")
            // Quiet trace, keep errors + warns; +pid to distinguish parent/child
            put("WINEDEBUG", "err+all,warn+loader,fixme-all,trace-all,+pid,+tid")
            // Keep is_prefix_bootstrap=TRUE through the whole wine run so child
            // processes (start.exe, explorer.exe) can resolve kernel32.dll via
            // find_builtin_without_file (WINEDLLDIR*). Without this, wine
            // sets WINEBOOTSTRAPMODE=1 only during its internal run_wineboot,
            // then restores to whatever we passed in. If we pass "1",
            // restore leaves it as "1" and is_prefix_bootstrap stays TRUE.
            put("WINEBOOTSTRAPMODE", "1")
        }
        Log.i(TAG, "Running $winePath wineboot --init")

        val pb = ProcessBuilder(winePath, "wineboot", "--init")
            .directory(File(dataDir))
            .redirectErrorStream(false)
        pb.environment().clear()
        pb.environment().putAll(env)

        return try {
            val proc = pb.start()
            val outBuf = StringBuilder()
            val errBuf = StringBuilder()
            val out = Thread {
                try {
                    val r = proc.inputStream.bufferedReader()
                    while (true) {
                        val line = r.readLine() ?: break
                        synchronized(outBuf) { outBuf.append(line).append('\n') }
                    }
                } catch (_: Throwable) {}
            }.apply { start() }
            val err = Thread {
                try {
                    val r = proc.errorStream.bufferedReader()
                    while (true) {
                        val line = r.readLine() ?: break
                        synchronized(errBuf) { errBuf.append(line).append('\n') }
                    }
                } catch (_: Throwable) {}
            }.apply { start() }

            val finished = proc.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
            val code = if (finished) proc.exitValue() else { proc.destroyForcibly(); -99 }
            out.join(500)
            err.join(500)
            val s = synchronized(outBuf) { outBuf.toString() }
            val e = synchronized(errBuf) { errBuf.toString() }
            Result(code, if (finished) s else "$s[TIMED OUT after ${timeoutMs}ms]\n", e)
        } catch (t: Throwable) {
            Log.e(TAG, "wineboot failed", t)
            Result(-2, "", t.stackTraceToString())
        }
    }

    /**
     * Env var recipe derived from GameNative's BionicProgramLauncherComponent.
     * Minimal subset — no LD_PRELOAD yet (skipped until we add sysvshm/evshim
     * from jniLibs). `wine --version` does not exercise the hooks, so this is
     * enough for the first iteration.
     */
    private fun buildEnv(useProton9: Boolean = false): Map<String, String> {
        val wineTree = if (useProton9) "$dataDir/proton9" else "$dataDir/proton11"
        // Prepend GameNative imagefs_bionic lib dir so dlopen("libvulkan.so.1")
        // resolves to the Bionic Khronos loader, and libvulkan_wrapper.so +
        // its deps (libadrenotools, libandroid-sysvshm, libxcb, libdrm, ...)
        // resolve to their Bionic-built copies. Must come BEFORE nativeLibDir
        // so our glibc libvortek_icd_wrapper.so / libvulkan_vortek.so (still
        // in jniLibs for the FEX pipeline) don't shadow the wrapper.
        val imageFsLib = "$dataDir/imagefs_bionic/usr/lib"
        val ldPath = listOf(
            imageFsLib,
            nativeLibDir,
            "$wineTree/lib/wine/aarch64-unix",
            "$wineTree/lib",
            "/system/lib64",
        ).joinToString(":")
        return mapOf(
            "PATH" to "$nativeLibDir:$wineTree/bin:/system/bin",
            "LD_LIBRARY_PATH" to ldPath,
            "WINELOADER" to "$wineTree/bin/wine",
            "WINEDLLPATH" to "$wineTree/lib/wine",
            "HOME" to dataDir,
            "TMPDIR" to "$dataDir/tmp",
            "WINEDEBUG" to "-all",
        )
    }
}
