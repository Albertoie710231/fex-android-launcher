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
 * Manages the FEX-Emu container lifecycle.
 *
 * Simplified architecture (no PRoot):
 *   App → ProcessBuilder → FEXLoader (ARM64) → x86-64 bash/Steam
 *
 * Setup:
 *   1. Extract FEX ARM64 binaries from bundled fex-bin.tgz
 *   2. Download x86-64 SquashFS rootfs from fex-emu.gg
 *   3. Extract SquashFS via bundled unsquashfs
 *   4. Configure FEX (Config.json, thunks)
 *   5. Install Vortek Vulkan ICD into rootfs
 *   6. Download/extract Steam into rootfs
 */
class ContainerManager(private val context: Context) {

    companion object {
        private const val TAG = "ContainerManager"

        // FEX x86-64 rootfs download
        private const val FEX_ROOTFS_URL =
            "https://rootfs.fex-emu.gg/Ubuntu_22_04/2025-01-08/Ubuntu_22_04.sqsh"

        // Steam
        private const val STEAM_DEB_URL =
            "https://cdn.cloudflare.steamstatic.com/client/installer/steam.deb"

        // Marker files
        private const val MARKER_FEX_BINARIES = ".fex_binaries_installed"
        private const val MARKER_FEX_ROOTFS = ".fex_rootfs_ready"
        private const val MARKER_STEAM = ".steam_installed"
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    /** Directory for FEX binaries (FEXLoader, bundled glibc) */
    private val fexDir: File
        get() = File(app.getFexDir())

    /** Directory for the extracted x86-64 rootfs */
    private val fexRootfsDir: File
        get() = File(app.getFexRootfsDir())

    /** Home directory for FEX config and user data */
    private val fexHomeDir: File
        get() = File(app.getFexHomeDir())

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(5, TimeUnit.MINUTES)
        .writeTimeout(5, TimeUnit.MINUTES)
        .build()

    private val largeDownloadClient = OkHttpClient.Builder()
        .connectTimeout(60, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.MINUTES)
        .writeTimeout(5, TimeUnit.MINUTES)
        .build()

    // ============================================================
    // Container Status
    // ============================================================

    fun isContainerReady(): Boolean {
        return File(fexDir, MARKER_FEX_BINARIES).exists() &&
                File(fexRootfsDir, "usr/lib/x86_64-linux-gnu/libc.so.6").exists()
    }

    fun isSteamInstalled(): Boolean {
        return File(fexHomeDir, MARKER_STEAM).exists() ||
                File(fexHomeDir, ".local/share/Steam/ubuntu12_32/steam").exists()
    }

    // ============================================================
    // Container Setup
    // ============================================================

    suspend fun setupContainer(
        progressCallback: (progress: Int, message: String) -> Unit
    ) = withContext(Dispatchers.IO) {
        try {
            // Phase 1: Extract FEX binaries (0-10%)
            progressCallback(0, "Extracting FEX binaries...")
            extractFexBinaries()
            progressCallback(10, "FEX binaries installed")

            // Phase 2: Download x86-64 rootfs (10-70%)
            val sqshFile = File(context.cacheDir, "Ubuntu_22_04.sqsh")
            if (fexRootfsDir.exists() &&
                File(fexRootfsDir, "usr/lib/x86_64-linux-gnu/libc.so.6").exists()) {
                progressCallback(70, "x86-64 rootfs already extracted")
            } else {
                if (sqshFile.exists() && sqshFile.length() > 900_000_000) {
                    progressCallback(70, "SquashFS already downloaded")
                } else {
                    progressCallback(11, "Downloading x86-64 rootfs (~995MB)...")
                    downloadFexRootfs(sqshFile, progressCallback)
                    progressCallback(70, "Download complete")
                }

                // Phase 3: Extract SquashFS (70-85%)
                progressCallback(71, "Extracting rootfs (takes several minutes)...")
                extractSquashfs(sqshFile, progressCallback)
                progressCallback(85, "x86-64 rootfs extracted")

                // Clean up sqsh file to save space
                if (sqshFile.exists()) {
                    sqshFile.delete()
                    Log.i(TAG, "Deleted sqsh file to save space")
                }
            }

            // Phase 4: Configure FEX (85-90%)
            progressCallback(86, "Configuring FEX-Emu...")
            writeFexConfig()
            progressCallback(90, "FEX configured")

            // Phase 5: Setup Vortek/Vulkan (90-95%)
            progressCallback(91, "Configuring Vulkan...")
            setupVortek()
            progressCallback(95, "Vulkan configured")

            // Phase 6: Finalize (95-100%)
            progressCallback(96, "Finalizing setup...")
            finalizeSetup()
            progressCallback(100, "Setup complete!")

        } catch (e: Exception) {
            Log.e(TAG, "Container setup failed", e)
            throw ContainerSetupException("Setup failed: ${e.message}", e)
        }
    }

    // ============================================================
    // FEX Binaries
    // ============================================================

    /**
     * Extract FEX binaries from bundled fex-bin.tgz to ${filesDir}/fex/.
     */
    private fun extractFexBinaries() {
        val markerFile = File(fexDir, MARKER_FEX_BINARIES)

        // Skip if already extracted
        if (File(fexDir, "bin/FEXLoader").exists() &&
            File(fexDir, "lib/ld-linux-aarch64.so.1").exists() &&
            markerFile.exists()) {
            Log.i(TAG, "FEX binaries already installed")
            return
        }

        // Stop FEXServer if running — prevents ETXTBSY when overwriting binaries
        try {
            app.fexExecutor.stopFexServer()
        } catch (e: Exception) {
            Log.w(TAG, "Could not stop FEXServer: ${e.message}")
        }

        fexDir.mkdirs()

        val rawStream = openAssetWithFallback("fex-bin.tgz", "fex-bin.tar", "fex-bin.tar.gz")
            ?: throw ContainerSetupException("FEX binary archive not found in assets")

        val tarStream = try {
            TarArchiveInputStream(GZIPInputStream(rawStream))
        } catch (e: Exception) {
            rawStream.close()
            val retryStream = openAssetWithFallback("fex-bin.tgz", "fex-bin.tar", "fex-bin.tar.gz")
                ?: throw ContainerSetupException("FEX binary archive not found (retry)")
            TarArchiveInputStream(retryStream)
        }

        tarStream.use { tar ->
            var entry = tar.nextTarEntry
            var fileCount = 0
            while (entry != null) {
                val destFile = File(fexDir, entry.name)
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
                            Log.w(TAG, "Symlink failed: ${entry.name}")
                        }
                    }
                    else -> {
                        destFile.parentFile?.mkdirs()
                        try {
                            FileOutputStream(destFile).use { output ->
                                tar.copyTo(output)
                            }
                            destFile.setExecutable(true)
                            destFile.setReadable(true, false)
                            fileCount++
                        } catch (e: FileNotFoundException) {
                            if (e.message?.contains("ETXTBSY") == true && destFile.exists()) {
                                // File is currently being executed (e.g. FEXServer running)
                                // Skip — the existing file is fine
                                Log.w(TAG, "Skipping busy file: ${entry.name}")
                                // Still need to consume the tar entry data
                            } else {
                                throw e
                            }
                        }
                    }
                }
                entry = tar.nextTarEntry
            }
            Log.i(TAG, "Extracted $fileCount FEX files to $fexDir")
        }

        markerFile.createNewFile()
    }

    private fun openAssetWithFallback(vararg names: String): InputStream? {
        for (name in names) {
            try {
                return context.assets.open(name)
            } catch (e: IOException) {
                continue
            }
        }
        return null
    }

    // ============================================================
    // FEX Rootfs Download & Extraction
    // ============================================================

    /**
     * Download FEX x86-64 SquashFS rootfs with resume support.
     */
    private suspend fun downloadFexRootfs(
        sqshFile: File,
        progressCallback: (Int, String) -> Unit
    ) = withContext(Dispatchers.IO) {
        sqshFile.parentFile?.mkdirs()

        val requestBuilder = Request.Builder().url(FEX_ROOTFS_URL)
        var startByte = 0L

        if (sqshFile.exists() && sqshFile.length() > 0) {
            startByte = sqshFile.length()
            requestBuilder.header("Range", "bytes=$startByte-")
            Log.i(TAG, "Resuming FEX rootfs download from byte $startByte")
        }

        val response = largeDownloadClient.newCall(requestBuilder.build()).execute()
        try {
            if (!response.isSuccessful && response.code != 206) {
                throw ContainerSetupException("Download failed: HTTP ${response.code}")
            }

            val body = response.body ?: throw ContainerSetupException("Empty response body")
            val contentLength = body.contentLength()
            val totalSize = if (response.code == 206) startByte + contentLength else contentLength
            var bytesWritten = startByte
            val append = response.code == 206

            FileOutputStream(sqshFile, append).use { output ->
                body.byteStream().use { input ->
                    val buffer = ByteArray(65536)
                    var read: Int
                    while (input.read(buffer).also { read = it } != -1) {
                        output.write(buffer, 0, read)
                        bytesWritten += read
                        if (totalSize > 0) {
                            val pct = (bytesWritten.toFloat() / totalSize * 100).toInt()
                            val mapped = 11 + (pct * 59 / 100) // Map 0-100% to 11-70
                            progressCallback(mapped,
                                "Downloading: ${bytesWritten / 1024 / 1024}MB / ${totalSize / 1024 / 1024}MB")
                        }
                    }
                }
            }

            if (sqshFile.length() < 100_000_000) {
                sqshFile.delete()
                throw ContainerSetupException("Download incomplete: only ${sqshFile.length()} bytes")
            }
            Log.i(TAG, "FEX rootfs downloaded: ${sqshFile.length() / 1024 / 1024}MB")
        } finally {
            response.close()
        }
    }

    /**
     * Extract SquashFS rootfs using the bundled unsquashfs binary.
     * Runs directly on Android via ProcessBuilder (no PRoot needed).
     */
    private suspend fun extractSquashfs(
        sqshFile: File,
        progressCallback: (Int, String) -> Unit
    ) = withContext(Dispatchers.IO) {
        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        val unsquashfsBin = File(nativeLibDir, "libunsquashfs.so")

        if (!unsquashfsBin.exists()) {
            throw ContainerSetupException(
                "unsquashfs binary not found at ${unsquashfsBin.absolutePath}. " +
                "Ensure squashfs-tools is compiled in CMakeLists.txt"
            )
        }

        fexRootfsDir.parentFile?.mkdirs()

        val process = ProcessBuilder(
            unsquashfsBin.absolutePath,
            "-d", fexRootfsDir.absolutePath,
            "-f",
            sqshFile.absolutePath
        ).apply {
            redirectErrorStream(true)
        }.start()

        val output = StringBuilder()
        try {
            val reader = process.inputStream.bufferedReader()
            val buffer = CharArray(4096)
            while (true) {
                val count = reader.read(buffer)
                if (count == -1) break
                val text = String(buffer, 0, count)
                output.append(text)

                // Parse progress from unsquashfs output
                val pctMatch = Regex("""(\d+)%""").findAll(text).lastOrNull()
                pctMatch?.let {
                    val pct = it.groupValues[1].toIntOrNull() ?: 0
                    val mapped = 71 + (pct * 14 / 100) // Map 0-100% to 71-85
                    progressCallback(mapped, "Extracting rootfs: $pct%")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Read error during extraction: ${e.message}")
        }

        val completed = process.waitFor(15, TimeUnit.MINUTES)
        if (!completed) {
            process.destroyForcibly()
            throw ContainerSetupException("unsquashfs timed out after 15 minutes")
        }

        val exitCode = process.exitValue()
        if (exitCode != 0) {
            throw ContainerSetupException(
                "unsquashfs failed (exit $exitCode): ${output.toString().takeLast(500)}"
            )
        }

        // Verify extraction
        val verifyFile = File(fexRootfsDir, "usr/lib/x86_64-linux-gnu/libc.so.6")
        if (!verifyFile.exists()) {
            throw ContainerSetupException("Rootfs extraction failed: libc.so.6 not found")
        }
        Log.i(TAG, "SquashFS rootfs extracted successfully to $fexRootfsDir")
    }

    // ============================================================
    // FEX Configuration
    // ============================================================

    /**
     * Write FEX config files (Config.json, thunks.json).
     */
    private fun writeFexConfig() {
        val configDir = File(fexHomeDir, ".fex-emu")
        configDir.mkdirs()

        // Point RootFS to the extracted rootfs directory
        // FEX looks in ~/.fex-emu/RootFS/<name>/ for the rootfs
        val rootfsSymlinkDir = File(configDir, "RootFS")
        rootfsSymlinkDir.mkdirs()

        // Create symlink: ~/.fex-emu/RootFS/Ubuntu_22_04 → actual rootfs location
        val symlink = File(rootfsSymlinkDir, SteamLauncherApp.FEX_ROOTFS_NAME)
        if (!symlink.exists()) {
            try {
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", fexRootfsDir.absolutePath, symlink.absolutePath)
                ).waitFor()
            } catch (e: Exception) {
                Log.w(TAG, "Failed to create rootfs symlink, copying path instead")
            }
        }

        File(configDir, "Config.json").writeText("""
{
  "Config": {
    "RootFS": "${SteamLauncherApp.FEX_ROOTFS_NAME}",
    "ThunkConfig": "${configDir.absolutePath}/thunks.json",
    "X87ReducedPrecision": "1"
  }
}
        """.trimIndent())

        // Thunks config for GPU passthrough
        val bundledThunks = File(fexDir, "config/thunks.json")
        val thunksFile = File(configDir, "thunks.json")
        if (bundledThunks.exists()) {
            bundledThunks.copyTo(thunksFile, overwrite = true)
        } else if (!thunksFile.exists()) {
            thunksFile.writeText("""{"ThunksDB": {"GL": 1, "Vulkan": 1}}""")
        }

        Log.i(TAG, "FEX config written to $configDir")
    }

    // ============================================================
    // Vortek / Vulkan Setup
    // ============================================================

    /**
     * Setup Vortek Vulkan passthrough in the FEX rootfs.
     */
    private fun setupVortek() {
        // Write Vortek ICD JSON into FEX rootfs
        VulkanBridge.setupVortekIcd(fexRootfsDir.absolutePath)

        // Install Vortek client library into FEX rootfs
        val libDir = File(fexRootfsDir, "lib")
        libDir.mkdirs()
        installVortekClient(libDir)

        // Install Vulkan test tools
        installVulkanTools()

        Log.i(TAG, "Vortek passthrough configured in FEX rootfs")
    }

    /**
     * Install the Vortek client library (libvulkan_vortek.so).
     */
    private fun installVortekClient(targetDir: File) {
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
            Log.i(TAG, "Vortek client library installed: ${targetFile.length()} bytes")
            return
        } catch (e: IOException) {
            Log.d(TAG, "Vortek client not in assets: ${e.message}")
        }

        // Try native libs directory
        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        val sourceFile = File(nativeLibDir, assetName)
        if (sourceFile.exists()) {
            sourceFile.copyTo(targetFile, overwrite = true)
            targetFile.setExecutable(true)
            targetFile.setReadable(true, false)
            Log.i(TAG, "Vortek client library installed from native libs")
            return
        }

        Log.w(TAG, "Vortek client library not found")
    }

    /**
     * Install Vulkan test tools (vkcube, vulkaninfo) into FEX rootfs.
     */
    private fun installVulkanTools() {
        val binDir = File(fexRootfsDir, "usr/local/bin")
        binDir.mkdirs()

        for (tool in listOf("vkcube", "vulkaninfo")) {
            try {
                context.assets.open(tool).use { input ->
                    val targetFile = File(binDir, tool)
                    FileOutputStream(targetFile).use { output ->
                        input.copyTo(output)
                    }
                    targetFile.setExecutable(true)
                    targetFile.setReadable(true, false)
                    Log.i(TAG, "$tool installed: ${targetFile.length()} bytes")
                }
            } catch (e: IOException) {
                Log.d(TAG, "$tool not in assets: ${e.message}")
            }
        }
    }

    // ============================================================
    // Finalization
    // ============================================================

    private fun finalizeSetup() {
        // Create user home directories in fex-home
        fexHomeDir.mkdirs()
        File(fexHomeDir, ".local/share").mkdirs()
        File(fexHomeDir, ".steam").mkdirs()

        // Create /tmp directories in rootfs for socket symlinks
        val rootfsTmp = File(fexRootfsDir, "tmp")
        rootfsTmp.mkdirs()
        File(rootfsTmp, ".X11-unix").mkdirs()
        File(rootfsTmp, ".vortek").mkdirs()
        File(rootfsTmp, "shm").mkdirs()

        // Ensure FEX binaries have execute permission
        File(fexDir, "bin").listFiles()?.forEach { it.setExecutable(true) }
        File(fexDir, "lib").listFiles()?.forEach {
            if (it.name.endsWith(".so") || it.name.contains(".so.")) {
                it.setExecutable(true)
            }
        }

        Log.i(TAG, "Container setup finalized")
    }

    // ============================================================
    // Steam Installation
    // ============================================================

    /**
     * Download and extract Steam .deb file.
     * Steam files go into the FEX rootfs so they're accessible via FEX's overlay.
     */
    suspend fun downloadAndExtractSteam(): String = withContext(Dispatchers.IO) {
        val result = StringBuilder()
        val debFile = File(context.cacheDir, "steam.deb")

        // Steam gets installed into the FEX rootfs so x86-64 binaries are available
        val steamInstallDir = fexRootfsDir

        try {
            // Download .deb if needed
            if (!debFile.exists() || debFile.length() < 1000000) {
                result.appendLine("Downloading Steam...")
                val request = Request.Builder().url(STEAM_DEB_URL).build()
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
                                    val destFile = File(steamInstallDir, tarEntry.name)

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
            val steamBin = File(steamInstallDir, "usr/bin/steam")
            val steamLib = File(steamInstallDir, "usr/lib/steam")
            result.appendLine("")
            result.appendLine("Steam binary exists: ${steamBin.exists()}")
            result.appendLine("Steam lib exists: ${steamLib.exists()}")

            if (steamBin.exists()) {
                steamBin.setExecutable(true)
            }

            // Extract Steam bootstrap
            val bootstrapFile = File(steamLib, "bootstraplinux_ubuntu12_32.tar.xz")
            if (bootstrapFile.exists()) {
                result.appendLine("")
                result.appendLine("Extracting Steam bootstrap...")
                val steamDataDir = File(fexHomeDir, ".local/share/Steam")
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
                    val dotSteam = File(fexHomeDir, ".steam")
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

            File(fexHomeDir, MARKER_STEAM).createNewFile()

            result.toString()
        } catch (e: Exception) {
            Log.e(TAG, "Steam extraction failed", e)
            result.appendLine("ERROR: ${e.message}")
            result.toString()
        }
    }

    // ============================================================
    // Utilities
    // ============================================================

    /**
     * Run a command inside the FEX environment and return output.
     */
    suspend fun runInContainer(command: String): String = withContext(Dispatchers.IO) {
        val result = StringBuilder()
        try {
            val process = app.fexExecutor.execute(
                command = command,
                environment = emptyMap()
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
        fexDir.deleteRecursively()
        fexRootfsDir.parentFile?.deleteRecursively()
        fexHomeDir.deleteRecursively()
    }
}

class ContainerSetupException(message: String, cause: Throwable? = null) : Exception(message, cause)
