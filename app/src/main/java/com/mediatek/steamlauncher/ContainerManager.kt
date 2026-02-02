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
            alias steam='box86 /opt/steam/steam.sh'
        """.trimIndent())
    }

    private fun installBox64() {
        // Create Box64 installation script
        val installScript = """
            #!/bin/bash
            set -e

            # Update package lists
            apt-get update

            # Install build dependencies
            apt-get install -y cmake git build-essential python3

            # Clone and build Box64
            cd /opt
            git clone https://github.com/ptitSeb/box64.git
            cd box64
            mkdir build && cd build
            cmake .. -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
            make -j$(nproc)
            make install

            # Verify installation
            box64 --version || echo "Box64 installed"

            # Create binfmt configuration
            echo ':box64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x3e\x00:\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/local/bin/box64:' > /usr/share/binfmts/box64
        """.trimIndent()

        writeScript("install_box64.sh", installScript)
    }

    private fun installBox86() {
        // Create Box86 installation script
        val installScript = """
            #!/bin/bash
            set -e

            # Add armhf architecture for 32-bit libraries
            dpkg --add-architecture armhf || true
            apt-get update

            # Install 32-bit libraries
            apt-get install -y libc6:armhf libstdc++6:armhf || true

            # Clone and build Box86
            cd /opt
            git clone https://github.com/ptitSeb/box86.git
            cd box86
            mkdir build && cd build
            cmake .. -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
            make -j$(nproc)
            make install

            # Verify installation
            box86 --version || echo "Box86 installed"

            # Create binfmt configuration
            echo ':box86:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x03\x00:\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/local/bin/box86:' > /usr/share/binfmts/box86
        """.trimIndent()

        writeScript("install_box86.sh", installScript)
    }

    private fun setupSteamDependencies() {
        // Create dependency installation script
        val depsScript = """
            #!/bin/bash
            set -e

            # Enable multiarch
            dpkg --add-architecture i386 || true
            apt-get update

            # Install X11 and graphics dependencies
            apt-get install -y --no-install-recommends \
                xorg \
                x11-xserver-utils \
                libx11-6 \
                libxext6 \
                libxrandr2 \
                libxrender1 \
                libxcursor1 \
                libxcomposite1 \
                libxi6 \
                libxtst6 \
                libxkbfile1 \
                libxinerama1 \
                libxss1 \
                libgl1 \
                libglu1 \
                libegl1 \
                libgles2 \
                libvulkan1 \
                vulkan-tools \
                mesa-vulkan-drivers

            # Install 32-bit libraries for Steam (Box86)
            apt-get install -y --no-install-recommends \
                libc6:i386 \
                libstdc++6:i386 \
                libgl1:i386 \
                libx11-6:i386 \
                libxext6:i386 \
                libxrandr2:i386 \
                libxrender1:i386 \
                libxcursor1:i386 \
                libxi6:i386 \
                libglib2.0-0:i386 \
                libnss3:i386 \
                libnspr4:i386 \
                libfontconfig1:i386 \
                libpango-1.0-0:i386 \
                libcairo2:i386 \
                libatk1.0-0:i386 \
                libgdk-pixbuf2.0-0:i386 \
                libgtk-3-0:i386 \
                libasound2:i386 \
                libpulse0:i386 \
                libcurl4:i386 \
                libdbus-1-3:i386 || true

            # Install fonts
            apt-get install -y --no-install-recommends \
                fonts-liberation \
                fonts-dejavu-core

            # Install utilities
            apt-get install -y --no-install-recommends \
                wget \
                curl \
                ca-certificates \
                locales

            # Generate locale
            echo "en_US.UTF-8 UTF-8" > /etc/locale.gen
            locale-gen

            echo "Dependencies installed successfully"
        """.trimIndent()

        writeScript("install_deps.sh", depsScript)
    }

    private fun setupVulkan() {
        // Create Vulkan ICD for Android passthrough
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
                result.appendLine("")
                result.appendLine("=== Steam installation complete! ===")
            }

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
