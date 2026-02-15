package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Executes commands inside the FEX-Emu x86-64 environment.
 * Runs FEXLoader via ProcessBuilder through the bundled ld.so — no PRoot needed.
 *
 * FEXLoader is an ARM64 binary that loads an x86-64 rootfs overlay
 * and JIT-compiles x86-64 instructions to ARM64. It provides:
 * - Full x86-64 environment (bash, tar, ls, etc.)
 * - glibc semaphore support (no Bionic issues)
 * - GPU passthrough via thunks (Vulkan/OpenGL)
 *
 * On Android 10+ (targetSdk 29+), SELinux prevents executing binaries from
 * app_data_file locations. FEX binaries are packaged as JNI libs (lib*.so)
 * in nativeLibraryDir which has executable SELinux context. We invoke
 * FEXLoader through the bundled ld-linux-aarch64.so.1 from nativeLibDir.
 */
class FexExecutor(private val context: Context) {

    companion object {
        private const val TAG = "FexExecutor"
    }

    /** FEXServer process — kept alive for the duration of the app session */
    private var fexServerProcess: Process? = null

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    /** Directory containing FEX libraries (glibc, libFEXCore, etc.) */
    private val fexDir: String
        get() = app.getFexDir()

    /** Directory containing the extracted x86-64 rootfs */
    private val fexRootfsDir: String
        get() = app.getFexRootfsDir()

    /** HOME directory for FEX config and user data */
    private val fexHomeDir: String
        get() = app.getFexHomeDir()

    /** Actual tmp dir on Android filesystem */
    private val tmpDir: String
        get() = app.getTmpDir()

    /** X11 socket directory on Android filesystem */
    private val x11SocketDir: String
        get() = app.getX11SocketDir()

    /**
     * Directory containing native libs with executable SELinux context.
     * On Android 10+ (targetSdk 29+), only files in nativeLibraryDir can be executed
     * by untrusted_app. FEX binaries are packaged as lib*.so in jniLibs/arm64-v8a/.
     */
    private val nativeLibDir: String
        get() = context.applicationInfo.nativeLibraryDir

    /** Path to the bundled dynamic linker (in nativeLibDir for exec permission) */
    private val ldLinuxPath: String
        get() = "$nativeLibDir/libld_linux_aarch64.so"

    /** Path to FEXLoader binary (in nativeLibDir for exec permission) */
    private val fexLoaderPath: String
        get() = "$nativeLibDir/libFEX.so"

    /** Path to FEXServer binary (in nativeLibDir for exec permission) */
    private val fexServerPath: String
        get() = "$nativeLibDir/libFEXServer.so"

    /** Library path for FEXLoader's dependencies */
    private val fexLibPath: String
        get() = "$nativeLibDir:$fexDir/lib:$fexDir/lib/aarch64-linux-gnu"

    init {
        // Ensure directories exist
        File(tmpDir).mkdirs()
        File(tmpDir, "shm").mkdirs()
        File(x11SocketDir).mkdirs()
        File(fexHomeDir).mkdirs()

        Log.i(TAG, "FexExecutor init: nativeLibDir=$nativeLibDir")
        Log.i(TAG, "  ld.so=$ldLinuxPath exists=${File(ldLinuxPath).exists()}")
        Log.i(TAG, "  FEX=$fexLoaderPath exists=${File(fexLoaderPath).exists()}")
        Log.i(TAG, "  FEXServer=$fexServerPath exists=${File(fexServerPath).exists()}")
        Log.i(TAG, "  fexDir=$fexDir")
        Log.i(TAG, "  rootfs=$fexRootfsDir")
        Log.i(TAG, "  home=$fexHomeDir")
        Log.i(TAG, "  tmpDir=$tmpDir")
    }

    /**
     * Execute a command inside the FEX x86-64 environment.
     *
     * @param command The command to execute (runs inside x86-64 bash)
     * @param environment Additional environment variables for the guest
     * @param workingDir Working directory inside the FEX environment
     * @return The Process object for the running command, or null on failure
     */
    fun execute(
        command: String,
        environment: Map<String, String> = emptyMap(),
        workingDir: String = "/home/user"
    ): Process? {
        if (!File(ldLinuxPath).exists()) {
            Log.e(TAG, "FEX dynamic linker not found at: $ldLinuxPath")
            return null
        }

        if (!File(fexLoaderPath).exists()) {
            Log.e(TAG, "FEXLoader not found at: $fexLoaderPath")
            return null
        }

        // Ensure FEXServer is running (required for guest binary execution)
        ensureFexServerRunning()

        // Verify FEXServer socket exists (FEXLoader will hang without it)
        val socketFiles = File(tmpDir).listFiles()?.filter { it.name.endsWith("FEXServer.Socket") } ?: emptyList()
        if (socketFiles.isEmpty()) {
            Log.e(TAG, "No FEXServer socket found in $tmpDir — FEXLoader will likely hang!")
        } else {
            Log.d(TAG, "FEXServer socket: ${socketFiles.first().absolutePath}")
        }

        // Ensure socket symlinks exist in FEX rootfs /tmp/
        ensureSocketSymlinks()

        // Build guest environment exports
        val guestEnvExports = environment.entries.joinToString("; ") { (key, value) ->
            "export $key='$value'"
        }
        val guestCommand = if (guestEnvExports.isNotEmpty()) {
            "$guestEnvExports; $command"
        } else {
            command
        }

        Log.d(TAG, "Guest command: $guestCommand")

        return try {
            val baseEnv = buildBaseEnvironment()

            // Build the shell command that invokes FEXLoader via the bundled ld.so.
            // FEX binaries live in nativeLibraryDir (executable SELinux context).
            // We invoke ld.so explicitly so it loads FEXLoader with the correct
            // library path, bypassing the hardcoded PT_INTERP.
            val shellCommand = buildString {
                // Clear Android environment pollution
                append("unset LD_PRELOAD TMPDIR PREFIX BOOTCLASSPATH ANDROID_ART_ROOT ANDROID_DATA && ")
                append("unset ANDROID_I18N_ROOT ANDROID_TZDATA_ROOT COLORTERM DEX2OATBOOTCLASSPATH && ")

                // Set FEX environment
                // HOME must be the host path here so FEX can find Config.json at $HOME/.fex-emu/
                // We'll override HOME for the guest later (to /home/user) inside the bash command
                append("export HOME='${baseEnv["HOME"]}' && ")
                append("export TMPDIR='$tmpDir' && ")
                append("export USE_HEAP=1 && ")
                append("export FEX_DISABLETELEMETRY=0 && ")
                append("export FEX_HOSTFEATURES='disableavx' && ")
                append("export FEX_SILENTLOG=0 && ")
                append("export FEX_OUTPUTLOG='$tmpDir/fex-debug.log' && ")
                // FEX config via env vars for child process re-exec
                append("export FEX_ROOTFS='Ubuntu_22_04' && ")
                append("export FEX_THUNKHOSTLIBS='$fexDir/lib/fex-emu/HostThunks' && ")
                append("export FEX_THUNKGUESTLIBS='$fexRootfsDir/opt/fex/share/fex-emu/GuestThunks' && ")
                append("export FEX_THUNKCONFIG='$fexHomeDir/.fex-emu/thunks.json' && ")
                append("export FEX_X87REDUCEDPRECISION=1 && ")
                // LD_LIBRARY_PATH for FEX native libraries (RPATH alone isn't sufficient)
                append("export LD_LIBRARY_PATH='$fexLibPath' && ")
                // Vulkan host-side ICD for thunks: host thunk → ICD loader → Vortek
                append("export VK_ICD_FILENAMES='${baseEnv["VK_ICD_FILENAMES"]}' && ")
                append("export VK_DRIVER_FILES='${baseEnv["VK_DRIVER_FILES"]}' && ")
                append("export VORTEK_SERVER_PATH='${baseEnv["VORTEK_SERVER_PATH"]}' && ")
                // FEX_SELF_LDSO/FEX_SELF_LIBPATH tell our patched FEX how to re-exec
                // itself for child processes via ld.so wrapper (on Android, FEX's
                // PT_INTERP doesn't exist and /proc/self/exe points to ld.so)
                append("export FEX_SELF_LDSO='$ldLinuxPath' && ")
                append("export FEX_SELF_LIBPATH='$fexLibPath' && ")

                // Invoke via ld.so wrapper: ld.so --library-path <path> FEXLoader <guest>
                // Note: no 'exec' - let shell survive to report errors
                append("'$ldLinuxPath' --library-path '$fexLibPath' '$fexLoaderPath'")
                // Override HOME inside the guest to the rootfs path (/home/user)
                // so programs like SteamCMD write to $HOME/Steam/ through the overlay,
                // not to the Android host path which bypasses the rootfs.
                append(" /bin/bash -c 'export HOME=/home/user; ${guestCommand.replace("'", "'\\''")}'")
                append(" ; echo \"[FEX exit code: \$?]\"")
            }

            Log.d(TAG, "Shell command: $shellCommand")

            val processBuilder = ProcessBuilder("/system/bin/sh", "-c", shellCommand).apply {
                directory(File(context.filesDir.absolutePath))

                environment().clear()
                environment().putAll(baseEnv)

                redirectErrorStream(true)
            }

            processBuilder.start().also {
                Log.i(TAG, "FEX process started successfully")
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to start FEX process", e)
            null
        }
    }

    /**
     * Execute a command and wait for completion, returning output.
     */
    fun executeBlocking(
        command: String,
        environment: Map<String, String> = emptyMap(),
        workingDir: String = "/home/user",
        timeoutMs: Long = 30000
    ): ExecutionResult {
        val process = execute(command, environment, workingDir)
            ?: return ExecutionResult(-1, "", "Failed to start process")

        return try {
            val output = StringBuilder()

            val outputThread = Thread {
                process.inputStream.bufferedReader().use { reader ->
                    reader.forEachLine { line ->
                        output.appendLine(line)
                        Log.d(TAG, "OUT: $line")
                    }
                }
            }

            outputThread.start()
            outputThread.join(timeoutMs)

            if (outputThread.isAlive) {
                process.destroyForcibly()
                outputThread.interrupt()
                return ExecutionResult(-1, output.toString(), "Process timed out")
            }

            val exitCode = process.waitFor()
            ExecutionResult(exitCode, output.toString(), "")

        } catch (e: Exception) {
            Log.e(TAG, "Execution failed", e)
            process.destroyForcibly()
            ExecutionResult(-1, "", e.message ?: "Unknown error")
        }
    }

    /**
     * Start FEXServer if it's not already running.
     * FEXServer provides services like rootfs overlay mounting and is required
     * for FEXLoader to execute guest binaries.
     */
    @Synchronized
    private fun ensureFexServerRunning() {
        if (!File(fexServerPath).exists()) {
            Log.w(TAG, "FEXServer not found at: $fexServerPath")
            return
        }

        // Check if FEXServer is still alive
        fexServerProcess?.let { process ->
            try {
                process.exitValue()
                // If we get here, the process has exited
                Log.d(TAG, "FEXServer process has exited, restarting...")
                fexServerProcess = null
            } catch (e: IllegalThreadStateException) {
                // Process is still running
                Log.d(TAG, "FEXServer already running")
                return
            }
        }

        try {
            // Kill any orphaned FEXServer processes from previous sessions
            killOrphanedFexServers()

            // Clean up stale socket files
            File(tmpDir).listFiles()?.filter { it.name.endsWith("FEXServer.Socket") }?.forEach { socket ->
                Log.d(TAG, "Removing stale FEXServer socket: ${socket.name}")
                socket.delete()
            }

            val baseEnv = buildBaseEnvironment()
            val shellCommand = buildString {
                append("unset LD_PRELOAD TMPDIR PREFIX BOOTCLASSPATH ANDROID_ART_ROOT && ")
                append("unset ANDROID_I18N_ROOT ANDROID_TZDATA_ROOT COLORTERM DEX2OATBOOTCLASSPATH && ")
                append("export HOME='${baseEnv["HOME"]}' && ")
                append("export TMPDIR='$tmpDir' && ")
                append("export USE_HEAP=1 && ")
                append("export FEX_DISABLETELEMETRY=0 && ")
                append("export FEX_HOSTFEATURES='disableavx' && ")
                append("export FEX_SILENTLOG=0 && ")
                append("export FEX_OUTPUTLOG='$tmpDir/fex-debug.log' && ")
                // FEX config via env vars for child process re-exec
                append("export FEX_ROOTFS='Ubuntu_22_04' && ")
                append("export FEX_THUNKHOSTLIBS='$fexDir/lib/fex-emu/HostThunks' && ")
                append("export FEX_THUNKGUESTLIBS='$fexRootfsDir/opt/fex/share/fex-emu/GuestThunks' && ")
                append("export FEX_THUNKCONFIG='$fexHomeDir/.fex-emu/thunks.json' && ")
                append("export FEX_X87REDUCEDPRECISION=1 && ")
                append("export LD_LIBRARY_PATH='$fexLibPath' && ")
                // Vulkan host-side ICD for thunks
                append("export VK_ICD_FILENAMES='${baseEnv["VK_ICD_FILENAMES"]}' && ")
                append("export VK_DRIVER_FILES='${baseEnv["VK_DRIVER_FILES"]}' && ")
                append("export VORTEK_SERVER_PATH='${baseEnv["VORTEK_SERVER_PATH"]}' && ")
                // Invoke FEXServer via ld.so wrapper (same as FEXLoader)
                append("exec '$ldLinuxPath' --library-path '$fexLibPath' '$fexServerPath'")
                append(" -f -p 300") // foreground, persistent for 5 minutes
            }

            val processBuilder = ProcessBuilder("/system/bin/sh", "-c", shellCommand).apply {
                directory(File(context.filesDir.absolutePath))
                environment().clear()
                environment().putAll(baseEnv)
                environment()["TMPDIR"] = tmpDir
                redirectErrorStream(true)
            }

            fexServerProcess = processBuilder.start()
            Log.i(TAG, "FEXServer started (pid=${fexServerProcess?.hashCode()})")

            // Drain FEXServer output in background thread to prevent pipe buffer fill
            // and to capture any error messages for debugging
            val serverProcess = fexServerProcess
            Thread {
                try {
                    serverProcess?.inputStream?.bufferedReader()?.use { reader ->
                        reader.forEachLine { line ->
                            Log.d(TAG, "FEXServer OUT: $line")
                        }
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "FEXServer output reader stopped: ${e.message}")
                }
                Log.d(TAG, "FEXServer output stream closed")
            }.apply { isDaemon = true }.start()

            // Give FEXServer time to create its socket
            Thread.sleep(2000)

            // Verify FEXServer is still alive
            val alive = try {
                fexServerProcess?.exitValue()
                false // exitValue() returned = process exited
            } catch (e: IllegalThreadStateException) {
                true // still running
            }

            // Check socket existence
            val tmpFiles = File(tmpDir).listFiles()?.map { it.name } ?: emptyList()
            Log.i(TAG, "FEXServer alive=$alive, TMPDIR=$tmpDir, files=${tmpFiles.joinToString()}")

            if (!alive) {
                Log.e(TAG, "FEXServer died during startup!")
                fexServerProcess = null
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start FEXServer", e)
        }
    }

    /**
     * Stop FEXServer if it's running.
     */
    fun stopFexServer() {
        fexServerProcess?.let { process ->
            try {
                process.destroyForcibly()
                Log.i(TAG, "FEXServer stopped")
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping FEXServer", e)
            }
            fexServerProcess = null
        }
    }

    /**
     * Create symlinks inside the FEX rootfs /tmp/ directory pointing to
     * actual socket files on the Android filesystem.
     *
     * FEX redirects guest /tmp → ${fexRootfsDir}/tmp/, so the guest
     * process sees these symlinks when accessing /tmp/vortek.sock etc.
     */
    private fun ensureSocketSymlinks() {
        val rootfsTmp = File(fexRootfsDir, "tmp")
        rootfsTmp.mkdirs()

        // Vortek socket: /tmp/vortek.sock → actual socket
        createSymlink(
            File(rootfsTmp, "vortek.sock"),
            File(tmpDir, "vortek.sock")
        )

        // Vortek V0 socket: /tmp/.vortek/V0 → actual socket
        val vortekDir = File(rootfsTmp, ".vortek")
        vortekDir.mkdirs()
        createSymlink(
            File(vortekDir, "V0"),
            File(tmpDir, ".vortek/V0")
        )

        // X11 socket: /tmp/.X11-unix/X0 → actual socket
        val x11Dir = File(rootfsTmp, ".X11-unix")
        x11Dir.mkdirs()
        createSymlink(
            File(x11Dir, "X0"),
            File(x11SocketDir, "X0")
        )

        // Shared memory
        val shmDir = File(rootfsTmp, "shm")
        if (!shmDir.exists()) {
            createSymlink(shmDir, File(tmpDir, "shm"))
        }
    }

    /**
     * Kill any orphaned FEXServer processes from previous app sessions.
     * These can persist with PPID=1 and block socket creation.
     *
     * Also cleans up stale lock files. FEXServer uses file locking to ensure
     * single-instance operation. A stale lock (from a killed but not cleaned-up
     * process) will block new FEXServer instances from starting (silent exit 255).
     */
    private fun killOrphanedFexServers() {
        try {
            // Find FEXServer processes by searching for our binary name in /proc
            val result = ProcessBuilder("/system/bin/sh", "-c",
                "ps -e -o pid,args 2>/dev/null | grep -i '[F]EXServer\\|[l]ibFEXServer' || true"
            ).redirectErrorStream(true).start()
            val output = result.inputStream.bufferedReader().readText().trim()
            result.waitFor()

            if (output.isNotEmpty()) {
                Log.d(TAG, "Found FEXServer processes:\n$output")
                output.lines().forEach { line ->
                    val pid = line.trim().split("\\s+".toRegex()).firstOrNull()?.toIntOrNull()
                    if (pid != null && pid > 1) {
                        Log.d(TAG, "Killing orphaned FEXServer PID=$pid")
                        android.os.Process.killProcess(pid)
                    }
                }
                // Give time for processes to die
                Thread.sleep(500)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error cleaning up orphaned FEXServer: ${e.message}")
        }

        // Clean up stale lock files that block FEXServer startup.
        // FEXServer holds a read lock on Server.lock while running.
        // If it dies without cleanup, the lock file remains but no lock is held.
        // However, if the process is still alive (orphaned), the lock prevents
        // new FEXServer instances from starting (InitializeServerPipe returns false).
        cleanupStaleLockFiles()
    }

    /**
     * Check and clean up stale FEXServer lock files.
     * Uses fcntl F_GETLK to detect if any process holds the lock.
     * If a process holds it, try to kill it. Then remove the lock files.
     */
    private fun cleanupStaleLockFiles() {
        val serverDir = File(fexHomeDir, ".fex-emu/Server")
        val lockFile = File(serverDir, "Server.lock")
        val rootfsLockFile = File(serverDir, "RootFS.lock")

        if (!lockFile.exists()) return

        try {
            // Use fuser or check /proc to find who holds the lock
            // Since we can't use fcntl from Kotlin, use a shell command
            val result = ProcessBuilder("/system/bin/sh", "-c",
                "flock -n '${lockFile.absolutePath}' true 2>/dev/null; echo \$?"
            ).redirectErrorStream(true).start()
            val exitStr = result.inputStream.bufferedReader().readText().trim()
            result.waitFor()

            if (exitStr == "0") {
                // Lock is free - stale file, safe to remove
                Log.d(TAG, "Server.lock exists but is unlocked (stale), removing")
                lockFile.delete()
                rootfsLockFile.delete()
            } else {
                // Lock is held - find and kill the holder
                Log.w(TAG, "Server.lock is held by another process, attempting cleanup")

                // Try to find the PID via lsof or /proc scan
                val lsofResult = ProcessBuilder("/system/bin/sh", "-c",
                    "ls /proc/*/fd/* 2>/dev/null | while read f; do " +
                    "readlink \$f 2>/dev/null | grep -q '${lockFile.absolutePath}' && echo \$f; " +
                    "done | head -5"
                ).redirectErrorStream(true).start()
                val lsofOutput = lsofResult.inputStream.bufferedReader().readText().trim()
                lsofResult.waitFor()

                if (lsofOutput.isNotEmpty()) {
                    Log.d(TAG, "Lock held by: $lsofOutput")
                    // Extract PIDs from /proc/<pid>/fd/<fd>
                    lsofOutput.lines().forEach { fdPath ->
                        val pid = fdPath.split("/").getOrNull(2)?.toIntOrNull()
                        if (pid != null && pid > 1 && pid != android.os.Process.myPid()) {
                            Log.d(TAG, "Killing lock holder PID=$pid")
                            android.os.Process.killProcess(pid)
                        }
                    }
                    Thread.sleep(500)
                }

                // Force-remove lock files
                lockFile.delete()
                rootfsLockFile.delete()
                Log.d(TAG, "Removed stale lock files")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error cleaning up lock files: ${e.message}")
            // Best-effort: just delete them
            try {
                lockFile.delete()
                rootfsLockFile.delete()
            } catch (_: Exception) {}
        }
    }

    private fun createSymlink(link: File, target: File) {
        try {
            link.delete()
            Runtime.getRuntime().exec(
                arrayOf("ln", "-sf", target.absolutePath, link.absolutePath)
            ).waitFor()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to create symlink: ${link.absolutePath} → ${target.absolutePath}", e)
        }
    }

    private fun buildBaseEnvironment(): Map<String, String> {
        // Host Vortek ICD JSON — tells the Vulkan ICD loader where to find libvulkan_vortek.so
        val hostIcdJson = "$fexHomeDir/.fex-emu/vortek_host_icd.json"

        return mapOf(
            // FEX reads Config.json from $HOME/.fex-emu/
            "HOME" to fexHomeDir,
            "USER" to "user",
            "SHELL" to "/bin/bash",
            "TERM" to "xterm-256color",
            "LANG" to "en_US.UTF-8",
            "LC_ALL" to "en_US.UTF-8",
            "PATH" to "/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games",

            // Display
            "DISPLAY" to ":0",

            // FEX-specific
            "USE_HEAP" to "1",
            "FEX_DISABLETELEMETRY" to "0",
            "FEX_HOSTFEATURES" to "disableavx",
            "FEX_SILENTLOG" to "0",
            "FEX_OUTPUTLOG" to "$tmpDir/fex-debug.log",

            // FEX config via env vars — critical for Wine child processes.
            // When Wine spawns subprocesses, FEX re-execs with HOME=/home/user (guest),
            // can't find Config.json, and falls back to wrong default paths.
            // These env vars persist through child process re-exec.
            "FEX_ROOTFS" to "Ubuntu_22_04",
            "FEX_THUNKHOSTLIBS" to "$fexDir/lib/fex-emu/HostThunks",
            "FEX_THUNKGUESTLIBS" to "$fexRootfsDir/opt/fex/share/fex-emu/GuestThunks",
            "FEX_THUNKCONFIG" to "$fexHomeDir/.fex-emu/thunks.json",
            "FEX_X87REDUCEDPRECISION" to "1",

            // Library path for FEX native binaries
            "LD_LIBRARY_PATH" to fexLibPath,

            // Vulkan host-side ICD configuration for thunks.
            // When the host thunk calls dlopen("libvulkan.so.1"), the ICD loader
            // reads these env vars to find the Vortek ICD JSON, which points to
            // libvulkan_vortek.so (ARM64 glibc Vortek client).
            "VK_ICD_FILENAMES" to hostIcdJson,
            "VK_DRIVER_FILES" to hostIcdJson,

            // Vortek socket path for the Vortek client to connect to VortekRenderer
            "VORTEK_SERVER_PATH" to "$tmpDir/.vortek/V0",

            // Android system paths (accessible via FEX's host fallthrough)
            "ANDROID_ROOT" to "/system",
            "ANDROID_DATA" to "/data"
        )
    }

    /**
     * Check if FEX is available and functional.
     */
    fun isFexAvailable(): Boolean {
        val ldExists = File(ldLinuxPath).exists()
        val loaderExists = File(fexLoaderPath).exists()

        if (!ldExists) {
            Log.w(TAG, "FEX dynamic linker not found: $ldLinuxPath")
            return false
        }
        if (!loaderExists) {
            Log.w(TAG, "FEXLoader not found: $fexLoaderPath")
            return false
        }

        return true
    }

    /**
     * Check if the FEX rootfs is set up and ready.
     */
    fun isRootfsReady(): Boolean {
        return File(fexRootfsDir, "usr/lib/x86_64-linux-gnu/libc.so.6").exists()
    }

    /**
     * Get FEX version string.
     */
    fun getFexVersion(): String {
        if (!isFexAvailable()) return "Not installed"

        return try {
            val shellCommand = "LD_LIBRARY_PATH='$fexLibPath' '$ldLinuxPath' --library-path '$fexLibPath' '$fexLoaderPath' --version"
            val process = ProcessBuilder("/system/bin/sh", "-c", shellCommand)
                .redirectErrorStream(true)
                .start()

            val output = process.inputStream.bufferedReader().readText().trim()
            process.waitFor()
            output
        } catch (e: Exception) {
            "Error: ${e.message}"
        }
    }

    data class ExecutionResult(
        val exitCode: Int,
        val stdout: String,
        val stderr: String
    ) {
        val isSuccess: Boolean get() = exitCode == 0
        val output: String get() = stdout + stderr
    }
}
