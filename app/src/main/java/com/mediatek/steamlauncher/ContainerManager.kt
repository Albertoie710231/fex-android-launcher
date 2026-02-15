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
    // Path Refresh (after APK reinstall)
    // ============================================================

    /**
     * Update Config.json, libvulkan.so.1 symlink, and vortek_host_icd.json
     * with the current nativeLibDir. Must be called on every activity launch
     * because nativeLibDir changes with each APK install.
     */
    fun refreshNativeLibPaths() {
        val configFile = File(fexHomeDir, ".fex-emu/Config.json")
        if (!configFile.exists()) return  // setup hasn't run yet

        val nativeLibDir = context.applicationInfo.nativeLibraryDir

        // 1. Update Config.json — ThunkHostLibs points to fexDir (not nativeLibDir)
        //    so both 64-bit and _32 directories are accessible
        val configDir = File(fexHomeDir, ".fex-emu")
        val thunkHostLibsPath = "${fexDir.absolutePath}/lib/fex-emu/HostThunks"
        val thunkGuestLibsPath = "${fexRootfsDir.absolutePath}/opt/fex/share/fex-emu/GuestThunks"
        configFile.writeText("""{
  "Config": {
    "RootFS": "${SteamLauncherApp.FEX_ROOTFS_NAME}",
    "ThunkConfig": "${configDir.absolutePath}/thunks.json",
    "ThunkHostLibs": "$thunkHostLibsPath",
    "ThunkGuestLibs": "$thunkGuestLibsPath",
    "X87ReducedPrecision": "1"
  }
}""")

        // 2. Refresh 64-bit host thunks from nativeLibDir (may have changed after APK install)
        val hostDst64 = File(fexDir, "lib/fex-emu/HostThunks")
        hostDst64.mkdirs()
        File(nativeLibDir).listFiles()?.filter { it.name.endsWith("-host.so") }?.forEach { src ->
            val dst = File(hostDst64, src.name)
            if (!dst.exists() || dst.length() != src.length()) {
                src.copyTo(dst, overwrite = true)
                dst.setExecutable(true)
                dst.setReadable(true, false)
            }
        }

        // 3. Update libvulkan.so.1 symlink → nativeLibDir/libvulkan_loader.so
        val vulkanSymlink = File(fexDir, "lib/libvulkan.so.1")
        val vulkanLoaderSrc = File(nativeLibDir, "libvulkan_loader.so")
        if (vulkanLoaderSrc.exists()) {
            try {
                vulkanSymlink.delete()
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", vulkanLoaderSrc.absolutePath, vulkanSymlink.absolutePath)
                ).waitFor()
            } catch (e: Exception) {
                Log.e(TAG, "Failed to update Vulkan loader symlink", e)
            }
        }

        // 4. Update vortek_host_icd.json → nativeLibDir/libvortek_icd_wrapper.so
        val hostIcdJson = File(configDir, "vortek_host_icd.json")
        val wrapperLibPath = "$nativeLibDir/libvortek_icd_wrapper.so"
        hostIcdJson.writeText("""{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$wrapperLibPath",
        "api_version": "1.1.128"
    }
}""")

        Log.i(TAG, "Refreshed nativeLibDir paths: $nativeLibDir")

        // Deploy stub DLLs and fix Mesa/GLVND on every launch
        // (these are idempotent and fast — ensures rootfs is always patched)
        if (fexRootfsDir.exists()) {
            deployStubDlls()
            fixupMesaAndGlvnd()
        }
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

        // ThunkHostLibs: ARM64 host thunk .so files
        //   Points to fexDir/lib/fex-emu/HostThunks/ — FEX appends "_32" for 32-bit guest.
        //   64-bit host thunks are copied from nativeLibDir by deployThunks().
        //   32-bit host thunks come from fex-bin.tgz at HostThunks_32/.
        // ThunkGuestLibs: x86/x86-64 guest thunk .so files (HOST filesystem path)
        //   Points to rootfs/opt/fex/share/fex-emu/GuestThunks/ — FEX appends "_32" for 32-bit.
        //   Deployed by deployThunks() from fexDir/share/fex-emu/.
        val thunkHostLibsPath = "${fexDir.absolutePath}/lib/fex-emu/HostThunks"
        val thunkGuestLibsPath = "${fexRootfsDir.absolutePath}/opt/fex/share/fex-emu/GuestThunks"

        File(configDir, "Config.json").writeText("""
{
  "Config": {
    "RootFS": "${SteamLauncherApp.FEX_ROOTFS_NAME}",
    "ThunkConfig": "${configDir.absolutePath}/thunks.json",
    "ThunkHostLibs": "$thunkHostLibsPath",
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

        // Guest-side: Install headless Vulkan wrapper (x86-64) for vkcube frame capture
        installHeadlessWrapper()

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

    /**
     * Install the x86-64 headless Vulkan wrapper into the rootfs.
     * This provides fake XCB + headless surface support via LD_PRELOAD,
     * enabling vkcube to render without an X11 server and capture frames via TCP.
     */
    private fun installHeadlessWrapper() {
        val targetFile = File(fexRootfsDir, "usr/lib/libvulkan_headless.so")
        try {
            context.assets.open("libvulkan_headless.so").use { input ->
                targetFile.parentFile?.mkdirs()
                FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
            }
            targetFile.setExecutable(true)
            targetFile.setReadable(true, false)
            Log.i(TAG, "Headless Vulkan wrapper installed: ${targetFile.absolutePath}")
        } catch (e: IOException) {
            Log.w(TAG, "Headless Vulkan wrapper not found in assets: ${e.message}")
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
        File(rootfsTmp, "dumps").mkdirs()

        // Create /dev/shm in rootfs for POSIX named semaphores (sem_open)
        // glibc's sem_open creates files in /dev/shm/sem.NAME
        val devShm = File(fexRootfsDir, "dev/shm")
        devShm.mkdirs()

        // Create /etc/resolv.conf for DNS resolution inside FEX guest
        val resolvConf = File(fexRootfsDir, "etc/resolv.conf")
        if (!resolvConf.exists() || (resolvConf.exists() && resolvConf.readText().isBlank())) {
            resolvConf.parentFile?.mkdirs()
            resolvConf.writeText("nameserver 8.8.8.8\nnameserver 8.8.4.4\n")
            Log.i(TAG, "Created resolv.conf with Google DNS")
        }

        // Allow apt to work with potentially stale GPG keys in the rootfs
        val aptInsecure = File(fexRootfsDir, "etc/apt/apt.conf.d/99allow-insecure")
        if (!aptInsecure.exists()) {
            aptInsecure.parentFile?.mkdirs()
            aptInsecure.writeText("Acquire::AllowInsecureRepositories \"true\";\nAcquire::AllowDowngradeToInsecureRepositories \"true\";\n")
            Log.i(TAG, "Created apt insecure repositories config")
        }

        // Deploy Proton setup scripts into rootfs
        deployProtonScripts()

        // Deploy stub DLLs to rootfs (d3dcompiler_47, Galaxy64, GFSDK_SSAO)
        deployStubDlls()

        // Remove Mesa GLX (SIGILL from LLVM AVX2) and install GLVND dispatchers
        fixupMesaAndGlvnd()

        // Deploy thunks (guest + host) to their expected locations
        deployThunks()

        // Ensure FEX binaries have execute permission
        File(fexDir, "bin").listFiles()?.forEach { it.setExecutable(true) }
        File(fexDir, "lib").listFiles()?.forEach {
            if (it.name.endsWith(".so") || it.name.contains(".so.")) {
                it.setExecutable(true)
            }
        }

        Log.i(TAG, "Container setup finalized")
    }

    /**
     * Deploy Proton setup and launch scripts into the rootfs at /opt/scripts/.
     * These are bundled as assets and run inside the FEX guest.
     */
    private fun deployProtonScripts() {
        val scriptsDir = File(fexRootfsDir, "opt/scripts")
        scriptsDir.mkdirs()

        for (scriptName in listOf("setup_proton.sh", "launch_wine.sh")) {
            val targetFile = File(scriptsDir, scriptName)
            try {
                context.assets.open(scriptName).use { input ->
                    FileOutputStream(targetFile).use { output ->
                        input.copyTo(output)
                    }
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "Deployed $scriptName to ${targetFile.absolutePath}")
            } catch (e: IOException) {
                Log.d(TAG, "$scriptName not in assets, skipping")
            }
        }

        // Create games directory
        val gamesDir = File(fexRootfsDir, "home/user/games")
        gamesDir.mkdirs()
    }

    /**
     * Deploy stub DLLs from assets into the rootfs.
     *
     * - d3dcompiler_47_stub.dll: replaces Wine's d3dcompiler_47 (which depends on
     *   wined3d.so and causes SIGILL). Goes into Wine's native DLL path so
     *   WINEDLLOVERRIDES=d3dcompiler_47=n picks it up.
     * - Galaxy64.dll, GFSDK_SSAO: game-specific stubs copied to /opt/stubs/ for
     *   deployment to game directories at launch time.
     */
    private fun deployStubDlls() {
        // d3dcompiler_47 stub → Wine's native DLL search path
        val wineWin64Dir = File(fexRootfsDir, "opt/proton-ge/files/lib/wine/x86_64-windows")
        wineWin64Dir.mkdirs()
        copyAssetToFile("d3dcompiler_47_stub.dll", File(wineWin64Dir, "d3dcompiler_47.dll"))

        // All stubs → /opt/stubs/ for game-directory deployment
        val stubsDir = File(fexRootfsDir, "opt/stubs")
        stubsDir.mkdirs()
        copyAssetToFile("d3dcompiler_47_stub.dll", File(stubsDir, "d3dcompiler_47.dll"))
        copyAssetToFile("Galaxy64.dll", File(stubsDir, "Galaxy64.dll"))
        copyAssetToFile("GFSDK_SSAO_D3D11.win64.dll", File(stubsDir, "GFSDK_SSAO_D3D11.win64.dll"))

        Log.i(TAG, "Stub DLLs deployed to Wine path and /opt/stubs/")
    }

    /**
     * Remove Mesa GLX backend and DRI drivers (contain LLVM with AVX2 → SIGILL),
     * then install GLVND dispatchers that safely return "no GL vendor".
     *
     * Chain that causes SIGILL:
     *   Wine opengl32.so → dlopen("libGL.so.1") → libGLX.so.0 → libGLX_mesa.so
     *   → DRI drivers → LLVM → AVX2 instructions → SIGILL on ARM64/FEX
     *
     * GLVND dispatchers provide libGL/libGLX API without Mesa backend.
     */
    private fun fixupMesaAndGlvnd() {
        val libDir = File(fexRootfsDir, "usr/lib/x86_64-linux-gnu")
        if (!libDir.exists()) return

        // Remove libGLX_mesa.so* (Mesa GLX backend with LLVM AVX2)
        libDir.listFiles()?.filter { it.name.startsWith("libGLX_mesa.so") }?.forEach { file ->
            file.delete()
            Log.i(TAG, "Removed ${file.name} (Mesa GLX, SIGILL source)")
        }

        // Remove DRI drivers directory (contains LLVM with AVX2)
        val driDir = File(libDir, "dri")
        if (driDir.exists() && driDir.isDirectory) {
            driDir.deleteRecursively()
            Log.i(TAG, "Removed dri/ directory (DRI drivers with LLVM AVX2)")
        }

        // Install GLVND dispatchers from assets
        copyAssetToFile("glvnd_libGL.so.1", File(libDir, "libGL.so.1"))
        copyAssetToFile("glvnd_libGLX.so.0", File(libDir, "libGLX.so.0"))
        copyAssetToFile("glvnd_libGLdispatch.so.0", File(libDir, "libGLdispatch.so.0"))

        // Ensure libGL.so symlink exists → libGL.so.1
        val glSymlink = File(libDir, "libGL.so")
        if (!glSymlink.exists() || glSymlink.isFile) {
            try {
                glSymlink.delete()
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", "libGL.so.1", glSymlink.absolutePath)
                ).waitFor()
            } catch (e: Exception) {
                Log.w(TAG, "Failed to create libGL.so symlink", e)
            }
        }

        Log.i(TAG, "Mesa GLX removed, GLVND dispatchers installed")
    }

    /** Copy an asset file to a target location, overwriting if exists. */
    private fun copyAssetToFile(assetName: String, target: File) {
        try {
            context.assets.open(assetName).use { input ->
                target.parentFile?.mkdirs()
                FileOutputStream(target).use { output ->
                    input.copyTo(output)
                }
            }
            target.setReadable(true, false)
            target.setExecutable(true)
        } catch (e: IOException) {
            Log.w(TAG, "Asset $assetName not found, skipping: ${e.message}")
        }
    }

    // Deploy FEX thunk libraries to their expected locations.
    //
    // Guest thunks (x86/x86-64, interpreted by FEX):
    //   Source: fexDir/share/fex-emu/GuestThunks and GuestThunks_32 (from fex-bin.tgz)
    //   Target: fexRootfsDir/opt/fex/share/fex-emu/GuestThunks and GuestThunks_32
    //   Config: ThunkGuestLibs points to target path (FEX appends _32 for 32-bit)
    //
    // Host thunks (ARM64, loaded via dlopen by FEX host process):
    //   Source 64-bit: nativeLibDir (jniLibs) — files matching *-host.so
    //   Source 32-bit: fexDir/lib/fex-emu/HostThunks_32 (from fex-bin.tgz)
    //   Target: fexDir/lib/fex-emu/HostThunks and HostThunks_32
    //   Config: ThunkHostLibs points to HostThunks (FEX appends _32 for 32-bit)
    private fun deployThunks() {
        val nativeLibDir = context.applicationInfo.nativeLibraryDir

        // --- Guest thunks: copy from fexDir to rootfs ---
        val guestSrc64 = File(fexDir, "share/fex-emu/GuestThunks")
        val guestSrc32 = File(fexDir, "share/fex-emu/GuestThunks_32")
        val guestDst64 = File(fexRootfsDir, "opt/fex/share/fex-emu/GuestThunks")
        val guestDst32 = File(fexRootfsDir, "opt/fex/share/fex-emu/GuestThunks_32")

        copyThunkDir(guestSrc64, guestDst64, "64-bit guest")
        copyThunkDir(guestSrc32, guestDst32, "32-bit guest")

        // --- Host thunks: ensure both 64-bit and 32-bit are in fexDir ---
        // 64-bit host thunks: copy from nativeLibDir (jniLibs) to fexDir/lib/fex-emu/HostThunks/
        val hostDst64 = File(fexDir, "lib/fex-emu/HostThunks")
        hostDst64.mkdirs()
        File(nativeLibDir).listFiles()?.filter { it.name.endsWith("-host.so") }?.forEach { src ->
            val dst = File(hostDst64, src.name)
            if (!dst.exists() || dst.length() != src.length()) {
                src.copyTo(dst, overwrite = true)
                dst.setExecutable(true)
                dst.setReadable(true, false)
                Log.i(TAG, "Deployed 64-bit host thunk: ${src.name}")
            }
        }

        // 32-bit host thunks: already at fexDir/lib/fex-emu/HostThunks_32/ from fex-bin.tgz
        val hostDir32 = File(fexDir, "lib/fex-emu/HostThunks_32")
        if (hostDir32.exists()) {
            hostDir32.listFiles()?.filter { it.name.endsWith(".so") }?.forEach { it.setExecutable(true) }
            val count = hostDir32.listFiles()?.size ?: 0
            Log.i(TAG, "32-bit host thunks ready: $count files in ${hostDir32.absolutePath}")
        }
    }

    /** Copy all .so files from src to dst directory */
    private fun copyThunkDir(src: File, dst: File, label: String) {
        if (!src.exists() || !src.isDirectory) {
            Log.w(TAG, "No $label thunks at ${src.absolutePath}")
            return
        }
        dst.mkdirs()
        var count = 0
        src.listFiles()?.filter { it.name.endsWith(".so") }?.forEach { srcFile ->
            val dstFile = File(dst, srcFile.name)
            if (!dstFile.exists() || dstFile.length() != srcFile.length()) {
                srcFile.copyTo(dstFile, overwrite = true)
                dstFile.setReadable(true, false)
                count++
            }
        }
        Log.i(TAG, "Deployed $count $label thunks to ${dst.absolutePath}")
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
