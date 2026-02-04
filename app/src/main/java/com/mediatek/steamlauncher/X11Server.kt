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
 * 1. CmdEntryPoint.startServer() creates the X11 socket and starts a listener thread
 * 2. When an X11 client connects, sendBroadcast() is called automatically
 * 3. sendBroadcast() gets the connection fd and calls LorieView.connect(fd)
 * 4. LorieView handles X11 protocol processing and renders to the Android surface
 */
class X11Server(private val context: Context) {

    companion object {
        private const val TAG = "X11Server"
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    private var isRunning = false
    private var lorieView: LorieView? = null
    private val handler = Handler(Looper.getMainLooper())

    var onServerStarted: (() -> Unit)? = null
    var onClientConnected: (() -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    /**
     * Start the X11 server.
     *
     * IMPORTANT: We let libXlorie (CmdEntryPoint) handle all socket operations.
     * When a client connects, CmdEntryPoint.sendBroadcast() is called, which
     * automatically calls LorieView.connect(fd) to process the X11 protocol.
     */
    fun start(): Boolean {
        if (isRunning) {
            Log.w(TAG, "X11 server already running (this instance)")
            return true
        }

        // Check if server is already started at native level (e.g., from previous service lifecycle)
        if (CmdEntryPoint.isServerStarted()) {
            Log.i(TAG, "X11 server already started at native level, reusing")
            isRunning = true
            handler.post { onServerStarted?.invoke() }
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

            // Ensure directory has proper permissions
            Runtime.getRuntime().exec(arrayOf("chmod", "777", socketDir.absolutePath)).waitFor()
            Log.i(TAG, "Socket dir: ${socketDir.absolutePath}")

            // Start the X11 server - let libXlorie handle everything
            // CmdEntryPoint.startServer() will:
            // 1. Create the X11 socket
            // 2. Start a listener thread that accepts connections
            // 3. When client connects, sendBroadcast() is called
            // 4. sendBroadcast() calls LorieView.connect(fd) automatically
            val started = CmdEntryPoint.startServer()

            if (started) {
                isRunning = true
                // Give server time to initialize
                Thread.sleep(300)

                Log.i(TAG, "X11 server started successfully")
                handler.post { onServerStarted?.invoke() }
                true
            } else {
                Log.e(TAG, "Failed to start X11 server")
                handler.post { onError?.invoke("Failed to start X11 server") }
                false
            }
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
        isRunning = false
    }

    fun isRunning(): Boolean = isRunning

    fun isConnected(): Boolean = CmdEntryPoint.connected() || LorieView.connected()
}
