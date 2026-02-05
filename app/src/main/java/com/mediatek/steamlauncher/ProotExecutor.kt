package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Manages PRoot execution for running Linux commands in the container.
 * PRoot provides user-space emulation of chroot, mount --bind, and binfmt_misc.
 */
class ProotExecutor(private val context: Context) {

    companion object {
        private const val TAG = "ProotExecutor"
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    private val prootBinary: File
        get() = File(context.applicationInfo.nativeLibraryDir, "libproot.so")

    private val rootfsPath: String
        get() = app.getRootfsDir()

    private val tmpPath: String
        get() = app.getTmpDir()

    private val x11SocketPath: String
        get() = app.getX11SocketDir()

    private val prootTmpDir: File
        get() = File(context.cacheDir, "proot-tmp").also { it.mkdirs() }

    init {
        // Ensure tmp directories exist (including shm subdirectory)
        File(tmpPath).mkdirs()
        File(tmpPath, "shm").mkdirs()
        File(x11SocketPath).mkdirs()
        prootTmpDir.mkdirs()

        // Create symlink for libtalloc.so.2 -> libtalloc.so
        // The linker looks for the exact filename from NEEDED entries
        ensureTallocSymlink()
    }

    private val libOverrideDir: File
        get() = File(context.filesDir, "lib-override").also { it.mkdirs() }

    /**
     * Creates a symlink from libtalloc.so.2 to libtalloc.so in a writable directory.
     * libproot.so requires "libtalloc.so.2" but Android packages it as "libtalloc.so".
     * Since nativeLibraryDir is read-only, we create the symlink in app's files dir.
     *
     * Note: APK reinstalls change nativeLibraryDir path, so we always recreate the symlink
     * to ensure it points to the current valid location.
     */
    private fun ensureTallocSymlink() {
        val nativeLibDir = File(context.applicationInfo.nativeLibraryDir)
        val tallocSource = File(nativeLibDir, "libtalloc.so")
        val tallocDest = File(libOverrideDir, "libtalloc.so.2")

        // Always delete and recreate
        try {
            tallocDest.delete()
            Log.d(TAG, "Removed old libtalloc.so.2 (if existed)")
        } catch (e: Exception) {
            Log.w(TAG, "Could not remove old file", e)
        }

        if (!tallocSource.exists()) {
            Log.e(TAG, "libtalloc.so not found in native lib dir: ${nativeLibDir.absolutePath}")
            return
        }

        // Copy the file directly - symlinks don't work reliably with Android's library loading
        try {
            tallocSource.copyTo(tallocDest, overwrite = true)
            // CRITICAL: Make the library executable - ld.so cannot preload non-executable .so files
            tallocDest.setExecutable(true, false)
            tallocDest.setReadable(true, false)
            Log.i(TAG, "Copied libtalloc.so to ${tallocDest.absolutePath} with exec permissions")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to copy libtalloc", e)
        }
    }

    /**
     * Execute a command inside the proot container.
     *
     * @param command The command to execute inside the container
     * @param environment Additional environment variables
     * @param workingDir Working directory inside the container
     * @return The Process object for the running command
     */
    fun execute(
        command: String,
        environment: Map<String, String> = emptyMap(),
        workingDir: String = "/home/user"
    ): Process? {
        if (!prootBinary.exists()) {
            Log.e(TAG, "PRoot binary not found at: ${prootBinary.absolutePath}")
            return null
        }

        if (!File(rootfsPath).exists()) {
            Log.e(TAG, "Rootfs not found at: $rootfsPath")
            return null
        }

        val prootArgs = buildProotArgs(workingDir)

        // Build the guest command with environment variables
        // Proot doesn't forward host environment to guest, so we export them in the command
        val guestEnvExports = environment.entries.joinToString("; ") { (key, value) ->
            "export $key='$value'"
        }
        val guestCommand = if (guestEnvExports.isNotEmpty()) {
            "$guestEnvExports; $command"
        } else {
            command
        }

        Log.d(TAG, "Guest command: $guestCommand")
        val fullCommand = listOf(prootBinary.absolutePath) + prootArgs + listOf("/bin/sh", "-c", guestCommand)

        Log.d(TAG, "Executing: ${fullCommand.joinToString(" ")}")

        return try {
            val baseEnv = buildBaseEnvironment()
            val ldLibPath = baseEnv["LD_LIBRARY_PATH"] ?: ""
            Log.d(TAG, "LD_LIBRARY_PATH: $ldLibPath")

            // Android's linker namespace restrictions may prevent LD_LIBRARY_PATH from working
            // when set via ProcessBuilder environment. Use a shell wrapper to ensure it works.
            val shellCommand = buildString {
                append("export LD_LIBRARY_PATH='$ldLibPath' && ")
                append("export PROOT_LOADER='${baseEnv["PROOT_LOADER"]}' && ")
                append("export PROOT_LOADER_32='${baseEnv["PROOT_LOADER_32"]}' && ")
                append("export PROOT_TMP_DIR='${baseEnv["PROOT_TMP_DIR"]}' && ")
                append("export PROOT_NO_SECCOMP=1 && ")
                append("exec ${fullCommand.joinToString(" ") { "'$it'" }}")
            }

            Log.d(TAG, "Shell command: $shellCommand")

            val processBuilder = ProcessBuilder("/system/bin/sh", "-c", shellCommand).apply {
                // Set working directory to app's files dir
                directory(File(context.filesDir.absolutePath))

                // Set environment variables (these supplement the shell exports)
                environment().clear()
                environment().putAll(baseEnv)
                environment().putAll(environment)

                // Redirect stderr to stdout
                redirectErrorStream(true)
            }

            processBuilder.start().also { process ->
                Log.i(TAG, "PRoot process started successfully")
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to start proot process", e)
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
            val error = StringBuilder()

            // Read output in a separate thread
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
            ExecutionResult(exitCode, output.toString(), error.toString())

        } catch (e: Exception) {
            Log.e(TAG, "Execution failed", e)
            process.destroyForcibly()
            ExecutionResult(-1, "", e.message ?: "Unknown error")
        }
    }

    private fun buildProotArgs(workingDir: String): List<String> {
        Log.d(TAG, "Tmp dir: $tmpPath")
        Log.d(TAG, "X11 socket dir: $x11SocketPath")

        return listOf(
            // Root filesystem
            "--rootfs=$rootfsPath",

            // Bind essential directories
            "--bind=/dev:/dev",
            "--bind=/dev/urandom:/dev/urandom",
            "--bind=/dev/random:/dev/random",
            "--bind=/dev/null:/dev/null",
            "--bind=/dev/zero:/dev/zero",
            "--bind=/dev/tty:/dev/tty",

            // GPU access (critical for Vulkan)
            "--bind=/dev/dri:/dev/dri",

            // Temporary directory - MUST come before X11 socket bind
            "--bind=$tmpPath:/tmp",

            // Shared memory
            "--bind=$tmpPath/shm:/dev/shm",

            // X11 socket - MUST come AFTER /tmp bind to override
            "--bind=$x11SocketPath:/tmp/.X11-unix",

            // Proc filesystem (limited in proot)
            "--bind=/proc:/proc",

            // System info
            "--bind=/sys:/sys",

            // Android linker (for Vulkan passthrough)
            "--bind=/system:/system",
            "--bind=/vendor:/vendor",

            // Vortek socket - bind the tmp dir to Winlator's expected path
            // The ICD has hardcoded path: /data/data/com.winlator/files/rootfs/tmp/.vortek/V0
            "--bind=$tmpPath:/data/data/com.winlator/files/rootfs/tmp",

            // Working directory inside container
            "-w", workingDir,

            // Run as root inside container (uid 0 mapped to current user)
            "-0"
        )
    }

    private fun buildBaseEnvironment(): Map<String, String> {
        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        return mapOf(
            "HOME" to "/home/user",
            "USER" to "user",
            "SHELL" to "/bin/bash",
            "TERM" to "xterm-256color",
            "LANG" to "en_US.UTF-8",
            "LC_ALL" to "en_US.UTF-8",
            "PATH" to "/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games:/opt/box64:/opt/box86",

            // Android-specific
            "ANDROID_ROOT" to "/system",
            "ANDROID_DATA" to "/data",

            // PRoot loader configuration (required for Termux proot)
            "PROOT_LOADER" to "$nativeLibDir/libproot-loader.so",
            "PROOT_LOADER_32" to "$nativeLibDir/libproot-loader32.so",

            // Library path for proot dependencies
            // lib-override dir first (contains libtalloc.so.2 symlink), then native lib dir
            "LD_LIBRARY_PATH" to "${libOverrideDir.absolutePath}:$nativeLibDir",

            // Proot doesn't support seccomp
            "PROOT_NO_SECCOMP" to "1",

            // Proot temp directory (must be writable)
            "PROOT_TMP_DIR" to prootTmpDir.absolutePath,

            // Disable ASLR for better Box64/Box86 compatibility
            "BOX64_NOSIGSEGV" to "1",
            "BOX86_NOSIGSEGV" to "1"
        )
    }

    /**
     * Set up common proot environment variables for a ProcessBuilder.
     */
    private fun ProcessBuilder.setupProotEnvironment(): ProcessBuilder {
        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        environment().apply {
            put("LD_LIBRARY_PATH", "${libOverrideDir.absolutePath}:$nativeLibDir")
            put("PROOT_TMP_DIR", prootTmpDir.absolutePath)
            put("PROOT_LOADER", "$nativeLibDir/libproot-loader.so")
            put("PROOT_LOADER_32", "$nativeLibDir/libproot-loader32.so")
            put("PROOT_NO_SECCOMP", "1")
        }
        return this
    }

    /**
     * Check if proot is available and functional.
     */
    fun isProotAvailable(): Boolean {
        if (!prootBinary.exists()) {
            Log.w(TAG, "PRoot binary not found")
            return false
        }

        // Try to run proot --version with proper LD_LIBRARY_PATH
        return try {
            val process = ProcessBuilder(prootBinary.absolutePath, "--version")
                .redirectErrorStream(true)
                .setupProotEnvironment()
                .start()

            val output = process.inputStream.bufferedReader().readText()
            val exitCode = process.waitFor()

            Log.d(TAG, "PRoot version check: $output (exit: $exitCode)")
            exitCode == 0 || output.contains("proot", ignoreCase = true)
        } catch (e: Exception) {
            Log.e(TAG, "PRoot availability check failed", e)
            false
        }
    }

    /**
     * Get proot version string.
     */
    fun getProotVersion(): String {
        if (!prootBinary.exists()) return "Not installed"

        return try {
            val process = ProcessBuilder(prootBinary.absolutePath, "--version")
                .redirectErrorStream(true)
                .setupProotEnvironment()
                .start()

            process.inputStream.bufferedReader().readText().trim()
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
