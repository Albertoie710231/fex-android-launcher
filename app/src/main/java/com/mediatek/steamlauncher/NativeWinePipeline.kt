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
    }

    private val nativeLibDir: String
        get() = context.applicationInfo.nativeLibraryDir

    private val dataDir: String
        get() = context.filesDir.absolutePath

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
            val targets = mapOf(
                "wine" to "$nativeLibDir/$WINE_LIB",
                "wine64" to "$nativeLibDir/$WINE_LIB",
                "wine-preloader" to "$nativeLibDir/libwine_preloader.so",
                "wine64-preloader" to "$nativeLibDir/libwine_preloader.so",
                "wineserver" to "$nativeLibDir/$WINESERVER_LIB",
                // Wine's preloader (loader/preloader.c map_so_lib) resolves
                // the loader ELF by its .so filename against the bin dir
                // (../../bin/libwine_native.so from lib/wine/...). Provide
                // those names as symlinks too so the open() succeeds.
                WINE_LIB to "$nativeLibDir/$WINE_LIB",
                "libwine_preloader.so" to "$nativeLibDir/libwine_preloader.so",
                WINESERVER_LIB to "$nativeLibDir/$WINESERVER_LIB",
            )
            for ((link, target) in targets) {
                val linkFile = File(binDir, link)
                if (linkFile.exists() || java.nio.file.Files.isSymbolicLink(linkFile.toPath())) {
                    linkFile.delete()
                }
                java.nio.file.Files.createSymbolicLink(
                    linkFile.toPath(),
                    java.nio.file.Paths.get(target),
                )
            }
            true
        } catch (t: Throwable) {
            Log.e(TAG, "refreshBinSymlinks failed", t)
            false
        }
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
            true
        } catch (t: Throwable) {
            Log.e(TAG, "ensureImageFsMirror failed", t)
            false
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
     * wine exits or timeout expires.
     */
    fun wineRun(args: List<String>, timeoutMs: Long = 30000): Result {
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
            put("WINEPREFIX", "$dataDir/proton11/prefix/.wine")
            put("WINEBOOTSTRAPMODE", "1")
            put("WINEDEBUG", "-all")
        }
        val argv = mutableListOf(winePath)
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
    private fun buildEnv(): Map<String, String> {
        val wineTree = "$dataDir/proton11"
        val ldPath = listOf(
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
