package com.mediatek.steamlauncher

import android.content.Context
import android.content.pm.FeatureInfo
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import java.io.File

/**
 * Manages Vulkan passthrough configuration for the Linux container.
 * Handles GPU detection, driver setup, and Vulkan ICD configuration
 * for Mali GPUs (G710, G720) found in MediaTek Dimensity SoCs.
 */
object VulkanBridge {

    private const val TAG = "VulkanBridge"

    // Mali GPU feature flags
    private const val FEATURE_VULKAN_HARDWARE_LEVEL = "android.hardware.vulkan.level"
    private const val FEATURE_VULKAN_HARDWARE_VERSION = "android.hardware.vulkan.version"
    private const val FEATURE_VULKAN_COMPUTE = "android.hardware.vulkan.compute"

    data class VulkanInfo(
        val isSupported: Boolean,
        val apiVersion: String,
        val driverVersion: String,
        val deviceName: String,
        val vendorName: String,
        val features: List<String>
    )

    data class GpuInfo(
        val name: String,
        val vendor: String,
        val renderer: String,
        val glVersion: String,
        val vulkanVersion: String
    )

    /**
     * Get Vulkan support information from the device.
     */
    fun getVulkanInfo(context: Context): String {
        val pm = context.packageManager
        val features = pm.systemAvailableFeatures

        val vulkanLevel = features.find { it.name == FEATURE_VULKAN_HARDWARE_LEVEL }
        val vulkanVersion = features.find { it.name == FEATURE_VULKAN_HARDWARE_VERSION }

        val levelStr = vulkanLevel?.version?.toString() ?: "Not available"
        val versionStr = vulkanVersion?.let { formatVulkanVersion(it.version) } ?: "Not available"

        val gpuName = getGpuName()

        return buildString {
            appendLine("GPU: $gpuName")
            appendLine("Vulkan Level: $levelStr")
            appendLine("Vulkan Version: $versionStr")
            appendLine("Android: ${Build.VERSION.SDK_INT}")
            appendLine("SOC: ${Build.SOC_MODEL}")
        }
    }

    /**
     * Check if Vulkan is supported on this device.
     */
    fun isVulkanSupported(context: Context): Boolean {
        val pm = context.packageManager

        // Check for Vulkan feature
        if (!pm.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)) {
            Log.w(TAG, "Device does not support Vulkan hardware")
            return false
        }

        // Check for Vulkan 1.1 minimum
        val features = pm.systemAvailableFeatures
        val vulkanVersion = features.find { it.name == FEATURE_VULKAN_HARDWARE_VERSION }

        if (vulkanVersion != null) {
            val major = (vulkanVersion.version shr 22) and 0x3FF
            val minor = (vulkanVersion.version shr 12) and 0x3FF

            if (major < 1 || (major == 1 && minor < 1)) {
                Log.w(TAG, "Vulkan version too low: $major.$minor")
                return false
            }
        }

        // Check for GPU driver
        val driverPath = "/system/lib64/libvulkan.so"
        if (!File(driverPath).exists()) {
            Log.w(TAG, "Vulkan driver not found at $driverPath")
            return false
        }

        return true
    }

    /**
     * Get the GPU name from system properties.
     */
    fun getGpuName(): String {
        return try {
            // Try to get from Build
            val soc = Build.SOC_MODEL
            val board = Build.BOARD

            when {
                soc.contains("MT", ignoreCase = true) -> {
                    val gpuType = when {
                        soc.contains("9400") -> "Mali-G925"
                        soc.contains("9300") -> "Mali-G720"
                        soc.contains("9200") -> "Mali-G715"
                        soc.contains("9000") -> "Mali-G710"
                        soc.contains("8300") -> "Mali-G615"
                        soc.contains("8200") -> "Mali-G610"
                        soc.contains("1300") -> "Mali-G77"
                        else -> "Mali GPU"
                    }
                    "$gpuType (MediaTek $soc)"
                }
                board.contains("mt", ignoreCase = true) -> "Mali GPU (MediaTek $board)"
                else -> "Unknown GPU ($soc)"
            }
        } catch (e: Exception) {
            "Unknown GPU"
        }
    }

    /**
     * Format Vulkan version from packed integer.
     */
    private fun formatVulkanVersion(version: Int): String {
        val major = (version shr 22) and 0x3FF
        val minor = (version shr 12) and 0x3FF
        val patch = version and 0xFFF
        return "$major.$minor.$patch"
    }

    /**
     * Setup Vortek ICD configuration for Vulkan passthrough via IPC.
     *
     * Vortek solves the glibc/Bionic incompatibility by using inter-process communication:
     * - libvulkan_vortek.so (in container) serializes Vulkan commands
     * - VortekRenderer (Android side) executes them on the real Mali GPU
     *
     * Note: The library is ARM64 because Box64 handles x86→ARM translation.
     * Vulkan calls from x86 games go through Box64 which calls the ARM64
     * libvulkan_vortek.so, which then communicates with Android's VortekRenderer.
     *
     * @param rootfsPath Path to the container rootfs
     * @return true if setup was successful
     */
    fun setupVortekIcd(rootfsPath: String): Boolean {
        return try {
            // Create ICD directory
            val icdDir = File(rootfsPath, "usr/share/vulkan/icd.d")
            icdDir.mkdirs()

            // Vortek ICD configuration
            // Points to the ARM64 Vulkan ICD that communicates with Android's VortekRenderer
            // (Box64 handles the x86→ARM64 translation layer)
            val vortekIcd = """
                {
                    "file_format_version": "1.0.0",
                    "ICD": {
                        "library_path": "/lib/libvulkan_vortek.so",
                        "api_version": "1.1.128"
                    }
                }
            """.trimIndent()

            File(icdDir, "vortek_icd.json").writeText(vortekIcd)
            Log.i(TAG, "Vortek ICD configuration created")
            true

        } catch (e: Exception) {
            Log.e(TAG, "Failed to setup Vortek ICD", e)
            false
        }
    }

    /**
     * Setup Vulkan ICD configuration for the container.
     * This creates the necessary configuration files for Vulkan passthrough.
     */
    fun setupVulkanPassthrough(rootfsPath: String): Boolean {
        return try {
            // Create ICD directory
            val icdDir = File(rootfsPath, "usr/share/vulkan/icd.d")
            icdDir.mkdirs()

            // Create Android Vulkan ICD
            val androidIcd = """
                {
                    "file_format_version": "1.0.0",
                    "ICD": {
                        "library_path": "/system/lib64/libvulkan.so",
                        "api_version": "1.3.0"
                    }
                }
            """.trimIndent()
            File(icdDir, "android_icd.json").writeText(androidIcd)

            // Create Mali-specific ICD (for direct passthrough)
            val maliIcd = """
                {
                    "file_format_version": "1.0.0",
                    "ICD": {
                        "library_path": "/vendor/lib64/hw/vulkan.mali.so",
                        "api_version": "1.3.0"
                    }
                }
            """.trimIndent()
            File(icdDir, "mali_icd.json").writeText(maliIcd)

            // Create environment setup script
            val envScript = """
                #!/bin/bash
                # Vulkan environment for Android passthrough

                # Use Android Vulkan driver
                export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/android_icd.json

                # WSI configuration
                export MESA_VK_WSI_PRESENT_MODE=fifo
                export VK_LAYER_PATH=/usr/share/vulkan/explicit_layer.d

                # Mali-specific optimizations
                export MALI_NO_ASYNC_COMPUTE=1

                # DXVK settings for Proton
                export DXVK_ASYNC=1
                export DXVK_STATE_CACHE=1
                export DXVK_LOG_LEVEL=none

                # Disable software rendering fallbacks
                export LIBGL_ALWAYS_SOFTWARE=0
                export MESA_GL_VERSION_OVERRIDE=4.6
                export MESA_GLSL_VERSION_OVERRIDE=460
            """.trimIndent()

            val scriptsDir = File(rootfsPath, "opt/scripts")
            scriptsDir.mkdirs()
            File(scriptsDir, "vulkan_env.sh").apply {
                writeText(envScript)
                setExecutable(true)
            }

            Log.i(TAG, "Vulkan passthrough configured")
            true

        } catch (e: Exception) {
            Log.e(TAG, "Failed to setup Vulkan passthrough", e)
            false
        }
    }

    /**
     * Create Vulkan layer configuration for debugging.
     */
    fun setupVulkanLayers(rootfsPath: String, enableValidation: Boolean = false): Boolean {
        if (!enableValidation) return true

        return try {
            val layerDir = File(rootfsPath, "usr/share/vulkan/explicit_layer.d")
            layerDir.mkdirs()

            // Validation layer configuration
            val validationLayer = """
                {
                    "file_format_version": "1.0.0",
                    "layer": {
                        "name": "VK_LAYER_KHRONOS_validation",
                        "type": "GLOBAL",
                        "library_path": "libVkLayer_khronos_validation.so",
                        "api_version": "1.3.0",
                        "implementation_version": "1",
                        "description": "Khronos Validation Layer"
                    }
                }
            """.trimIndent()
            File(layerDir, "VkLayer_khronos_validation.json").writeText(validationLayer)

            Log.i(TAG, "Vulkan validation layers configured")
            true

        } catch (e: Exception) {
            Log.e(TAG, "Failed to setup Vulkan layers", e)
            false
        }
    }

    /**
     * Get recommended Vulkan environment variables for Steam/Proton.
     *
     * @param useVortek If true, configure for Vortek passthrough. If false, use direct Android ICD.
     * @param vortekSocketPath Path to the Vortek Unix socket (required if useVortek is true)
     * @param enableHeadless If true, include LD_PRELOAD for headless frame capture
     * @param enableDxvkHud If true, show DXVK FPS overlay
     */
    fun getProtonVulkanEnv(
        useVortek: Boolean = false,
        vortekSocketPath: String? = null,
        enableHeadless: Boolean = true,
        enableDxvkHud: Boolean = false
    ): Map<String, String> {
        val baseEnv = mutableMapOf(
            // WSI
            "MESA_VK_WSI_PRESENT_MODE" to "fifo",
            "VK_LAYER_PATH" to "/usr/share/vulkan/explicit_layer.d",

            // DXVK (DirectX to Vulkan)
            "DXVK_ASYNC" to "1",
            "DXVK_STATE_CACHE" to "1",
            "DXVK_LOG_LEVEL" to "info",
            "DXVK_HUD" to if (enableDxvkHud) "fps,devinfo" else "",

            // VKD3D (DirectX 12 to Vulkan)
            "VKD3D_FEATURE_LEVEL" to "12_1",
            "VKD3D_DISABLE_EXTENSIONS" to "",

            // Proton — esync ENABLED (Android kernel supports eventfd, FEX passes syscalls through)
            // fsync disabled: futex_waitv requires Linux 5.16+, not on Android 14 kernel
            "PROTON_USE_WINED3D" to "0",
            "PROTON_NO_FSYNC" to "1",
            "PROTON_ENABLE_NVAPI" to "0",
            "PROTON_HIDE_NVIDIA_GPU" to "0",

            // Mesa
            "MESA_GL_VERSION_OVERRIDE" to "4.6",
            "MESA_GLSL_VERSION_OVERRIDE" to "460",

            // Mali optimizations
            "MALI_NO_ASYNC_COMPUTE" to "1"
        )

        if (useVortek && vortekSocketPath != null) {
            // Vortek passthrough configuration
            baseEnv.putAll(mapOf(
                "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/vortek_icd.json",
                "VORTEK_SERVER_PATH" to vortekSocketPath,
                "VK_DRIVER_FILES" to "" // Disable direct driver loading
            ))
        } else {
            // Direct Android ICD (won't work from glibc, but kept for reference)
            baseEnv["VK_ICD_FILENAMES"] = "/usr/share/vulkan/icd.d/android_icd.json"
        }

        // Headless frame capture: intercepts Vulkan surface/swapchain creation
        // and sends rendered frames via TCP 19850 to FrameSocketServer
        if (enableHeadless) {
            baseEnv["LD_PRELOAD"] = "/usr/lib/libvulkan_headless.so"
        }

        return baseEnv
    }

    /**
     * Test Vulkan functionality inside the container.
     */
    fun testVulkanInContainer(fexExecutor: FexExecutor): String {
        val result = fexExecutor.executeBlocking(
            command = """
                if command -v vulkaninfo &> /dev/null; then
                    vulkaninfo --summary 2>&1
                else
                    echo "vulkaninfo not installed"
                fi
            """.trimIndent(),
            environment = getProtonVulkanEnv()
        )

        return if (result.isSuccess) {
            result.stdout
        } else {
            "Vulkan test failed: ${result.stderr}"
        }
    }
}
