package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader

/**
 * Manages a QEMU ARM64-on-ARM64 virtual machine (TCG mode, no KVM needed).
 *
 * The VM runs an ARM64 Linux kernel with vm.max_map_count=1048576,
 * solving the webhelper Chromium crash on Android's default ~65K limit.
 *
 * QEMU is bundled as libqemu.so in jniLibs for SELinux exec permission.
 * Launched via ProcessBuilder, same pattern as FexExecutor.
 */
class VmManager(private val context: Context) {

    companion object {
        private const val TAG = "VmManager"
    }

    private var qemuProcess: Process? = null

    /**
     * Get path to the bundled QEMU binary.
     */
    private fun getQemuPath(): String {
        val app = context as SteamLauncherApp
        return "${app.applicationInfo.nativeLibraryDir}/libqemu.so"
    }

    /**
     * Check if QEMU binary exists and is executable.
     */
    fun isAvailable(): String {
        val sb = StringBuilder()
        sb.appendLine("=== QEMU VM Check ===")

        val qemuPath = getQemuPath()
        val qemuFile = File(qemuPath)
        sb.appendLine("QEMU binary: $qemuPath")
        sb.appendLine("  exists: ${qemuFile.exists()}")
        sb.appendLine("  size: ${if (qemuFile.exists()) "${qemuFile.length() / 1024 / 1024}MB" else "N/A"}")
        sb.appendLine("  executable: ${qemuFile.canExecute()}")

        // Check for kernel and initrd
        val assetsDir = File(context.filesDir, "vm-assets")
        val kernelFile = File(assetsDir, "vm_kernel.gz")
        val initrdFile = File(assetsDir, "vm_initrd.gz")
        sb.appendLine("Kernel: ${if (kernelFile.exists()) "${kernelFile.length()} bytes" else "MISSING"}")
        sb.appendLine("Initrd: ${if (initrdFile.exists()) "${initrdFile.length()} bytes" else "MISSING"}")

        val result = sb.toString()
        Log.i(TAG, result)
        return result
    }

    /**
     * Run QEMU smoke test — boot minimal kernel+initrd, check vm.max_map_count.
     */
    fun smokeTest(outputCallback: (String) -> Unit) {
        outputCallback("=== QEMU Smoke Test ===\n")

        val qemuPath = getQemuPath()
        if (!File(qemuPath).exists()) {
            outputCallback("ERROR: QEMU binary not found at $qemuPath\n")
            return
        }

        // Extract kernel and initrd from assets
        val assetsDir = File(context.filesDir, "vm-assets")
        assetsDir.mkdirs()
        val kernelFile = File(assetsDir, "vm_kernel.gz")
        val initrdFile = File(assetsDir, "vm_initrd.gz")

        try {
            extractAsset("vm_kernel.gz", kernelFile)
            extractAsset("vm_initrd.gz", initrdFile)
        } catch (e: Exception) {
            outputCallback("ERROR: Could not extract VM assets: ${e.message}\n")
            return
        }

        if (kernelFile.length() < 1000) {
            outputCallback("ERROR: vm_kernel.gz is too small (${kernelFile.length()} bytes) — need a real ARM64 kernel\n")
            outputCallback("Push a kernel: adb push Image.gz /data/local/tmp/vm_kernel.gz\n")
            outputCallback("Then copy: run-as com.mediatek.steamlauncher cp /data/local/tmp/vm_kernel.gz files/vm-assets/vm_kernel.gz\n")
            return
        }

        outputCallback("QEMU: $qemuPath\n")
        outputCallback("Kernel: ${kernelFile.absolutePath} (${kernelFile.length()} bytes)\n")
        outputCallback("Initrd: ${initrdFile.absolutePath} (${initrdFile.length()} bytes)\n")
        outputCallback("Starting QEMU...\n")

        try {
            val cmd = listOf(
                qemuPath,
                "-machine", "virt",
                "-cpu", "max",
                "-m", "256",
                "-nographic",
                "-no-reboot",
                "-nodefaults",
                "-serial", "stdio",
                "-kernel", kernelFile.absolutePath,
                "-initrd", initrdFile.absolutePath,
                "-append", "console=ttyAMA0 panic=-1"
            )

            outputCallback("CMD: ${cmd.joinToString(" ")}\n\n")

            val pb = ProcessBuilder(cmd)
            pb.redirectErrorStream(true)
            pb.environment()["HOME"] = context.filesDir.absolutePath
            pb.environment()["TMPDIR"] = context.cacheDir.absolutePath

            val process = pb.start()
            qemuProcess = process

            // Read output in background
            Thread {
                try {
                    val reader = BufferedReader(InputStreamReader(process.inputStream))
                    var line: String?
                    while (reader.readLine().also { line = it } != null) {
                        val l = line
                        outputCallback("[VM] $l\n")
                    }
                    val exitCode = process.waitFor()
                    outputCallback("\n[QEMU exited with code $exitCode]\n")
                } catch (e: Exception) {
                    outputCallback("[QEMU read error: ${e.message}]\n")
                } finally {
                    qemuProcess = null
                }
            }.start()

        } catch (e: Exception) {
            outputCallback("ERROR: ${e.javaClass.simpleName}: ${e.message}\n")
            Log.e(TAG, "QEMU smoke test failed", e)
        }
    }

    /**
     * Stop the running QEMU VM.
     */
    fun stop() {
        qemuProcess?.let {
            it.destroy()
            qemuProcess = null
            Log.i(TAG, "QEMU stopped")
        }
    }

    /**
     * Check if QEMU is running.
     */
    fun isRunning(): Boolean = qemuProcess?.isAlive == true

    private fun extractAsset(assetName: String, targetFile: File) {
        if (targetFile.exists() && targetFile.length() > 0) return
        context.assets.open(assetName).use { input ->
            targetFile.outputStream().use { output ->
                input.copyTo(output)
            }
        }
    }
}
