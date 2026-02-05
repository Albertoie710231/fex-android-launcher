package com.mediatek.steamlauncher

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.hardware.HardwareBuffer
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import androidx.annotation.RequiresApi
import com.winlator.xenvironment.components.VortekRendererComponent.WindowInfoProvider
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * FramebufferBridge connects Vortek's Vulkan rendering to an Android Surface.
 *
 * Architecture:
 * 1. We create HardwareBuffers that Vortek can render to
 * 2. Native code calls getWindowHardwareBuffer() to get AHardwareBuffer* pointer
 * 3. Native code renders Vulkan content to the HardwareBuffer
 * 4. Native code calls updateWindowContent() when done
 * 5. We wrap the HardwareBuffer as Bitmap and blit to output Surface
 *
 * This bypasses X11 entirely - the container app renders via Vulkan,
 * and we display the result directly on Android.
 */
@RequiresApi(Build.VERSION_CODES.O)
class FramebufferBridge(
    private var outputSurface: Surface?,
    private var width: Int,
    private var height: Int
) : WindowInfoProvider {

    companion object {
        private const val TAG = "FramebufferBridge"

        // HardwareBuffer format and usage flags
        private const val AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1
        private const val USAGE_FLAGS = (
            HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or
            HardwareBuffer.USAGE_GPU_COLOR_OUTPUT or
            HardwareBuffer.USAGE_CPU_READ_OFTEN
        ).toLong()

        private var nativeLibLoaded = false

        init {
            try {
                System.loadLibrary("framebuffer_bridge")
                nativeLibLoaded = true
                Log.i(TAG, "libframebuffer_bridge.so loaded successfully")
            } catch (e: UnsatisfiedLinkError) {
                Log.w(TAG, "libframebuffer_bridge.so not loaded: ${e.message}")
            }
        }
    }

    // Per-window HardwareBuffers and their native pointers
    private data class WindowBuffer(
        val buffer: HardwareBuffer,
        val nativePtr: Long
    )
    private val windowBuffers = ConcurrentHashMap<Int, WindowBuffer>()

    // Background thread for rendering
    private val renderThread = HandlerThread("FramebufferBridge").apply { start() }
    private val renderHandler = Handler(renderThread.looper)

    // Paint for blitting
    private val paint = Paint().apply {
        isFilterBitmap = false
        isAntiAlias = false
    }

    // Frame stats
    private var frameCount = 0L
    private var lastStatsTime = System.currentTimeMillis()
    private val isRunning = AtomicBoolean(true)

    // Callback for frame updates
    var onFrameRendered: ((windowId: Int) -> Unit)? = null

    init {
        Log.i(TAG, "Initializing FramebufferBridge: ${width}x${height}, native lib: $nativeLibLoaded")
    }

    /**
     * Get or create a HardwareBuffer for a window.
     */
    private fun getOrCreateBuffer(windowId: Int): WindowBuffer? {
        return windowBuffers.getOrPut(windowId) {
            Log.i(TAG, "Creating HardwareBuffer for window $windowId: ${width}x${height}")

            try {
                // Create HardwareBuffer with GPU-compatible usage flags
                val buffer = HardwareBuffer.create(
                    width,
                    height,
                    HardwareBuffer.RGBA_8888,
                    1,  // layers
                    USAGE_FLAGS
                )

                // Get the native AHardwareBuffer* pointer
                val nativePtr = if (nativeLibLoaded) {
                    getNativeHardwareBuffer(buffer)
                } else {
                    // Fallback: use reflection (less reliable)
                    getNativeHardwareBufferReflection(buffer)
                }

                Log.i(TAG, "Created buffer for window $windowId: ptr=0x${nativePtr.toString(16)}")
                WindowBuffer(buffer, nativePtr)
            } catch (e: Exception) {
                Log.e(TAG, "Failed to create HardwareBuffer: ${e.message}")
                null
            }
        }
    }

    // === WindowInfoProvider implementation ===

    override fun getWindowWidth(windowId: Int): Int {
        return width
    }

    override fun getWindowHeight(windowId: Int): Int {
        return height
    }

    override fun getWindowHardwareBuffer(windowId: Int): Long {
        val windowBuffer = getOrCreateBuffer(windowId)
        return windowBuffer?.nativePtr ?: 0L
    }

    override fun updateWindowContent(windowId: Int) {
        // Called by native code after Vulkan rendering is complete
        frameCount++

        // Log stats periodically
        val now = System.currentTimeMillis()
        if (now - lastStatsTime > 5000) {
            val fps = frameCount * 1000.0 / (now - lastStatsTime)
            Log.i(TAG, "Window $windowId: $frameCount frames, ${String.format("%.1f", fps)} FPS")
            frameCount = 0
            lastStatsTime = now
        }

        // Blit the HardwareBuffer to the output Surface
        if (isRunning.get()) {
            renderHandler.post {
                blitToSurface(windowId)
            }
        }

        onFrameRendered?.invoke(windowId)
    }

    /**
     * Blit the window's HardwareBuffer to the output Surface.
     */
    @RequiresApi(Build.VERSION_CODES.P)
    private fun blitToSurface(windowId: Int) {
        val surface = outputSurface ?: return
        val windowBuffer = windowBuffers[windowId] ?: return

        try {
            // Wrap the HardwareBuffer as a Bitmap
            val bitmap = Bitmap.wrapHardwareBuffer(windowBuffer.buffer, null)
            if (bitmap == null) {
                Log.w(TAG, "Failed to wrap HardwareBuffer as Bitmap")
                return
            }

            // Lock the Surface canvas and draw
            val canvas = surface.lockCanvas(null)
            try {
                canvas.drawBitmap(bitmap, 0f, 0f, paint)
            } finally {
                surface.unlockCanvasAndPost(canvas)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error blitting to surface: ${e.message}")
        }
    }

    /**
     * Update the output Surface (e.g., when view is recreated).
     */
    fun setOutputSurface(surface: Surface?) {
        outputSurface = surface
        Log.i(TAG, "Output surface updated: ${surface != null}")
    }

    /**
     * Resize all framebuffers to new dimensions.
     */
    fun resize(newWidth: Int, newHeight: Int) {
        if (width == newWidth && height == newHeight) return

        Log.i(TAG, "Resizing framebuffers: ${newWidth}x${newHeight}")

        // Release old buffers
        windowBuffers.values.forEach { wb ->
            if (nativeLibLoaded && wb.nativePtr != 0L) {
                releaseNativeHardwareBuffer(wb.nativePtr)
            }
            wb.buffer.close()
        }
        windowBuffers.clear()

        // Update dimensions
        width = newWidth
        height = newHeight
    }

    /**
     * Release all resources.
     */
    fun release() {
        Log.i(TAG, "Releasing FramebufferBridge")
        isRunning.set(false)

        windowBuffers.values.forEach { wb ->
            if (nativeLibLoaded && wb.nativePtr != 0L) {
                releaseNativeHardwareBuffer(wb.nativePtr)
            }
            wb.buffer.close()
        }
        windowBuffers.clear()

        renderThread.quitSafely()
    }

    // === Native methods (from libframebuffer_bridge.so) ===

    /**
     * Get native AHardwareBuffer* pointer from Java HardwareBuffer.
     */
    private external fun getNativeHardwareBuffer(buffer: HardwareBuffer): Long

    /**
     * Release a native AHardwareBuffer reference.
     */
    private external fun releaseNativeHardwareBuffer(nativePtr: Long)

    /**
     * Fallback: Get native pointer using reflection.
     */
    private fun getNativeHardwareBufferReflection(buffer: HardwareBuffer): Long {
        return try {
            val field = HardwareBuffer::class.java.getDeclaredField("mNativeObject")
            field.isAccessible = true
            field.getLong(buffer)
        } catch (e: Exception) {
            Log.w(TAG, "Reflection fallback failed: ${e.message}")
            0L
        }
    }
}
