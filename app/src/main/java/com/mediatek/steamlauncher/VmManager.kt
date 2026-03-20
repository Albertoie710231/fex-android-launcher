package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader

/**
 * Manages a QEMU x86-64 virtual machine (TCG mode on ARM64, no KVM needed).
 *
 * Steam runs natively as x86-64 inside the VM — no FEX needed.
 * The VM kernel has vm.max_map_count=1048576 for webhelper/Chromium.
 *
 * GPU passthrough via Vortek TCP bridge:
 *   VM → TCP → vortek_proxy → VortekRenderer → Mali GPU
 */
class VmManager(private val context: Context) {

    companion object {
        private const val TAG = "VmManager"
        private const val VORTEK_TCP_PORT = 5900
    }

    var qemuProcess: Process? = null
        private set
    private var proxyProcess: Process? = null

    private fun getQemuPath(): String {
        val app = context as SteamLauncherApp
        return "${app.applicationInfo.nativeLibraryDir}/libqemu.so"
    }

    private fun getApp(): SteamLauncherApp = context as SteamLauncherApp

    /**
     * Extract QEMU BIOS/ROM files from assets to app files dir.
     */
    private fun extractFirmware(): String {
        val fwDir = File(context.filesDir, "qemu-fw")
        fwDir.mkdirs()
        val fwFiles = listOf("bios-256k.bin", "kvmvapic.bin", "linuxboot_dma.bin",
            "linuxboot.bin", "efi-e1000.rom", "efi-virtio.rom", "vgabios-virtio.bin")
        for (name in fwFiles) {
            val target = File(fwDir, name)
            if (!target.exists() || target.length() == 0L) {
                try {
                    context.assets.open("qemu-fw/$name").use { inp ->
                        target.outputStream().use { out -> inp.copyTo(out) }
                    }
                } catch (_: Exception) {}
            }
        }
        return fwDir.absolutePath
    }

    /**
     * Boot x86-64 Ubuntu VM with networking and GPU bridge.
     */
    fun bootFull(outputCallback: (String) -> Unit) {
        outputCallback("=== Booting x86-64 Ubuntu VM ===\n")

        val qemuPath = getQemuPath()
        if (!File(qemuPath).exists()) {
            outputCallback("ERROR: QEMU binary not found\n")
            return
        }

        val dataDir = "/data/local/tmp"
        val rootfsPath = "$dataDir/vm_rootfs_x86.img"
        val kernelPath = "$dataDir/vm_kernel_x86"
        val initrdPath = "$dataDir/vm_initrd_x86.gz"

        outputCallback("Rootfs: $rootfsPath\n")
        outputCallback("Kernel: $kernelPath\n")
        outputCallback("Initrd: $initrdPath\n")

        // Extract BIOS firmware
        val fwDir = extractFirmware()
        outputCallback("Firmware: $fwDir\n")

        // Kill any old proxy/QEMU from previous runs
        killOldProcesses()

        // Start Vortek TCP proxy
        startVortekProxy(outputCallback)

        outputCallback("Starting QEMU x86-64 VM...\n\n")

        try {
            val cmd = listOf(
                qemuPath,
                "-machine", "q35",
                "-accel", "tcg,thread=multi",
                "-cpu", "qemu64",
                "-m", "8192",
                "-smp", "8",
                "-nographic",
                "-nodefaults",
                "-serial", "stdio",
                "-no-reboot",
                "-L", fwDir,
                // Networking with slirp + Vortek port forward
                "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:${VORTEK_TCP_PORT + 1}-:$VORTEK_TCP_PORT",
                "-device", "e1000,netdev=net0",
                // Rootfs disk
                "-drive", "file=$rootfsPath,format=raw,if=virtio",
                // Kernel + initrd
                "-kernel", kernelPath,
                "-initrd", initrdPath,
                "-append", "console=ttyS0 root=/dev/vda rw quiet"
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
                    var shellReady = false
                    while (reader.readLine().also { line = it } != null) {
                        val l = stripAnsiEscapes(line ?: "")
                        if (!shellReady) {
                            if (l.isNotBlank()) outputCallback("[boot] $l\n")
                            if (l.contains("root@") || l.contains("Last login")) {
                                shellReady = true
                                outputCallback("\n=== VM Shell Ready (x86-64 Ubuntu + Steam) ===\n")
                                outputCallback("root@steam-vm:~# \n")
                            }
                        } else {
                            if (l.isNotBlank()) outputCallback("$l\n")
                        }
                    }
                    val exitCode = process.waitFor()
                    outputCallback("\n[VM exited]\n")
                } catch (e: Exception) {
                    outputCallback("[VM error: ${e.message}]\n")
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
     * Start Vortek TCP proxy (bridges TCP port → Vortek Unix socket).
     */
    private fun startVortekProxy(outputCallback: (String) -> Unit) {
        val app = getApp()
        val vortekSocket = "${app.getTmpDir()}/vortek.sock"
        val proxyBin = File("${app.applicationInfo.nativeLibraryDir}/libvortek_proxy.so")

        if (!proxyBin.exists()) {
            outputCallback("[Proxy] WARNING: libvortek_proxy.so not found\n")
            return
        }

        if (!File(vortekSocket).exists()) {
            outputCallback("[Proxy] WARNING: Vortek socket not found — GPU bridge disabled\n")
            return
        }

        try {
            val pb = ProcessBuilder(proxyBin.absolutePath, VORTEK_TCP_PORT.toString(), vortekSocket)
            pb.redirectErrorStream(true)
            val process = pb.start()
            proxyProcess = process

            Thread {
                try {
                    val reader = BufferedReader(InputStreamReader(process.inputStream))
                    var line: String?
                    while (reader.readLine().also { line = it } != null) {
                        outputCallback("[Proxy] $line\n")
                    }
                } catch (_: Exception) {}
            }.start()

            outputCallback("[Proxy] Vortek proxy on port $VORTEK_TCP_PORT → $vortekSocket\n")
        } catch (e: Exception) {
            outputCallback("[Proxy] Failed: ${e.message}\n")
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
        killOldProcesses()
    }

    private fun killOldProcesses() {
        try {
            Runtime.getRuntime().exec(arrayOf("killall", "libvortek_proxy.so")).waitFor()
            Runtime.getRuntime().exec(arrayOf("killall", "vortek_proxy")).waitFor()
        } catch (_: Exception) {}
    }

    fun isRunning(): Boolean = qemuProcess?.isAlive == true

    private fun stripAnsiEscapes(s: String): String {
        return s.replace(Regex("\\x1b\\[[0-9;?]*[a-zA-Z]"), "")
                .replace(Regex("\\x1b\\]\\d+;[^\\x07]*\\x07"), "")
    }
}
