package com.mediatek.steamlauncher

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.termux.x11.CmdEntryPoint
import com.termux.x11.LorieView
import java.io.File

/**
 * X11 Server wrapper using libXlorie from Termux:X11.
 *
 * The X11 server architecture is:
 * 1. X11SocketServer creates a Unix socket at /tmp/.X11-unix/X0 (bound by proot)
 * 2. When an X11 client connects, the fd is passed to LorieView.connect()
 * 3. LorieView handles X11 protocol processing and renders to the Android surface
 * 4. CmdEntryPoint.startServer() initializes the X11 internals
 */
class X11Server(private val context: Context) {

    companion object {
        private const val TAG = "X11Server"
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    private var isRunning = false
    private var lorieView: LorieView? = null
    private var socketServer: X11SocketServer? = null
    private val handler = Handler(Looper.getMainLooper())

    var onServerStarted: (() -> Unit)? = null
    var onClientConnected: (() -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    /**
     * Start the X11 server.
     *
     * This starts both the X11 internals (via CmdEntryPoint) and a Unix socket
     * listener that proot can access at /tmp/.X11-unix/X0.
     */
    fun start(): Boolean {
        if (isRunning) {
            Log.w(TAG, "X11 server already running (this instance)")
            return true
        }

        Log.i(TAG, "Starting X11 server")

        // Reset connection state before starting
        CmdEntryPoint.resetConnectionState()

        return try {
            // Create socket directory and clean old sockets
            val socketDir = File(app.getX11SocketDir())
            socketDir.mkdirs()

            // Remove old socket files if they exist (X0, X1, etc.)
            socketDir.listFiles()?.forEach { file ->
                if (file.name.startsWith("X") && file.name.length <= 3) {
                    file.delete()
                    Log.i(TAG, "Removed old socket: ${file.name}")
                }
            }

            // Ensure directory has proper permissions (777 for proot access)
            Runtime.getRuntime().exec(arrayOf("chmod", "777", socketDir.absolutePath)).waitFor()
            Log.i(TAG, "Socket dir: ${socketDir.absolutePath}")

            // Start the X11 internals - initializes rendering pipeline
            if (!CmdEntryPoint.isServerStarted()) {
                val started = CmdEntryPoint.startServer()
                if (!started) {
                    Log.e(TAG, "Failed to start X11 internals")
                    handler.post { onError?.invoke("Failed to start X11 server") }
                    return false
                }
                Thread.sleep(100)
            }

            // Create a symlink from where proot expects the socket to where libXlorie creates it
            // libXlorie creates an abstract socket, so we need to bridge this
            // For now, clients should use TCP: DISPLAY=localhost:0 or DISPLAY=127.0.0.1:0
            // Or use our socket server for bridging (disabled for now due to crashes)

            // NOTE: libXlorie doesn't expose filesystem sockets easily.
            // Best approach for proot X11 clients is to use TCP.
            // DISPLAY=127.0.0.1:0 connects to TCP port 6000 if enabled.

            isRunning = true
            Log.i(TAG, "X11 server started successfully")
            Log.i(TAG, "For proot X11 apps, use: DISPLAY=127.0.0.1:0 (if TCP enabled)")
            Log.i(TAG, "Or use: DISPLAY=:0 with proper socket forwarding")

            handler.post { onServerStarted?.invoke() }
            true
        } catch (e: Exception) {
            Log.e(TAG, "Error starting X11 server", e)
            handler.post { onError?.invoke(e.message ?: "Unknown error") }
            false
        }
    }

    /**
     * Attach LorieView for rendering.
     * THIS IS THE KEY FIX: We must call LorieView.connect(fd) to establish
     * the connection between the X11 server and the rendering surface.
     */
    fun attachSurface(view: LorieView) {
        lorieView = view
        Log.i(TAG, "Attaching LorieView surface")

        // Set up callback for surface changes (but NOT surfaceReadyCallback - that's for initial startup only)
        view.setCallback(object : LorieView.Callback {
            override fun changed(surfaceWidth: Int, surfaceHeight: Int, screenWidth: Int, screenHeight: Int) {
                Log.d(TAG, "Surface changed: ${surfaceWidth}x${surfaceHeight}")
                if (surfaceWidth > 0 && surfaceHeight > 0) {
                    // Connect the X11 server to the LorieView
                    connectToView()
                }
            }
        })

        // Clear surfaceReadyCallback to prevent re-triggering service start
        view.setSurfaceReadyCallback(null)

        // Trigger initial callback to set up rendering
        view.triggerCallback()
    }

    /**
     * Surface is ready for X11 rendering.
     * Establishes the connection and ensures frame delivery is set up.
     */
    private fun connectToView() {
        val view = lorieView ?: return
        Log.i(TAG, "Surface ready for X11 rendering")

        // Check connection status
        val isConnected = LorieView.connected()
        Log.i(TAG, "LorieView.connected() = $isConnected")

        // Get dimensions
        val width = view.width
        val height = view.height

        if (width > 0 && height > 0) {
            // Send window change to ensure X11 knows our dimensions
            LorieView.sendWindowChange(width, height, 60, "Steam Launcher")
            Log.i(TAG, "Sent window change: ${width}x${height}")

            // Request connection to trigger rendering
            val reqResult = LorieView.requestConnection()
            Log.i(TAG, "requestConnection() returned: $reqResult")
        }

        handler.post { onClientConnected?.invoke() }
    }

    fun stop() {
        if (!isRunning) return
        Log.i(TAG, "Stopping X11 server")

        // Stop socket server
        socketServer?.stop()
        socketServer = null

        isRunning = false
    }

    fun isRunning(): Boolean = isRunning

    fun isConnected(): Boolean = CmdEntryPoint.connected() || LorieView.connected()
}
