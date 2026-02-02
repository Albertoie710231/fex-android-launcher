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
     */
    private fun ensureTallocSymlink() {
        val nativeLibDir = File(context.applicationInfo.nativeLibraryDir)
        val tallocSource = File(nativeLibDir, "libtalloc.so")
        val tallocSymlink = File(libOverrideDir, "libtalloc.so.2")

        if (tallocSymlink.exists()) {
            Log.d(TAG, "libtalloc.so.2 symlink already exists at ${tallocSymlink.absolutePath}")
            return
        }

        if (!tallocSource.exists()) {
            Log.e(TAG, "libtalloc.so not found in native lib dir")
            return
        }

        try {
            // Use Runtime.exec to create symlink
            val process = Runtime.getRuntime().exec(
                arrayOf("ln", "-sf", tallocSource.absolutePath, tallocSymlink.absolutePath)
            )
            val exitCode = process.waitFor()
            if (exitCode == 0) {
                Log.i(TAG, "Created symlink: ${tallocSymlink.absolutePath} -> ${tallocSource.absolutePath}")
            } else {
                Log.e(TAG, "Failed to create symlink, exit code: $exitCode")
                // Fallback: copy the file instead
                tryCopyTalloc(tallocSource, tallocSymlink)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to create libtalloc symlink", e)
            // Fallback: copy the file
            tryCopyTalloc(tallocSource, tallocSymlink)
        }
    }

    private fun tryCopyTalloc(source: File, dest: File) {
        try {
            source.copyTo(dest, overwrite = true)
            Log.i(TAG, "Copied libtalloc.so to ${dest.absolutePath} as fallback")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to copy libtalloc as fallback", e)
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
        val fullCommand = listOf(prootBinary.absolutePath) + prootArgs + listOf("/bin/sh", "-c", command)

        Log.d(TAG, "Executing: ${fullCommand.joinToString(" ")}")

        return try {
            val processBuilder = ProcessBuilder(fullCommand).apply {
                // Set working directory to app's files dir
                directory(File(context.filesDir.absolutePath))

                // Merge environment variables
                environment().apply {
                    putAll(buildBaseEnvironment())
                    putAll(environment)
                }

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

            // Shared memory
            "--bind=$tmpPath/shm:/dev/shm",

            // X11 socket
            "--bind=$x11SocketPath:/tmp/.X11-unix",

            // Proc filesystem (limited in proot)
            "--bind=/proc:/proc",

            // System info
            "--bind=/sys:/sys",

            // Android linker (for Vulkan passthrough)
            "--bind=/system:/system",
            "--bind=/vendor:/vendor",

            // Temporary directory
            "--bind=$tmpPath:/tmp",

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

        // Try to run proot --version
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
