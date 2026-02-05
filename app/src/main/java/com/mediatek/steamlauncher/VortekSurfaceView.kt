package com.mediatek.steamlauncher

import android.content.Context
import android.graphics.PixelFormat
import android.os.Build
import android.util.AttributeSet
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.annotation.RequiresApi

/**
 * SurfaceView that displays Vortek's Vulkan rendering output.
 *
 * This view provides a Surface that the FramebufferBridge blits to
 * after Vortek completes rendering. It bypasses X11 entirely.
 *
 * Usage:
 * 1. Create VortekSurfaceView in your layout
 * 2. Connect to FramebufferBridge when surface is ready
 * 3. Start Vortek server with FramebufferBridge as WindowInfoProvider
 * 4. Vortek renders to HardwareBuffer, FramebufferBridge blits to this view
 */
@RequiresApi(Build.VERSION_CODES.O)
class VortekSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : SurfaceView(context, attrs, defStyleAttr) {

    companion object {
        private const val TAG = "VortekSurfaceView"
    }

    // Surface state
    private var surfaceReady = false
    private var surfaceWidth = 0
    private var surfaceHeight = 0

    // FramebufferBridge connection
    private var framebufferBridge: FramebufferBridge? = null

    // Callback for surface events
    interface Callback {
        fun onSurfaceReady(surface: Surface, width: Int, height: Int)
        fun onSurfaceDestroyed()
        fun onSurfaceSizeChanged(width: Int, height: Int)
    }

    private var callback: Callback? = null

    private val surfaceCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            // Set BGRA_8888 format for optimal Vulkan compatibility
            holder.setFormat(PixelFormat.RGBA_8888)
            Log.d(TAG, "Surface created")
        }

        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
            Log.i(TAG, "Surface changed: ${width}x${height}, format=$format")

            surfaceWidth = width
            surfaceHeight = height
            surfaceReady = true

            // Update FramebufferBridge
            framebufferBridge?.let {
                it.setOutputSurface(holder.surface)
                it.resize(width, height)
            }

            // Notify callback
            callback?.onSurfaceReady(holder.surface, width, height)
        }

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            Log.d(TAG, "Surface destroyed")
            surfaceReady = false

            // Clear FramebufferBridge surface
            framebufferBridge?.setOutputSurface(null)

            callback?.onSurfaceDestroyed()
        }
    }

    init {
        holder.addCallback(surfaceCallback)

        // Make view focusable for input events
        isFocusable = true
        isFocusableInTouchMode = true
        requestFocus()

        Log.d(TAG, "VortekSurfaceView initialized")
    }

    /**
     * Set the callback for surface events.
     */
    fun setCallback(callback: Callback?) {
        this.callback = callback
    }

    /**
     * Connect a FramebufferBridge to this view.
     * The bridge will blit rendered content to this view's Surface.
     */
    fun connectFramebufferBridge(bridge: FramebufferBridge) {
        framebufferBridge = bridge

        if (surfaceReady) {
            bridge.setOutputSurface(holder.surface)
            bridge.resize(surfaceWidth, surfaceHeight)
        }

        Log.i(TAG, "FramebufferBridge connected")
    }

    /**
     * Disconnect the FramebufferBridge.
     */
    fun disconnectFramebufferBridge() {
        framebufferBridge?.setOutputSurface(null)
        framebufferBridge = null
        Log.i(TAG, "FramebufferBridge disconnected")
    }

    /**
     * Create a new FramebufferBridge connected to this view.
     */
    fun createFramebufferBridge(): FramebufferBridge {
        val surface = if (surfaceReady) holder.surface else null
        val bridge = FramebufferBridge(
            surface,
            if (surfaceWidth > 0) surfaceWidth else 1920,
            if (surfaceHeight > 0) surfaceHeight else 1080
        )
        framebufferBridge = bridge
        return bridge
    }

    /**
     * Check if the surface is ready for rendering.
     */
    fun isSurfaceReady(): Boolean = surfaceReady

    /**
     * Get the current surface dimensions.
     */
    fun getSurfaceWidth(): Int = surfaceWidth
    fun getSurfaceHeight(): Int = surfaceHeight

    /**
     * Get the Surface object (for manual blitting if needed).
     */
    fun getSurface(): Surface? = if (surfaceReady) holder.surface else null
}
