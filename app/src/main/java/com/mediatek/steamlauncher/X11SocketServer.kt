package com.mediatek.steamlauncher

import android.util.Log
import com.termux.x11.LorieView
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Unix domain socket server for X11 connections.
 *
 * Creates a filesystem-based Unix socket at /tmp/.X11-unix/X0 (inside proot)
 * that X11 clients can connect to. When clients connect, the fd is passed
 * to LorieView for X11 protocol processing.
 */
class X11SocketServer(private val socketPath: String) {

    companion object {
        private const val TAG = "X11SocketServer"
    }

    private var serverFd: Int = -1
    private var listenerThread: Thread? = null
    private val running = AtomicBoolean(false)

    var onClientConnected: ((fd: Int) -> Unit)? = null
    var onError: ((String) -> Unit)? = null

    /**
     * Start listening for X11 connections.
     */
    fun start(): Boolean {
        if (running.get()) {
            Log.w(TAG, "Already running")
            return true
        }

        return try {
            // Ensure parent directory exists with proper permissions
            val socketFile = File(socketPath)
            val parentDir = socketFile.parentFile
            parentDir?.mkdirs()

            // Set directory permissions to 777 so proot can access
            parentDir?.let {
                Runtime.getRuntime().exec(arrayOf("chmod", "777", it.absolutePath)).waitFor()
                Log.i(TAG, "Set permissions on: ${it.absolutePath}")
            }

            // Remove old socket file if exists
            if (socketFile.exists()) {
                socketFile.delete()
                Log.i(TAG, "Removed old socket file: $socketPath")
            }

            // Create Unix socket using native helper
            serverFd = X11SocketHelper.createUnixSocket(socketPath)
            if (serverFd < 0) {
                Log.e(TAG, "Failed to create Unix socket at: $socketPath")
                onError?.invoke("Failed to create Unix socket")
                return false
            }

            Log.i(TAG, "Unix socket created at: $socketPath (fd=$serverFd)")

            // Verify socket file exists
            if (socketFile.exists()) {
                Log.i(TAG, "Socket file confirmed: ${socketFile.absolutePath}")
            } else {
                Log.w(TAG, "Socket file not visible in filesystem (may be abstract)")
            }

            // Start listener thread
            running.set(true)
            listenerThread = Thread({
                acceptLoop()
            }, "X11-Socket-Accept").apply {
                isDaemon = true
                start()
            }

            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start X11 socket server", e)
            onError?.invoke(e.message ?: "Unknown error")
            false
        }
    }

    /**
     * Accept loop - waits for connections and passes them to LorieView.
     */
    private fun acceptLoop() {
        Log.i(TAG, "Accept loop started on fd=$serverFd")

        while (running.get() && serverFd >= 0) {
            try {
                // Accept connection (blocking)
                val clientFd = X11SocketHelper.acceptConnection(serverFd)

                if (clientFd < 0) {
                    if (running.get()) {
                        Log.w(TAG, "accept() returned $clientFd")
                        Thread.sleep(100) // Avoid tight loop on error
                    }
                    continue
                }

                Log.i(TAG, "X11 client connected, clientFd=$clientFd")

                // Notify callback
                onClientConnected?.invoke(clientFd)

                // Connect to LorieView for X11 protocol processing
                LorieView.connect(clientFd)
                Log.i(TAG, "Passed fd=$clientFd to LorieView, connected=${LorieView.connected()}")

            } catch (e: Exception) {
                if (running.get()) {
                    Log.e(TAG, "Error accepting connection", e)
                    Thread.sleep(100)
                }
            }
        }

        Log.i(TAG, "Accept loop ended")
    }

    /**
     * Stop the server.
     */
    fun stop() {
        Log.i(TAG, "Stopping X11 socket server")
        running.set(false)

        // Close server socket
        if (serverFd >= 0) {
            X11SocketHelper.closeSocket(serverFd)
            serverFd = -1
        }

        // Interrupt listener thread
        listenerThread?.interrupt()
        listenerThread = null

        // Remove socket file
        X11SocketHelper.unlinkSocket(socketPath)

        Log.i(TAG, "X11 socket server stopped")
    }

    fun isRunning(): Boolean = running.get()

    fun getSocketPath(): String = socketPath
}
