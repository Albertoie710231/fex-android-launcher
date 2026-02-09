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
     * Write FEX config files (Config.json, thunks.json, ThunksDB.json).
     *
     * Thunk configuration requires:
     * - Config.json: ThunkHostLibs (ARM64 host thunks), ThunkGuestLibs (x86-64 guest thunks)
     * - thunks.json: Which thunks to enable (GL, Vulkan)
     * - ThunksDB.json: Database mapping library names to overlay paths
     */
    private fun writeFexConfig() {
        val configDir = File(fexHomeDir, ".fex-emu")
        configDir.mkdirs()

        val nativeLibDir = context.applicationInfo.nativeLibraryDir

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

        // ThunkHostLibs: ARM64 host thunk .so files (in nativeLibDir for SELinux exec)
        // ThunkGuestLibs: x86-64 guest thunk .so files (HOST filesystem path, not rootfs-relative)
        // FEX checks FHU::Filesystem::Exists() on HOST, so must be absolute HOST path
        val thunkGuestLibsPath = "${fexRootfsDir.absolutePath}/opt/fex/share/fex-emu/GuestThunks"

        File(configDir, "Config.json").writeText("""
{
  "Config": {
    "RootFS": "${SteamLauncherApp.FEX_ROOTFS_NAME}",
    "ThunkConfig": "${configDir.absolutePath}/thunks.json",
    "ThunkHostLibs": "$nativeLibDir",
    "ThunkGuestLibs": "$thunkGuestLibsPath",
    "X87ReducedPrecision": "1"
  }
}
        """.trimIndent())

        // Thunks config: enable Vulkan passthrough, disable GL (no host GL available)
        File(configDir, "thunks.json").writeText("""{"ThunksDB": {"GL": 0, "Vulkan": 1}}""")

        // Deploy ThunksDB.json — FEX's LoadThunkDatabase() looks for it at
        // $HOME/.fex-emu/ThunksDB.json on the HOST filesystem
        deployThunksDB(configDir)

        Log.i(TAG, "FEX config written to $configDir")
    }

    /**
     * Deploy ThunksDB.json from assets to the FEX config directory.
     * This database maps library names (e.g. "Vulkan") to overlay paths
     * that FEX intercepts with guest thunk libraries.
     */
    private fun deployThunksDB(configDir: File) {
        val targetFile = File(configDir, "ThunksDB.json")
        try {
            context.assets.open("ThunksDB.json").use { input ->
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
            }
            Log.i(TAG, "ThunksDB.json deployed to ${targetFile.absolutePath}")
        } catch (e: IOException) {
            Log.w(TAG, "ThunksDB.json not in assets, checking fex dir")
            // Fallback: copy from extracted FEX binaries
            val fallback = File(fexDir, "share/fex-emu/ThunksDB.json")
            if (fallback.exists()) {
                fallback.copyTo(targetFile, overwrite = true)
                Log.i(TAG, "ThunksDB.json deployed from fex dir")
            } else {
                Log.e(TAG, "ThunksDB.json not found anywhere!")
            }
        }
    }

    // ============================================================
    // Vortek / Vulkan Setup
    // ============================================================

    /**
     * Setup Vortek Vulkan passthrough — both guest-side (rootfs) and host-side (ICD bridge).
     *
     * The Vulkan thunk chain works like this:
     *   1. Guest x86-64 app calls libvulkan.so.1 → intercepted by FEX thunk overlay
     *   2. Guest thunk (libvulkan-guest.so, x86-64) forwards to host thunk
     *   3. Host thunk (libvulkan-host.so, ARM64) calls dlopen("libvulkan.so.1")
     *   4. ARM64 Vulkan ICD loader (libvulkan_loader.so) reads VK_ICD_FILENAMES
     *   5. ICD JSON points to libvulkan_vortek.so (Vortek client, ARM64 glibc)
     *   6. Vortek client → Unix socket → VortekRenderer → Mali GPU
     */
    private fun setupVortek() {
        // Guest-side: Write Vortek ICD JSON into FEX rootfs (for non-thunk fallback)
        VulkanBridge.setupVortekIcd(fexRootfsDir.absolutePath)

        // Guest-side: Install Vortek client library into FEX rootfs (for non-thunk fallback)
        val libDir = File(fexRootfsDir, "lib")
        libDir.mkdirs()
        installVortekClient(libDir)

        // Host-side: Setup Vulkan ICD bridge for thunks
        setupVortekHostBridge()

        // Install Vulkan test tools
        installVulkanTools()

        Log.i(TAG, "Vortek passthrough configured (guest + host)")
    }

    /**
     * Setup the host-side Vulkan ICD bridge for FEX thunks.
     *
     * When the host thunk (libvulkan-host.so) calls dlopen("libvulkan.so.1"),
     * it needs to find an ARM64 glibc-linked Vulkan ICD loader. We create:
     *   1. Symlink: $fexDir/lib/libvulkan.so.1 → $nativeLibDir/libvulkan_loader.so
     *   2. Host ICD JSON pointing to $nativeLibDir/libvulkan_vortek.so
     *
     * The symlink goes in $fexDir/lib/ which is already in LD_LIBRARY_PATH.
     * The ICD JSON is referenced by VK_ICD_FILENAMES env var (set in FexExecutor).
     */
    private fun setupVortekHostBridge() {
        val nativeLibDir = context.applicationInfo.nativeLibraryDir
        val configDir = File(fexHomeDir, ".fex-emu")
        configDir.mkdirs()

        // 1. Create symlink: $fexDir/lib/libvulkan.so.1 → nativeLibDir/libvulkan_loader.so
        //    This is what the host thunk's dlopen("libvulkan.so.1") will find
        val fexLibDir = File(fexDir, "lib")
        fexLibDir.mkdirs()
        val vulkanSymlink = File(fexLibDir, "libvulkan.so.1")
        val vulkanLoaderSrc = File(nativeLibDir, "libvulkan_loader.so")

        if (vulkanLoaderSrc.exists()) {
            try {
                vulkanSymlink.delete()
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", vulkanLoaderSrc.absolutePath, vulkanSymlink.absolutePath)
                ).waitFor()
                Log.i(TAG, "Host Vulkan ICD loader symlink: ${vulkanSymlink.absolutePath} → ${vulkanLoaderSrc.absolutePath}")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to create Vulkan loader symlink", e)
            }
        } else {
            Log.w(TAG, "libvulkan_loader.so not found in nativeLibDir")
        }

        // 2. Create host ICD JSON pointing to Vortek ICD wrapper in nativeLibDir
        //    The Vulkan ICD loader reads VK_ICD_FILENAMES to find this JSON,
        //    then loads the library_path from it.
        //    We use libvortek_icd_wrapper.so (not libvulkan_vortek.so directly) because
        //    the Vortek library's vk_icdGetInstanceProcAddr returns NULL without
        //    calling vortekInitOnce first. The wrapper handles this initialization.
        val wrapperLibPath = "$nativeLibDir/libvortek_icd_wrapper.so"
        val hostIcdJson = File(configDir, "vortek_host_icd.json")
        hostIcdJson.writeText("""{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$wrapperLibPath",
        "api_version": "1.1.128"
    }
}""")
        Log.i(TAG, "Host Vortek ICD JSON: ${hostIcdJson.absolutePath} → $wrapperLibPath")
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
     * Clean up any stale ARM64 Vulkan tools that shadow the x86-64 rootfs versions.
     * The rootfs already has x86-64 vulkaninfo/vkcube at /usr/bin/ from Ubuntu packages.
     */
    private fun installVulkanTools() {
        val binDir = File(fexRootfsDir, "usr/local/bin")
        for (tool in listOf("vkcube", "vulkaninfo")) {
            val stale = File(binDir, tool)
            if (stale.exists()) {
                stale.delete()
                Log.i(TAG, "Removed stale ARM64 $tool from /usr/local/bin (rootfs has x86-64 version)")
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
