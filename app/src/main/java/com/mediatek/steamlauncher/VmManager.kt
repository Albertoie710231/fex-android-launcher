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
 * GPU passthrough via Vortek TCP bridge:
 *   VM (vortek_vm_bridge) → TCP → vortek_proxy → VortekRenderer → Mali GPU
 */
class VmManager(private val context: Context) {

    companion object {
        private const val TAG = "VmManager"
        private const val VORTEK_TCP_PORT = 5900
    }

    private var qemuProcess: Process? = null
    private var proxyProcess: Process? = null

    private fun getQemuPath(): String {
        val app = context as SteamLauncherApp
        return "${app.applicationInfo.nativeLibraryDir}/libqemu.so"
    }

    private fun getApp(): SteamLauncherApp = context as SteamLauncherApp

    /**
     * Boot full Ubuntu VM with networking and GPU bridge.
     */
    fun bootFull(outputCallback: (String) -> Unit) {
        outputCallback("=== Booting Ubuntu VM ===\n")

        val qemuPath = getQemuPath()
        if (!File(qemuPath).exists()) {
            outputCallback("ERROR: QEMU binary not found\n")
            return
        }

        // Check for rootfs, kernel, initrd on device
        val dataDir = "/data/local/tmp"
        val rootfsPath = "$dataDir/vm_rootfs.img"
        val kernelPath = "$dataDir/vm_kernel"
        val initrdPath = "$dataDir/vm_initrd.gz"

        // Also check app files dir for kernel/initrd (fallback)
        val appKernel = File(context.filesDir, "vm-assets/vm_kernel.gz")
        val appInitrd = File(context.filesDir, "vm-assets/vm_initrd.gz")

        // Use /data/local/tmp paths (pushed via adb)
        outputCallback("Rootfs: $rootfsPath\n")
        outputCallback("Kernel: $kernelPath\n")
        outputCallback("Initrd: $initrdPath\n")

        // Start Vortek TCP proxy (TCP 5900 → Vortek Unix socket)
        startVortekProxy(outputCallback)

        outputCallback("Starting QEMU with networking + GPU bridge...\n\n")

        try {
            val cmd = listOf(
                qemuPath,
                "-machine", "virt",
                "-cpu", "max",
                "-m", "2048",
                "-smp", "4",
                "-nographic",
                "-nodefaults",
                "-serial", "stdio",
                // Networking with slirp + Vortek port forward
                "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:${VORTEK_TCP_PORT + 1}-:$VORTEK_TCP_PORT",
                "-device", "virtio-net-device,netdev=net0",
                // Rootfs disk
                "-drive", "file=$rootfsPath,format=raw,if=virtio",
                // Kernel + initrd (external boot)
                "-kernel", kernelPath,
                "-initrd", initrdPath,
                "-append", "console=ttyAMA0 root=/dev/vda rw quiet"
            )

            outputCallback("CMD: ${cmd.joinToString(" ")}\n\n")

            val pb = ProcessBuilder(cmd)
            pb.redirectErrorStream(true)
            pb.environment()["HOME"] = context.filesDir.absolutePath
            pb.environment()["TMPDIR"] = context.cacheDir.absolutePath

            val process = pb.start()
            qemuProcess = process

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
                    stopVortekProxy()
                }
            }.start()

        } catch (e: Exception) {
            outputCallback("ERROR: ${e.javaClass.simpleName}: ${e.message}\n")
            Log.e(TAG, "VM boot failed", e)
        }
    }

    /**
     * Run smoke test — boot minimal kernel+initrd only.
     */
    fun smokeTest(outputCallback: (String) -> Unit) {
        outputCallback("=== QEMU Smoke Test ===\n")

        val qemuPath = getQemuPath()
        if (!File(qemuPath).exists()) {
            outputCallback("ERROR: QEMU binary not found at $qemuPath\n")
            return
        }

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
            outputCallback("ERROR: vm_kernel.gz too small — push a real ARM64 kernel\n")
            return
        }

        outputCallback("Kernel: ${kernelFile.length()} bytes\n")
        outputCallback("Initrd: ${initrdFile.length()} bytes\n")
        outputCallback("Starting QEMU...\n\n")

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

            val pb = ProcessBuilder(cmd)
            pb.redirectErrorStream(true)
            pb.environment()["HOME"] = context.filesDir.absolutePath
            pb.environment()["TMPDIR"] = context.cacheDir.absolutePath

            val process = pb.start()
            qemuProcess = process

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
            Log.e(TAG, "Smoke test failed", e)
        }
    }

    /**
     * Start Vortek TCP proxy (bridges TCP port → Vortek Unix socket).
     * The proxy binary is bundled as libvortek_proxy.so in jniLibs for SELinux exec permission.
     */
    private fun startVortekProxy(outputCallback: (String) -> Unit) {
        val app = getApp()
        val vortekSocket = "${app.getTmpDir()}/vortek.sock"
        val proxyBin = File("${app.applicationInfo.nativeLibraryDir}/libvortek_proxy.so")

        if (!proxyBin.exists()) {
            outputCallback("[Proxy] WARNING: libvortek_proxy.so not found in nativeLibDir\n")
            return
        }

        // Check if Vortek socket exists
        if (!File(vortekSocket).exists()) {
            outputCallback("[Proxy] WARNING: Vortek socket not found at $vortekSocket\n")
            outputCallback("[Proxy] VortekRenderer may not be running — GPU bridge disabled\n")
            return
        }

        try {
            val pb = ProcessBuilder(proxyBin.absolutePath, VORTEK_TCP_PORT.toString(), vortekSocket)
            pb.redirectErrorStream(true)
            val process = pb.start()
            proxyProcess = process

            // Read proxy output in background
            Thread {
                try {
                    val reader = BufferedReader(InputStreamReader(process.inputStream))
                    var line: String?
                    while (reader.readLine().also { line = it } != null) {
                        outputCallback("[Proxy] $line\n")
                    }
                } catch (_: Exception) {}
            }.start()

            outputCallback("[Proxy] Vortek TCP proxy started on port $VORTEK_TCP_PORT → $vortekSocket\n")
        } catch (e: Exception) {
            outputCallback("[Proxy] Failed to start proxy: ${e.message}\n")
        }
    }

    private fun stopVortekProxy() {
        proxyProcess?.let {
            it.destroy()
            proxyProcess = null
            Log.i(TAG, "Vortek proxy stopped")
        }
    }

    fun stop() {
        qemuProcess?.let {
            it.destroy()
            qemuProcess = null
            Log.i(TAG, "QEMU stopped")
        }
        stopVortekProxy()
    }

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
