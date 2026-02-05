package com.mediatek.steamlauncher

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.util.Log
import android.view.Surface
import java.io.InputStream
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * TCP socket server for receiving Vulkan frames from proot.
 *
 * Uses TCP on localhost instead of Unix sockets for proot compatibility.
 *
 * The native wrapper sends frames in format:
 * - 4 bytes: width (little-endian uint32)
 * - 4 bytes: height (little-endian uint32)
 * - width * height * 4 bytes: RGBA pixel data
 */
class FrameSocketServer(private val port: Int = 19850) {

    companion object {
        private const val TAG = "FrameSocketServer"
    }

    private var serverSocket: ServerSocket? = null
    private var clientSocket: Socket? = null
    private var listenerThread: Thread? = null
    private var receiverThread: Thread? = null
    private val running = AtomicBoolean(false)

    // Output surface for rendering
    private var outputSurface: Surface? = null
    private val paint = Paint().apply {
        isFilterBitmap = false
        isAntiAlias = false
    }

    // Frame stats
    private var frameCount = 0L
    private var lastStatsTime = System.currentTimeMillis()

    /**
     * Set the output surface for rendering frames.
     */
    fun setOutputSurface(surface: Surface?) {
        outputSurface = surface
        Log.i(TAG, "Output surface set: ${surface != null}")
    }

    /**
     * Start listening for frame connections.
     */
    fun start(): Boolean {
        if (running.get()) {
            Log.w(TAG, "Already running")
            return true
        }

        return try {
            serverSocket = ServerSocket(port)
            Log.i(TAG, "Frame socket server listening on port $port")

            running.set(true)
            listenerThread = Thread({
                acceptLoop()
            }, "Frame-Socket-Accept").apply {
                isDaemon = true
                start()
            }

            true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start frame socket server on port $port", e)
            false
        }
    }

    /**
     * Accept connections and start receiving frames.
     */
    private fun acceptLoop() {
        Log.i(TAG, "Frame accept loop started on port $port")

        while (running.get()) {
            try {
                val server = serverSocket ?: break
                val newClient = server.accept()

                Log.i(TAG, "Frame client connected from ${newClient.inetAddress}")

                // Close previous client if any
                clientSocket?.close()
                clientSocket = newClient

                // Start receiver thread for this client
                receiverThread?.interrupt()
                receiverThread = Thread({
                    receiveFrames(newClient)
                }, "Frame-Receiver").apply {
                    isDaemon = true
                    start()
                }

            } catch (e: Exception) {
                if (running.get()) {
                    Log.e(TAG, "Error accepting frame connection", e)
                    Thread.sleep(100)
                }
            }
        }

        Log.i(TAG, "Frame accept loop ended")
    }

    /**
     * Receive and display frames from client.
     */
    private fun receiveFrames(socket: Socket) {
        Log.i(TAG, "Receiving frames from ${socket.inetAddress}")

        val inputStream = socket.getInputStream()
        val headerBuffer = ByteArray(8)
        var pixelBuffer: ByteArray? = null

        while (running.get() && !socket.isClosed) {
            try {
                // Read header: width + height
                var bytesRead = 0
                while (bytesRead < 8) {
                    val n = inputStream.read(headerBuffer, bytesRead, 8 - bytesRead)
                    if (n <= 0) {
                        Log.w(TAG, "Client disconnected (header read returned $n)")
                        return
                    }
                    bytesRead += n
                }

                // Parse header
                val bb = ByteBuffer.wrap(headerBuffer).order(ByteOrder.LITTLE_ENDIAN)
                val width = bb.getInt()
                val height = bb.getInt()

                if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
                    Log.e(TAG, "Invalid frame dimensions: ${width}x${height}")
                    continue
                }

                // Read pixel data
                val pixelSize = width * height * 4
                if (pixelBuffer == null || pixelBuffer.size < pixelSize) {
                    pixelBuffer = ByteArray(pixelSize)
                }

                bytesRead = 0
                while (bytesRead < pixelSize) {
                    val n = inputStream.read(pixelBuffer, bytesRead, pixelSize - bytesRead)
                    if (n <= 0) {
                        Log.w(TAG, "Client disconnected (pixel read returned $n)")
                        return
                    }
                    bytesRead += n
                }

                // Display frame
                displayFrame(width, height, pixelBuffer)

                // Stats
                frameCount++
                val now = System.currentTimeMillis()
                if (now - lastStatsTime > 5000) {
                    val fps = frameCount * 1000.0 / (now - lastStatsTime)
                    Log.i(TAG, "Frame rate: %.1f FPS (%d frames)".format(fps, frameCount))
                    frameCount = 0
                    lastStatsTime = now
                }

            } catch (e: Exception) {
                if (running.get()) {
                    Log.e(TAG, "Error receiving frame", e)
                }
                break
            }
        }

        Log.i(TAG, "Frame receiver ended")
    }

    /**
     * Display a frame on the output surface.
     */
    private fun displayFrame(width: Int, height: Int, pixels: ByteArray) {
        // Save first frame to file for debugging
        if (frameCount == 0L) {
            try {
                val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))

                // Save to app's files directory (accessible via run-as)
                val file = java.io.File("/data/data/com.mediatek.steamlauncher/files/vkcube_frame.png")
                java.io.FileOutputStream(file).use { out ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
                }
                Log.i(TAG, "Saved first frame to ${file.absolutePath}")
                bitmap.recycle()
            } catch (e: Exception) {
                Log.e(TAG, "Failed to save frame: ${e.message}")
            }
        }

        // For now, just log that we received the frame
        // TODO: Need separate SurfaceView for frame display (can't use LorieView's surface)
        if (frameCount < 5) {
            Log.i(TAG, "Received frame ${frameCount}: ${width}x${height}")
        }
    }

    /**
     * Stop the server.
     */
    fun stop() {
        Log.i(TAG, "Stopping frame socket server")
        running.set(false)

        clientSocket?.close()
        clientSocket = null

        serverSocket?.close()
        serverSocket = null

        listenerThread?.interrupt()
        listenerThread = null
        receiverThread?.interrupt()
        receiverThread = null

        Log.i(TAG, "Frame socket server stopped")
    }

    fun isRunning(): Boolean = running.get()
}
