package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.apache.commons.compress.archivers.ar.ArArchiveInputStream
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.compressors.xz.XZCompressorInputStream
import org.apache.commons.compress.compressors.zstandard.ZstdCompressorInputStream
import java.io.*
import java.util.concurrent.TimeUnit
import java.util.zip.GZIPInputStream

/**
 * Manages the Linux container (rootfs) lifecycle.
 * Handles downloading, extracting, and configuring the Ubuntu rootfs
 * with Box64, Box86, Steam, and required dependencies.
 */
class ContainerManager(private val context: Context) {

    companion object {
        private const val TAG = "ContainerManager"

        // Ubuntu 22.04 arm64 rootfs URLs (use one of these)
        private const val ROOTFS_URL_PRIMARY =
            "https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04-base-arm64.tar.gz"

        // Marker files to check if container is properly set up
        private const val MARKER_ROOTFS = ".rootfs_installed"
        private const val MARKER_BOX64 = ".box64_installed"
        private const val MARKER_BOX86 = ".box86_installed"
        private const val MARKER_STEAM = ".steam_installed"
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    private val rootfsDir: File
        get() = File(app.getRootfsDir())

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(5, TimeUnit.MINUTES)
        .writeTimeout(5, TimeUnit.MINUTES)
        .build()

    fun isContainerReady(): Boolean {
        val markers = listOf(MARKER_ROOTFS, MARKER_BOX64, MARKER_BOX86)
        return markers.all { File(rootfsDir, it).exists() }
    }

    fun isSteamInstalled(): Boolean {
        return File(rootfsDir, MARKER_STEAM).exists() ||
                File(rootfsDir, "opt/steam/steam.sh").exists() ||
                File(rootfsDir, "home/user/.steam/steam/steam.sh").exists()
    }

    suspend fun setupContainer(
        progressCallback: (progress: Int, message: String) -> Unit
    ) = withContext(Dispatchers.IO) {
        try {
            // Phase 1: Download and extract rootfs (0-40%)
            if (!File(rootfsDir, MARKER_ROOTFS).exists()) {
                progressCallback(0, "Downloading Ubuntu rootfs...")
                downloadAndExtractRootfs(progressCallback)
                File(rootfsDir, MARKER_ROOTFS).createNewFile()
            }
            progressCallback(40, "Rootfs installed")

            // Phase 2: Configure base system (40-50%)
            progressCallback(42, "Configuring base system...")
            configureBaseSystem()
            progressCallback(50, "Base system configured")

            // Phase 3: Install Box64 (50-65%)
            if (!File(rootfsDir, MARKER_BOX64).exists()) {
                progressCallback(52, "Installing Box64...")
                installBox64()
                File(rootfsDir, MARKER_BOX64).createNewFile()
            }
            progressCallback(65, "Box64 installed")

            // Phase 4: Install Box86 (65-80%)
            if (!File(rootfsDir, MARKER_BOX86).exists()) {
                progressCallback(67, "Installing Box86...")
                installBox86()
                File(rootfsDir, MARKER_BOX86).createNewFile()
            }
            progressCallback(80, "Box86 installed")

            // Phase 5: Setup Steam dependencies (80-90%)
            progressCallback(82, "Setting up Steam dependencies...")
            setupSteamDependencies()
            progressCallback(90, "Dependencies installed")

            // Phase 6: Setup Vulkan (90-95%)
            progressCallback(92, "Configuring Vulkan...")
            setupVulkan()
            setupVortek()
            progressCallback(95, "Vulkan configured")

            // Phase 7: Create user and finalize (95-100%)
            progressCallback(97, "Finalizing setup...")
            finalizeSetup()
            progressCallback(100, "Setup complete!")

        } catch (e: Exception) {
            Log.e(TAG, "Container setup failed", e)
            throw ContainerSetupException("Setup failed: ${e.message}", e)
        }
    }

    private suspend fun downloadAndExtractRootfs(
        progressCallback: (progress: Int, message: String) -> Unit
    ) = withContext(Dispatchers.IO) {
        val downloadFile = File(context.cacheDir, "rootfs.tar.gz")

        try {
            // Download rootfs
            val request = Request.Builder()
                .url(ROOTFS_URL_PRIMARY)
                .build()

            httpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) {
                    throw IOException("Download failed: ${response.code}")
                }

                val body = response.body ?: throw IOException("Empty response body")
                val contentLength = body.contentLength()
                var bytesRead = 0L

                FileOutputStream(downloadFile).use { output ->
                    body.byteStream().use { input ->
                        val buffer = ByteArray(8192)
                        var read: Int

                        while (input.read(buffer).also { read = it } != -1) {
                            output.write(buffer, 0, read)
                            bytesRead += read

                            if (contentLength > 0) {
                                val progress = ((bytesRead.toFloat() / contentLength) * 30).toInt()
                                progressCallback(progress, "Downloading: ${bytesRead / 1024 / 1024}MB")
                            }
                        }
                    }
                }
            }

            progressCallback(30, "Extracting rootfs...")

            // Extract rootfs
            rootfsDir.mkdirs()
            extractTarGz(downloadFile, rootfsDir)

            progressCallback(40, "Extraction complete")

        } finally {
            downloadFile.delete()
        }
    }

    private fun extractTarGz(archive: File, destDir: File) {
        FileInputStream(archive).use { fis ->
            GZIPInputStream(fis).use { gzis ->
                TarArchiveInputStream(gzis).use { tar ->
                    var entry = tar.nextTarEntry
                    while (entry != null) {
                        val destFile = File(destDir, entry.name)

                        when {
                            entry.isDirectory -> {
                                destFile.mkdirs()
                            }
                            entry.isSymbolicLink -> {
                                // Handle symlinks
                                destFile.parentFile?.mkdirs()
                                val linkTarget = entry.linkName
                                try {
                                    // Delete any existing file first
                                    destFile.delete()
                                    // Create symlink using Runtime.exec
                                    val process = Runtime.getRuntime().exec(
                                        arrayOf("ln", "-sf", linkTarget, destFile.absolutePath)
                                    )
                                    process.waitFor()
                                    Log.d(TAG, "Created symlink: ${entry.name} -> $linkTarget")
                                } catch (e: Exception) {
                                    Log.w(TAG, "Failed to create symlink ${entry.name} -> $linkTarget: ${e.message}")
                                }
                            }
                            entry.isLink -> {
                                // Handle hard links
                                destFile.parentFile?.mkdirs()
                                val linkTarget = File(destDir, entry.linkName)
                                try {
                                    destFile.delete()
                                    linkTarget.copyTo(destFile, overwrite = true)
                                } catch (e: Exception) {
                                    Log.w(TAG, "Failed to create hard link ${entry.name}: ${e.message}")
                                }
                            }
                            else -> {
                                // Regular file
                                destFile.parentFile?.mkdirs()
                                FileOutputStream(destFile).use { output ->
                                    tar.copyTo(output)
                                }

                                // Preserve executable permission
                                if (entry.mode and 0b001_000_000 != 0) {
                                    destFile.setExecutable(true)
                                }
                            }
                        }
                        entry = tar.nextTarEntry
                    }
                }
            }
        }
    }

    private fun configureBaseSystem() {
        // Create essential directories (including steam dir to avoid proot mkdir issues)
        listOf(
            "proc", "sys", "dev", "dev/pts", "dev/shm",
            "tmp", "tmp/.X11-unix", "run", "var/run",
            "home/user", "home/user/.steam", "home/user/.local", "home/user/.local/share",
            "opt", "opt/steam", "opt/scripts"
        ).forEach { dir ->
            File(rootfsDir, dir).mkdirs()
        }

        // Write resolv.conf for DNS
        File(rootfsDir, "etc/resolv.conf").writeText("""
            nameserver 8.8.8.8
            nameserver 8.8.4.4
        """.trimIndent())

        // Write hosts file
        File(rootfsDir, "etc/hosts").writeText("""
            127.0.0.1 localhost
            ::1 localhost
        """.trimIndent())

        // Create passwd entry for user
        File(rootfsDir, "etc/passwd").appendText("\nuser:x:1000:1000:User:/home/user:/bin/bash\n")
        File(rootfsDir, "etc/group").appendText("\nuser:x:1000:\n")

        // Create .bashrc for user
        File(rootfsDir, "home/user/.bashrc").writeText("""
            export HOME=/home/user
            export PATH=/usr/local/bin:/usr/bin:/bin:/opt/box64:/opt/box86
            export DISPLAY=:0
            export BOX64_LOG=1
            export BOX86_LOG=1
            export BOX64_LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu
            export BOX86_LD_LIBRARY_PATH=/usr/lib/i386-linux-gnu:/lib/i386-linux-gnu

            alias ll='ls -la'
            alias steam='box32 /opt/steam/steam.sh'
        """.trimIndent())
    }

    private fun installBox64() {
        // Extract pre-compiled Box64 from assets
        val box64Dir = File(rootfsDir, "usr/local/bin")
        box64Dir.mkdirs()
        val box64File = File(box64Dir, "box64")

        try {
            // Extract box64.xz from assets
            context.assets.open("box64.xz").use { input ->
                XZCompressorInputStream(input).use { xzInput ->
                    FileOutputStream(box64File).use { output ->
                        xzInput.copyTo(output)
                    }
                }
            }
            box64File.setExecutable(true)

            // Create box32 symlink (Box64 with BOX32 support handles 32-bit x86)
            val box32File = File(box64Dir, "box32")
            box32File.delete()
            Runtime.getRuntime().exec(
                arrayOf("ln", "-sf", "box64", box32File.absolutePath)
            ).waitFor()

            Log.i(TAG, "Box64 installed: ${box64File.length()} bytes")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract Box64", e)
            throw e
        }
    }

    private fun installBox86() {
        // On 64-bit only devices, Box86 (ARM32) cannot run.
        // We use Box64 compiled with BOX32=ON which handles 32-bit x86 directly.
        // The box32 symlink was already created in installBox64().

        val box32File = File(rootfsDir, "usr/local/bin/box32")
        if (box32File.exists()) {
            Log.i(TAG, "Box32 (via Box64) already available - skipping Box86")
        } else {
            // Ensure box32 symlink exists
            val box64File = File(rootfsDir, "usr/local/bin/box64")
            if (box64File.exists()) {
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", "box64", box32File.absolutePath)
                ).waitFor()
                Log.i(TAG, "Created box32 symlink to box64")
            }
        }
    }

    private fun setupSteamDependencies() {
        // Extract pre-bundled x86 libraries from assets
        // These are needed for Box64's BOX32 to run 32-bit x86 Steam

        val libDir = File(rootfsDir, "lib")
        val usrLibDir = File(rootfsDir, "usr/lib")
        libDir.mkdirs()
        usrLibDir.mkdirs()

        try {
            // Extract x86-libs.tar.xz from assets
            context.assets.open("x86-libs.tar.xz").use { input ->
                XZCompressorInputStream(input).use { xzInput ->
                    TarArchiveInputStream(xzInput).use { tar ->
                        var entry = tar.nextTarEntry
                        while (entry != null) {
                            val destFile = File(libDir, entry.name)

                            when {
                                entry.isDirectory -> destFile.mkdirs()
                                entry.isSymbolicLink -> {
                                    destFile.parentFile?.mkdirs()
                                    try {
                                        destFile.delete()
                                        Runtime.getRuntime().exec(
                                            arrayOf("ln", "-sf", entry.linkName, destFile.absolutePath)
                                        ).waitFor()
                                    } catch (e: Exception) {
                                        Log.w(TAG, "Failed to create symlink: ${entry.name}")
                                    }
                                }
                                else -> {
                                    destFile.parentFile?.mkdirs()
                                    FileOutputStream(destFile).use { output ->
                                        tar.copyTo(output)
                                    }
                                    if (entry.mode and 0b001_000_000 != 0) {
                                        destFile.setExecutable(true)
                                    }
                                }
                            }
                            entry = tar.nextTarEntry
                        }
                    }
                }
            }

            // Create ld-linux.so.2 symlink in /lib if needed
            val ldLinux = File(libDir, "ld-linux.so.2")
            val ldLinuxSource = File(libDir, "i386-linux-gnu/ld-linux.so.2")
            if (!ldLinux.exists() && ldLinuxSource.exists()) {
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", "i386-linux-gnu/ld-linux.so.2", ldLinux.absolutePath)
                ).waitFor()
            }

            Log.i(TAG, "x86 libraries installed successfully")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to extract x86 libraries", e)
            // Don't throw - Steam may still partially work
        }
    }

    private fun setupVulkan() {
        // Create Vulkan ICD for Android passthrough (legacy, won't work from glibc)
        val icdJson = """
            {
                "file_format_version": "1.0.0",
                "ICD": {
                    "library_path": "/system/lib64/libvulkan.so",
                    "api_version": "1.3.0"
                }
            }
        """.trimIndent()

        val icdDir = File(rootfsDir, "usr/share/vulkan/icd.d")
        icdDir.mkdirs()
        File(icdDir, "android_icd.json").writeText(icdJson)

        // Create a Vulkan setup script
        val vulkanScript = """
            #!/bin/bash
            # Vulkan environment setup for Android passthrough

            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/android_icd.json
            export MESA_VK_WSI_PRESENT_MODE=fifo
            export VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d

            # For DXVK/Proton
            export DXVK_ASYNC=1
            export PROTON_USE_WINED3D=0

            # Test Vulkan
            vulkaninfo --summary 2>/dev/null || echo "vulkaninfo not available"
        """.trimIndent()

        writeScript("setup_vulkan.sh", vulkanScript)
    }

    /**
     * Setup Vortek Vulkan passthrough.
     *
     * Vortek provides IPC-based Vulkan passthrough that solves the glibc/Bionic
     * incompatibility. The container uses libvulkan_vortek.so (ARM64) which
     * communicates with VortekRenderer (Android, Bionic) via Unix socket.
     *
     * Note: The library is ARM64 because Box64 handles x86→ARM translation.
     * Vulkan calls from x86 games go through Box64 which calls the ARM64
     * libvulkan_vortek.so.
     */
    private fun setupVortek() {
        // Setup Vortek ICD configuration
        VulkanBridge.setupVortekIcd(rootfsDir.absolutePath)

        // Create lib directory for Vortek client (ARM64 library)
        val libDir = File(rootfsDir, "lib")
        libDir.mkdirs()

        // Try to install Vortek client library from assets
        installVortekClient(libDir)

        // Install Vulkan test tools (vkcube, vulkaninfo)
        installVulkanTools()

        // Install headless surface support for vkcube/vulkaninfo without X11
        installHeadlessSurface()

        // Create Vortek environment script
        val vortekScript = """
            #!/bin/bash
            # Vortek Vulkan passthrough environment setup
            #
            # Vortek provides IPC-based Vulkan passthrough:
            # - Container (glibc): libvulkan_vortek.so serializes Vulkan commands
            # - Android (Bionic): VortekRenderer executes on real Mali GPU
            #
            # VORTEK_SERVER_PATH is set by the Android app when launching

            # Use Vortek as the Vulkan ICD
            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json

            # Disable direct driver loading (won't work anyway)
            export VK_DRIVER_FILES=""

            # DXVK optimizations for Mali
            export DXVK_ASYNC=1
            export DXVK_STATE_CACHE=1
            export DXVK_LOG_LEVEL=none

            # Mali-specific settings
            export MALI_NO_ASYNC_COMPUTE=1

            # Check Vortek status
            if [ -n "${'$'}VORTEK_SERVER_PATH" ]; then
                if [ -S "${'$'}VORTEK_SERVER_PATH" ]; then
                    echo "Vortek server socket found at ${'$'}VORTEK_SERVER_PATH"
                else
                    echo "WARNING: Vortek socket not found at ${'$'}VORTEK_SERVER_PATH"
                fi
            else
                echo "WARNING: VORTEK_SERVER_PATH not set"
            fi

            # Test Vulkan
            if command -v vulkaninfo &> /dev/null; then
                vulkaninfo --summary 2>&1
            else
                echo "vulkaninfo not available - install vulkan-tools to test"
            fi
        """.trimIndent()

        writeScript("setup_vortek.sh", vortekScript)
        Log.i(TAG, "Vortek passthrough configured")
    }

    /**
     * Install the Vortek client library (libvulkan_vortek.so) into the container.
     * This is the ARM64 Vulkan ICD that communicates with Android's VortekRenderer.
     * Box64 handles x86→ARM translation, so the library is ARM64.
     */
    private fun installVortekClient(targetDir: File) {
        try {
            // Try to extract from assets
            val assetName = "libvulkan_vortek.so"
            val targetFile = File(targetDir, assetName)

            try {
                context.assets.open(assetName).use { input ->
                    FileOutputStream(targetFile).use { output ->
                        input.copyTo(output)
                    }
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "Vortek client library installed from assets: ${targetFile.length()} bytes")
                return
            } catch (e: IOException) {
                Log.d(TAG, "Vortek client library not in assets: ${e.message}")
            }

            // Try to extract from native libs directory
            val nativeLibDir = context.applicationInfo.nativeLibraryDir
            val sourceFile = File(nativeLibDir, assetName)
            if (sourceFile.exists()) {
                sourceFile.copyTo(targetFile, overwrite = true)
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "Vortek client library installed from native libs")
                return
            }

            // Library not found - create a placeholder README
            Log.w(TAG, "Vortek client library not found - creating placeholder")
            File(targetDir, "README_VORTEK.txt").writeText("""
                Vortek Vulkan Passthrough
                =========================

                The libvulkan_vortek.so library is required for Vulkan support.

                To obtain this library:
                1. Download Winlator APK from https://winlator.com or GitHub
                2. Extract the APK (it's a ZIP file)
                3. Extract assets/graphics_driver/vortek-2.0.tzst (zstd compressed tar)
                4. Copy usr/lib/libvulkan_vortek.so to this directory

                Without this library, Vulkan/DXVK games will not work.
            """.trimIndent())

        } catch (e: Exception) {
            Log.e(TAG, "Failed to install Vortek client", e)
        }
    }

    /**
     * Install Vulkan test tools (vkcube, vulkaninfo) from assets.
     */
    private fun installVulkanTools() {
        val binDir = File(rootfsDir, "usr/local/bin")
        binDir.mkdirs()

        // Install vkcube
        try {
            context.assets.open("vkcube").use { input ->
                val targetFile = File(binDir, "vkcube")
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "vkcube installed: ${targetFile.length()} bytes")
            }
        } catch (e: IOException) {
            Log.d(TAG, "vkcube not in assets: ${e.message}")
        }

        // Install vulkaninfo if available
        try {
            context.assets.open("vulkaninfo").use { input ->
                val targetFile = File(binDir, "vulkaninfo")
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "vulkaninfo installed: ${targetFile.length()} bytes")
            }
        } catch (e: IOException) {
            Log.d(TAG, "vulkaninfo not in assets: ${e.message}")
        }
    }

    /**
     * Install headless surface support for Vulkan apps that don't have X11.
     * This provides VK_EXT_headless_surface via an LD_PRELOAD wrapper.
     *
     * Apps like vkcube normally require X11 to create a VkSurfaceKHR, but
     * with headless surface support, they can render to an offscreen surface
     * which Vortek then captures and displays on Android.
     */
    private fun installHeadlessSurface() {
        val tmpDir = File(rootfsDir, "tmp")
        val optDir = File(rootfsDir, "opt")
        val libDir = File(rootfsDir, "lib")
        val binDir = File(rootfsDir, "usr/local/bin")
        tmpDir.mkdirs()
        optDir.mkdirs()
        libDir.mkdirs()
        binDir.mkdirs()

        // Install pre-compiled libvulkan_headless.so to /lib/
        // This is cross-compiled for ARM64 Linux (glibc) and provides VK_EXT_headless_surface
        try {
            context.assets.open("libvulkan_headless.so").use { input ->
                val targetFile = File(libDir, "libvulkan_headless.so")
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "libvulkan_headless.so installed to /lib: ${targetFile.length()} bytes")
            }
        } catch (e: IOException) {
            Log.w(TAG, "libvulkan_headless.so not in assets, will need manual compilation: ${e.message}")
        }

        // Install fake libxcb.so for headless X11 (intercepts XCB calls)
        // NOTE: Must deploy to the ACTUAL tmp dir used by proot, not rootfs/tmp
        val actualTmpDir = File(context.cacheDir, "tmp")
        actualTmpDir.mkdirs()
        try {
            context.assets.open("libfakexcb.so").use { input ->
                val targetFile = File(actualTmpDir, "libfakexcb.so")
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "libfakexcb.so installed to ${actualTmpDir.absolutePath}: ${targetFile.length()} bytes")
            }
        } catch (e: IOException) {
            Log.w(TAG, "libfakexcb.so not in assets: ${e.message}")
        }

        // Install vulkan_headless.c source to /tmp (backup for manual compilation)
        try {
            context.assets.open("vulkan_headless.c").use { input ->
                val targetFile = File(tmpDir, "vulkan_headless.c")
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setReadable(true, false)
                Log.i(TAG, "vulkan_headless.c installed to /tmp: ${targetFile.length()} bytes")
            }
        } catch (e: IOException) {
            Log.d(TAG, "vulkan_headless.c not in assets: ${e.message}")
        }

        // Install setup_headless.sh script
        try {
            context.assets.open("setup_headless.sh").use { input ->
                val targetFile = File(binDir, "setup_headless.sh")
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "setup_headless.sh installed: ${targetFile.length()} bytes")
            }
        } catch (e: IOException) {
            Log.d(TAG, "setup_headless.sh not in assets: ${e.message}")
        }

        // Create a convenient wrapper script for running apps with headless surface
        val wrapperScript = """
            #!/bin/bash
            # Run Vulkan apps with headless surface support
            # Usage: vk-headless <command> [args...]
            #
            # This wrapper enables VK_EXT_headless_surface for apps that need a
            # window surface but don't have X11. The app renders to a headless
            # surface which Vortek captures and displays on Android.

            HEADLESS_LIB=/lib/libvulkan_headless.so

            if [ ! -f "${'$'}HEADLESS_LIB" ]; then
                echo "Headless surface library not found."
                echo "Run 'setup_headless.sh' first to compile it."
                exit 1
            fi

            if [ ${'$'}# -eq 0 ]; then
                echo "Usage: vk-headless <command> [args...]"
                echo ""
                echo "Examples:"
                echo "  vk-headless vkcube"
                echo "  vk-headless vulkaninfo --summary"
                exit 0
            fi

            exec env LD_PRELOAD="${'$'}HEADLESS_LIB" "${'$'}@"
        """.trimIndent()

        File(binDir, "vk-headless").apply {
            writeText(wrapperScript)
            setExecutable(true)
        }
        Log.i(TAG, "vk-headless wrapper script installed")
    }

    private fun finalizeSetup() {
        // Create Steam installation script (user runs this manually or on first launch)
        // Note: Steam .deb is downloaded by Android (downloadSteamDeb) before this script runs
        val steamScript = """
            #!/bin/bash

            echo "=== Steam Installation Script ==="
            echo "Working directory: ${'$'}(pwd)"

            # Check if steam_latest.deb exists (downloaded by Android app)
            if [ ! -f steam_latest.deb ]; then
                echo "ERROR: steam_latest.deb not found!"
                echo "The Android app should have downloaded it first."
                ls -la
                exit 1
            fi
            echo "Found steam_latest.deb: ${'$'}(ls -lh steam_latest.deb | awk '{print ${'$'}5}')"

            # Extract Steam .deb (don't use dpkg in proot)
            echo "Extracting Steam .deb..."
            ar x steam_latest.deb 2>&1
            if [ ${'$'}? -ne 0 ]; then
                echo "ar command failed, trying alternative..."
                # Alternative: use tar to peek inside
                ls -la
            fi

            # Extract data archive
            echo "Extracting data archive..."
            if [ -f data.tar.xz ]; then
                tar xf data.tar.xz 2>&1
                echo "Extracted data.tar.xz"
            elif [ -f data.tar.gz ]; then
                tar xzf data.tar.gz 2>&1
                echo "Extracted data.tar.gz"
            elif [ -f data.tar.zst ]; then
                zstd -d data.tar.zst -o data.tar 2>&1 && tar xf data.tar 2>&1
                echo "Extracted data.tar.zst"
            else
                echo "Contents after ar extraction:"
                ls -la
                echo "No recognized data archive found"
            fi

            # Check what we got
            echo ""
            echo "Checking extracted files..."
            if [ -d usr ]; then
                echo "Found usr directory:"
                ls -la usr/ 2>/dev/null | head -10
                ls -la usr/bin/ 2>/dev/null | head -5
                ls -la usr/lib/steam/ 2>/dev/null | head -5
            fi

            # Find steam binary/script
            STEAM_SH=${'$'}(find . -name "steam" -type f 2>/dev/null | head -1)
            if [ -n "${'$'}STEAM_SH" ]; then
                echo ""
                echo "Steam binary found at: ${'$'}STEAM_SH"
                chmod +x "${'$'}STEAM_SH" 2>/dev/null
            fi

            echo ""
            echo "=== Steam installation complete! ==="
        """.trimIndent()

        writeScript("install_steam.sh", steamScript)

        // Create main entry point script
        val entryScript = """
            #!/bin/bash
            # Entry point for Steam Launcher container

            export HOME=/home/user
            export USER=user
            export DISPLAY=:0
            export LANG=en_US.UTF-8

            # Source user bashrc
            [ -f ~/.bashrc ] && source ~/.bashrc

            # Check if this is first run
            if [ ! -f /opt/.setup_complete ]; then
                echo "First run detected. Running setup..."

                # Run installation scripts if Box64/Box86 not present
                [ ! -f /usr/local/bin/box64 ] && bash /opt/scripts/install_box64.sh
                [ ! -f /usr/local/bin/box86 ] && bash /opt/scripts/install_box86.sh

                # Install dependencies
                bash /opt/scripts/install_deps.sh

                # Setup Vulkan
                bash /opt/scripts/setup_vulkan.sh

                touch /opt/.setup_complete
                echo "Setup complete!"
            fi

            # Execute command or start shell
            if [ ${'$'}# -gt 0 ]; then
                exec "${'$'}@"
            else
                exec /bin/bash
            fi
        """.trimIndent()

        val scriptsDir = File(rootfsDir, "opt/scripts")
        scriptsDir.mkdirs()
        File(scriptsDir, "entry.sh").apply {
            writeText(entryScript)
            setExecutable(true)
        }

        // Set correct permissions
        File(rootfsDir, "home/user").apply {
            setReadable(true, false)
            setWritable(true, false)
            setExecutable(true, false)
        }

        Log.i(TAG, "Container setup finalized")
    }

    private fun writeScript(name: String, content: String) {
        val scriptsDir = File(rootfsDir, "opt/scripts")
        scriptsDir.mkdirs()
        File(scriptsDir, name).apply {
            writeText(content)
            setExecutable(true)
        }
    }

    /**
     * Download and extract Steam .deb file from Android side
     */
    suspend fun downloadAndExtractSteam(): String = withContext(Dispatchers.IO) {
        val result = StringBuilder()
        val steamDebUrl = "https://cdn.cloudflare.steamstatic.com/client/installer/steam.deb"
        val debFile = File(context.cacheDir, "steam.deb")
        val steamDir = File(rootfsDir, "home/user")

        try {
            // Download .deb if needed
            if (!debFile.exists() || debFile.length() < 1000000) {
                result.appendLine("Downloading Steam...")
                val request = Request.Builder().url(steamDebUrl).build()
                httpClient.newCall(request).execute().use { response ->
                    if (!response.isSuccessful) {
                        return@withContext "Download failed: ${response.code}"
                    }
                    response.body?.byteStream()?.use { input ->
                        FileOutputStream(debFile).use { output ->
                            input.copyTo(output)
                        }
                    }
                }
                result.appendLine("Downloaded: ${debFile.length() / 1024 / 1024}MB")
            } else {
                result.appendLine("Using cached Steam .deb")
            }

            // Extract .deb (ar archive)
            result.appendLine("Extracting .deb archive...")
            var dataExtracted = false

            FileInputStream(debFile).use { fis ->
                ArArchiveInputStream(fis).use { ar ->
                    var entry = ar.nextArEntry
                    while (entry != null) {
                        result.appendLine("  Found: ${entry.name}")

                        if (entry.name.startsWith("data.tar")) {
                            // Extract data archive to rootfs
                            result.appendLine("Extracting ${entry.name} to rootfs...")

                            val tarStream: InputStream = when {
                                entry.name.endsWith(".xz") -> XZCompressorInputStream(ar)
                                entry.name.endsWith(".gz") -> GZIPInputStream(ar)
                                entry.name.endsWith(".zst") -> ZstdCompressorInputStream(ar)
                                else -> ar
                            }

                            TarArchiveInputStream(tarStream).use { tar ->
                                var tarEntry = tar.nextTarEntry
                                var fileCount = 0
                                while (tarEntry != null) {
                                    val destFile = File(steamDir, tarEntry.name)

                                    when {
                                        tarEntry.isDirectory -> destFile.mkdirs()
                                        tarEntry.isSymbolicLink -> {
                                            destFile.parentFile?.mkdirs()
                                            try {
                                                destFile.delete()
                                                Runtime.getRuntime().exec(
                                                    arrayOf("ln", "-sf", tarEntry.linkName, destFile.absolutePath)
                                                ).waitFor()
                                            } catch (e: Exception) {
                                                // Ignore symlink errors
                                            }
                                        }
                                        else -> {
                                            destFile.parentFile?.mkdirs()
                                            FileOutputStream(destFile).use { out ->
                                                tar.copyTo(out)
                                            }
                                            if (tarEntry.mode and 0b001_000_000 != 0) {
                                                destFile.setExecutable(true)
                                            }
                                            fileCount++
                                        }
                                    }
                                    tarEntry = tar.nextTarEntry
                                }
                                result.appendLine("Extracted $fileCount files")
                            }
                            dataExtracted = true
                            // Break after extracting data archive (stream is consumed)
                            break
                        }
                        entry = ar.nextArEntry
                    }
                }
            }

            if (!dataExtracted) {
                result.appendLine("ERROR: No data archive found in .deb")
                return@withContext result.toString()
            }

            // Check for steam binary
            val steamBin = File(steamDir, "usr/bin/steam")
            val steamLib = File(steamDir, "usr/lib/steam")
            result.appendLine("")
            result.appendLine("Steam binary exists: ${steamBin.exists()}")
            result.appendLine("Steam lib exists: ${steamLib.exists()}")

            if (steamBin.exists()) {
                steamBin.setExecutable(true)
            }

            // Extract Steam bootstrap (rootfs doesn't have xz, so we do it from Android)
            val bootstrapFile = File(steamLib, "bootstraplinux_ubuntu12_32.tar.xz")
            if (bootstrapFile.exists()) {
                result.appendLine("")
                result.appendLine("Extracting Steam bootstrap...")
                val steamDataDir = File(steamDir, ".local/share/Steam")
                steamDataDir.mkdirs()

                try {
                    FileInputStream(bootstrapFile).use { fis ->
                        XZCompressorInputStream(fis).use { xzInput ->
                            TarArchiveInputStream(xzInput).use { tar ->
                                var tarEntry = tar.nextTarEntry
                                var fileCount = 0
                                while (tarEntry != null) {
                                    val destFile = File(steamDataDir, tarEntry.name)

                                    when {
                                        tarEntry.isDirectory -> destFile.mkdirs()
                                        tarEntry.isSymbolicLink -> {
                                            destFile.parentFile?.mkdirs()
                                            try {
                                                destFile.delete()
                                                Runtime.getRuntime().exec(
                                                    arrayOf("ln", "-sf", tarEntry.linkName, destFile.absolutePath)
                                                ).waitFor()
                                            } catch (e: Exception) {
                                                // Ignore symlink errors
                                            }
                                        }
                                        else -> {
                                            destFile.parentFile?.mkdirs()
                                            FileOutputStream(destFile).use { out ->
                                                tar.copyTo(out)
                                            }
                                            if (tarEntry.mode and 0b001_000_000 != 0) {
                                                destFile.setExecutable(true)
                                            }
                                            fileCount++
                                        }
                                    }
                                    tarEntry = tar.nextTarEntry
                                }
                                result.appendLine("Bootstrap extracted: $fileCount files")
                            }
                        }
                    }

                    // Create .steam symlink
                    val dotSteam = File(steamDir, ".steam")
                    dotSteam.mkdirs()
                    val steamSymlink = File(dotSteam, "steam")
                    if (!steamSymlink.exists()) {
                        Runtime.getRuntime().exec(
                            arrayOf("ln", "-sf", "../.local/share/Steam", steamSymlink.absolutePath)
                        ).waitFor()
                    }

                    result.appendLine("Steam data dir: ${steamDataDir.absolutePath}")
                } catch (e: Exception) {
                    result.appendLine("Bootstrap extraction error: ${e.message}")
                    Log.e(TAG, "Failed to extract Steam bootstrap", e)
                }
            }

            result.appendLine("")
            result.appendLine("=== Steam installation complete! ===")

            // Mark Steam as installed
            File(rootfsDir, MARKER_STEAM).createNewFile()

            result.toString()
        } catch (e: Exception) {
            Log.e(TAG, "Steam extraction failed", e)
            result.appendLine("ERROR: ${e.message}")
            result.toString()
        }
    }

    suspend fun runInContainer(command: String): String = withContext(Dispatchers.IO) {
        val result = StringBuilder()
        try {
            val process = app.prootExecutor.execute(
                command = command,
                environment = emptyMap(),
                workingDir = "/home/user"
            )

            process?.inputStream?.bufferedReader()?.use { reader ->
                reader.forEachLine { line ->
                    result.appendLine(line)
                }
            }

            process?.waitFor()
        } catch (e: Exception) {
            Log.e(TAG, "Command failed: $command", e)
            result.append("Error: ${e.message}")
        }
        result.toString()
    }

    fun cleanup() {
        if (rootfsDir.exists()) {
            rootfsDir.deleteRecursively()
        }
    }
}

class ContainerSetupException(message: String, cause: Throwable? = null) : Exception(message, cause)
