package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Executes commands inside the FEX-Emu x86-64 environment.
 * Runs FEXLoader directly via ProcessBuilder — no PRoot needed.
 *
 * FEXLoader is an ARM64 binary that loads an x86-64 rootfs overlay
 * and JIT-compiles x86-64 instructions to ARM64. It provides:
 * - Full x86-64 environment (bash, tar, ls, etc.)
 * - glibc semaphore support (no Bionic issues)
 * - GPU passthrough via thunks (Vulkan/OpenGL)
 *
 * Since the FEX binaries have PT_INTERP=/opt/fex/lib/ld-linux-aarch64.so.1
 * (which doesn't exist on Android), we invoke FEXLoader through the bundled
 * dynamic linker directly.
 */
class FexExecutor(private val context: Context) {

    companion object {
        private const val TAG = "FexExecutor"
    }

    /** FEXServer process — kept alive for the duration of the app session */
    private var fexServerProcess: Process? = null

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    /** Directory containing FEX binaries (FEXLoader, FEXInterpreter, etc.) */
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

    /** Path to the bundled dynamic linker */
    private val ldLinuxPath: String
        get() = "$fexDir/lib/ld-linux-aarch64.so.1"

    /** Path to FEXLoader binary */
    private val fexLoaderPath: String
        get() = "$fexDir/bin/FEXLoader"

    /** Library path for FEXLoader's dependencies */
    private val fexLibPath: String
        get() = "$fexDir/lib:$fexDir/lib/aarch64-linux-gnu"

    init {
        // Ensure directories exist
        File(tmpDir).mkdirs()
        File(tmpDir, "shm").mkdirs()
        File(x11SocketDir).mkdirs()
        File(fexHomeDir).mkdirs()
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

            // Build the shell command that invokes FEXLoader directly.
            // FEX binaries are patched with patchelf to use the correct PT_INTERP
            // pointing to the bundled ld-linux-aarch64.so.1, so they can be
            // invoked directly. This makes /proc/self/exe point to FEXLoader
            // (not ld.so), which is required for child process exec to work.
            val shellCommand = buildString {
                // Clear Android environment pollution
                append("unset LD_PRELOAD TMPDIR PREFIX BOOTCLASSPATH ANDROID_ART_ROOT ANDROID_DATA && ")
                append("unset ANDROID_I18N_ROOT ANDROID_TZDATA_ROOT COLORTERM DEX2OATBOOTCLASSPATH && ")

                // Set FEX environment
                append("export HOME='${baseEnv["HOME"]}' && ")
                append("export TMPDIR='$tmpDir' && ")
                append("export USE_HEAP=1 && ")
                append("export FEX_DISABLETELEMETRY=1 && ")
                // LD_LIBRARY_PATH for FEX native libraries (RPATH alone isn't sufficient)
                append("export LD_LIBRARY_PATH='$fexLibPath' && ")

                // Invoke FEXLoader directly (PT_INTERP patched to bundled ld.so)
                append("exec '$fexLoaderPath'")
                append(" -- /bin/bash -c '${guestCommand.replace("'", "'\\''")}'")
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
        val fexServerPath = "$fexDir/bin/FEXServer"
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
            val baseEnv = buildBaseEnvironment()
            val shellCommand = buildString {
                append("unset LD_PRELOAD TMPDIR PREFIX BOOTCLASSPATH ANDROID_ART_ROOT && ")
                append("unset ANDROID_I18N_ROOT ANDROID_TZDATA_ROOT COLORTERM DEX2OATBOOTCLASSPATH && ")
                append("export HOME='${baseEnv["HOME"]}' && ")
                append("export TMPDIR='$tmpDir' && ")
                append("export USE_HEAP=1 && ")
                append("export FEX_DISABLETELEMETRY=1 && ")
                append("export LD_LIBRARY_PATH='$fexLibPath' && ")
                append("exec '$fexServerPath'")
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
            Log.i(TAG, "FEXServer started")

            // Give FEXServer time to create its socket
            Thread.sleep(1500)
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
            "FEX_DISABLETELEMETRY" to "1",

            // Library path for FEX native binaries
            "LD_LIBRARY_PATH" to fexLibPath,

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
            val shellCommand = "'$fexLoaderPath' --version"
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
