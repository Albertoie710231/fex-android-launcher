package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import com.winlator.xenvironment.components.VortekRendererComponent

/**
 * Kotlin wrapper for the Vortek Vulkan passthrough renderer.
 *
 * Vortek provides a bridge between glibc applications running in FEX
 * and Android's Bionic-based Vulkan drivers. It works via IPC:
 *
 * 1. VortekRenderer (Android/Bionic side) - Runs as a native server that
 *    receives serialized Vulkan commands and executes them on the real GPU.
 *
 * 2. libvulkan_vortek.so (Container/ARM64 side) - A Vulkan ICD that serializes
 *    Vulkan calls and sends them to the Android renderer via Unix socket.
 *    Note: It's ARM64 because FEX handles x86→ARM translation.
 *
 * This solves the fundamental incompatibility between Android's Bionic libc
 * and the container's glibc - you cannot load a Bionic .so from glibc code.
 */
object VortekRenderer {

    private const val TAG = "VortekRenderer"

    // Track initialization state
    private var isLoaded = false
    private var component: VortekRendererComponent? = null
    private var currentSocketPath: String? = null

    /**
     * Load the native library. Call this before any other methods.
     * Returns true if the library was loaded successfully.
     */
    @Synchronized
    fun loadLibrary(): Boolean {
        if (isLoaded) return true

        return try {
            // The library is loaded by VortekRendererComponent's static initializer
            // We just need to verify it worked by loading the class
            Class.forName("com.winlator.xenvironment.components.VortekRendererComponent")
            isLoaded = true
            Log.i(TAG, "Vortek renderer library loaded successfully")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load Vortek renderer: ${e.message}")
            Log.w(TAG, "Make sure libvortekrenderer.so and libwinlator.so are in jniLibs/arm64-v8a/")
            false
        }
    }

    /**
     * Check if the Vortek library is available.
     */
    fun isAvailable(): Boolean = isLoaded

    /**
     * Check if the Vortek server is currently running.
     */
    fun isRunning(): Boolean = component != null

    /**
     * Start the Vortek Vulkan passthrough server.
     *
     * @param socketPath Path where the Unix socket will be created.
     * @param context Android context to get native library directory
     * @return true if server started successfully
     */
    @Synchronized
    fun start(socketPath: String, context: Context): Boolean {
        if (!isLoaded) {
            Log.e(TAG, "Cannot start Vortek server: library not loaded")
            return false
        }

        if (component != null) {
            Log.w(TAG, "Vortek server already running, stopping first...")
            stop()
        }

        Log.i(TAG, "Starting Vortek server at: $socketPath")

        return try {
            val nativeLibDir = context.applicationInfo.nativeLibraryDir
            val options = VortekRendererComponent.Options()

            // GameNative leaves libvulkanPath null by default (only sets it
            // when the user picks an adrenotools driver). With null, the
            // server falls back to Android's standard /system/lib64/libvulkan.so
            // which goes through the Vulkan loader → Mali HAL. Passing the HAL
            // path directly (our previous value) was breaking device
            // enumeration for the Vortek RPC — DXVK saw "No adapters found".
            options.libvulkanPath = null
            Log.i(TAG, "libvulkanPath: null (defer to Android default libvulkan.so)")

            component = VortekRendererComponent(socketPath, nativeLibDir, options)
            component?.start()
            currentSocketPath = socketPath

            Log.i(TAG, "Vortek server started successfully")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start Vortek server", e)
            component = null
            false
        }
    }

    /**
     * Set window info provider for native callbacks.
     */
    fun setWindowInfoProvider(provider: VortekRendererComponent.WindowInfoProvider) {
        component?.setWindowInfoProvider(provider)
    }

    /**
     * Stop the Vortek server.
     */
    @Synchronized
    fun stop() {
        if (component == null) {
            Log.d(TAG, "Vortek server not running, nothing to stop")
            return
        }

        Log.i(TAG, "Stopping Vortek server...")
        try {
            component?.stop()
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping Vortek server", e)
        } finally {
            component = null
            currentSocketPath = null
        }
        Log.i(TAG, "Vortek server stopped")
    }

    /**
     * Get information about the Vortek server state.
     */
    fun getInfo(): VortekInfo? {
        val comp = component ?: return null

        return VortekInfo(
            isRunning = true,
            socketPath = comp.socketPath ?: currentSocketPath
        )
    }

    data class VortekInfo(
        val isRunning: Boolean,
        val socketPath: String?
    )
}
