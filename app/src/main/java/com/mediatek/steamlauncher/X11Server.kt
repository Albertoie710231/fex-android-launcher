package com.mediatek.steamlauncher

import android.content.Context
import android.graphics.PixelFormat
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.io.File
import java.net.ServerSocket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * X11 Server implementation that renders X11 content to an Android SurfaceView.
 * This is a bridge between the native Lorie X11 server and Android's graphics system.
 *
 * The X11 server:
 * - Listens on the X11 socket (/tmp/.X11-unix/X0)
 * - Receives rendering commands from X11 clients (Steam, games)
 * - Renders to an Android Surface via OpenGL ES
 * - Forwards input events back to X11 clients
 */
class X11Server(private val context: Context) {

    companion object {
        private const val TAG = "X11Server"
        private const val DEFAULT_DISPLAY = ":0"
        private const val X11_SOCKET_PORT = 6000 // X11 uses port 6000 + display number
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    private val isRunning = AtomicBoolean(false)
    private var nativeServerPtr: Long = 0
    private var serverThread: Thread? = null
    private var surfaceView: LorieView? = null

    // Callbacks for server events
    var onServerStarted: (() -> Unit)? = null
    var onServerStopped: (() -> Unit)? = null
    var onClientConnected: ((clientId: Int) -> Unit)? = null
    var onClientDisconnected: ((clientId: Int) -> Unit)? = null
    var onError: ((message: String) -> Unit)? = null

    private val handler = Handler(Looper.getMainLooper())

    /**
     * Start the X11 server.
     */
    fun start(): Boolean {
        if (isRunning.get()) {
            Log.w(TAG, "X11 server already running")
            return true
        }

        Log.i(TAG, "Starting X11 server on display $DEFAULT_DISPLAY")

        // Create socket directory
        val socketDir = File(app.getX11SocketDir())
        if (!socketDir.exists()) {
            socketDir.mkdirs()
        }

        // Remove stale socket if exists
        val socketFile = File(socketDir, "X0")
        if (socketFile.exists()) {
            socketFile.delete()
        }

        return try {
            // Initialize native server
            nativeServerPtr = nativeInit(
                socketDir.absolutePath,
                0 // Display number
            )

            if (nativeServerPtr == 0L) {
                Log.e(TAG, "Failed to initialize native X11 server")
                return false
            }

            // Start server thread
            serverThread = Thread({
                isRunning.set(true)
                handler.post { onServerStarted?.invoke() }

                try {
                    nativeRun(nativeServerPtr)
                } catch (e: Exception) {
                    Log.e(TAG, "X11 server error", e)
                    handler.post { onError?.invoke(e.message ?: "Unknown error") }
                } finally {
                    isRunning.set(false)
                    handler.post { onServerStopped?.invoke() }
                }
            }, "X11Server").apply {
                priority = Thread.MAX_PRIORITY
                start()
            }

            // Wait for server to be ready
            Thread.sleep(100)

            Log.i(TAG, "X11 server started successfully")
            true

        } catch (e: Exception) {
            Log.e(TAG, "Failed to start X11 server", e)
            handler.post { onError?.invoke(e.message ?: "Failed to start") }
            false
        }
    }

    /**
     * Stop the X11 server.
     */
    fun stop() {
        if (!isRunning.get()) {
            return
        }

        Log.i(TAG, "Stopping X11 server")

        try {
            if (nativeServerPtr != 0L) {
                nativeStop(nativeServerPtr)
            }

            serverThread?.join(2000)

            if (nativeServerPtr != 0L) {
                nativeDestroy(nativeServerPtr)
                nativeServerPtr = 0
            }

        } catch (e: Exception) {
            Log.e(TAG, "Error stopping X11 server", e)
        }

        isRunning.set(false)
    }

    /**
     * Attach a SurfaceView for rendering.
     */
    fun attachSurface(view: LorieView) {
        surfaceView = view

        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.d(TAG, "Surface created")
                if (nativeServerPtr != 0L) {
                    nativeSetSurface(nativeServerPtr, holder.surface)
                }
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.d(TAG, "Surface changed: ${width}x${height}, format=$format")
                if (nativeServerPtr != 0L) {
                    nativeResizeSurface(nativeServerPtr, width, height)
                }
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.d(TAG, "Surface destroyed")
                if (nativeServerPtr != 0L) {
                    nativeSetSurface(nativeServerPtr, null)
                }
            }
        })
    }

    /**
     * Send touch event to X11 server.
     */
    fun sendTouchEvent(action: Int, x: Float, y: Float, pointerId: Int) {
        if (nativeServerPtr != 0L) {
            nativeSendTouch(nativeServerPtr, action, x, y, pointerId)
        }
    }

    /**
     * Send key event to X11 server.
     */
    fun sendKeyEvent(keyCode: Int, isDown: Boolean) {
        if (nativeServerPtr != 0L) {
            nativeSendKey(nativeServerPtr, keyCode, isDown)
        }
    }

    /**
     * Send mouse button event to X11 server.
     */
    fun sendMouseButton(button: Int, isDown: Boolean, x: Float, y: Float) {
        if (nativeServerPtr != 0L) {
            nativeSendMouseButton(nativeServerPtr, button, isDown, x, y)
        }
    }

    /**
     * Send mouse motion event to X11 server.
     */
    fun sendMouseMotion(x: Float, y: Float) {
        if (nativeServerPtr != 0L) {
            nativeSendMouseMotion(nativeServerPtr, x, y)
        }
    }

    /**
     * Send scroll/wheel event to X11 server.
     */
    fun sendScroll(deltaX: Float, deltaY: Float) {
        if (nativeServerPtr != 0L) {
            nativeSendScroll(nativeServerPtr, deltaX, deltaY)
        }
    }

    /**
     * Set clipboard content.
     */
    fun setClipboard(text: String) {
        if (nativeServerPtr != 0L) {
            nativeSetClipboard(nativeServerPtr, text)
        }
    }

    /**
     * Get clipboard content.
     */
    fun getClipboard(): String {
        return if (nativeServerPtr != 0L) {
            nativeGetClipboard(nativeServerPtr)
        } else {
            ""
        }
    }

    fun isRunning(): Boolean = isRunning.get()

    fun getDisplayInfo(): DisplayInfo {
        if (nativeServerPtr == 0L) {
            return DisplayInfo(0, 0, 0)
        }
        return nativeGetDisplayInfo(nativeServerPtr)
    }

    // Native method declarations (implemented in C++)
    private external fun nativeInit(socketPath: String, displayNum: Int): Long
    private external fun nativeRun(ptr: Long)
    private external fun nativeStop(ptr: Long)
    private external fun nativeDestroy(ptr: Long)
    private external fun nativeSetSurface(ptr: Long, surface: android.view.Surface?)
    private external fun nativeResizeSurface(ptr: Long, width: Int, height: Int)
    private external fun nativeSendTouch(ptr: Long, action: Int, x: Float, y: Float, pointerId: Int)
    private external fun nativeSendKey(ptr: Long, keyCode: Int, isDown: Boolean)
    private external fun nativeSendMouseButton(ptr: Long, button: Int, isDown: Boolean, x: Float, y: Float)
    private external fun nativeSendMouseMotion(ptr: Long, x: Float, y: Float)
    private external fun nativeSendScroll(ptr: Long, deltaX: Float, deltaY: Float)
    private external fun nativeSetClipboard(ptr: Long, text: String)
    private external fun nativeGetClipboard(ptr: Long): String
    private external fun nativeGetDisplayInfo(ptr: Long): DisplayInfo

    data class DisplayInfo(
        val width: Int,
        val height: Int,
        val depth: Int
    )
}
