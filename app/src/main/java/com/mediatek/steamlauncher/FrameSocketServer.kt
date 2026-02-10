package com.mediatek.steamlauncher

import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Paint
import android.util.Log
import android.view.Surface
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * TCP socket server for receiving Vulkan frames from the FEX container.
 *
 * Architecture: receiver thread reads each frame and renders it directly
 * via lockHardwareCanvas. The native wrapper caps at ~60 FPS via vsync
 * emulation, so each received frame maps 1:1 to a display frame.
 * lockHardwareCanvas naturally paces to vsync, preventing tearing.
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
    @Volatile
    private var outputSurface: Surface? = null

    // Reusable rendering resources (accessed only from receiver thread)
    private var frameBitmap: Bitmap? = null
    private var bitmapWidth = 0
    private var bitmapHeight = 0
    private val paint = Paint().apply {
        isFilterBitmap = false
        isAntiAlias = false
        // Swizzle R↔B: GPU/Vortek outputs RGBA bytes but Android ARGB_8888 expects BGRA
        colorFilter = ColorMatrixColorFilter(ColorMatrix(floatArrayOf(
            0f, 0f, 1f, 0f, 0f,  // R ← B
            0f, 1f, 0f, 0f, 0f,  // G ← G
            1f, 0f, 0f, 0f, 0f,  // B ← R
            0f, 0f, 0f, 1f, 0f   // A ← A
        )))
    }

    // Frame stats
    private var frameCount = 0L
    private var receivedCount = 0L
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

                // Low-latency TCP settings
                newClient.tcpNoDelay = true
                newClient.receiveBufferSize = 4 * 1024 * 1024  // 4MB receive buffer

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
     * Receive frames and render each one directly via lockHardwareCanvas.
     * Since the native wrapper caps at ~60 FPS, lockHardwareCanvas naturally
     * paces to vsync giving smooth 1:1 frame delivery.
     */
    private fun receiveFrames(socket: Socket) {
        Log.i(TAG, "Receiving frames from ${socket.inetAddress}")

        val inputStream = socket.getInputStream()
        val headerBuffer = ByteArray(8)
        var pixelBuffer = ByteArray(0)

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

                val pixelSize = width * height * 4

                // Reuse buffer if large enough
                if (pixelBuffer.size < pixelSize) {
                    pixelBuffer = ByteArray(pixelSize)
                }

                // Read pixel data
                bytesRead = 0
                while (bytesRead < pixelSize) {
                    val n = inputStream.read(pixelBuffer, bytesRead, pixelSize - bytesRead)
                    if (n <= 0) {
                        Log.w(TAG, "Client disconnected (pixel read returned $n)")
                        return
                    }
                    bytesRead += n
                }

                // Save first frame to file for debugging
                if (receivedCount == 0L) {
                    saveDebugFrame(width, height, pixelBuffer)
                }

                receivedCount++

                // Render this frame directly
                renderFrame(width, height, pixelBuffer)

                // Stats
                val now = System.currentTimeMillis()
                if (now - lastStatsTime > 5000) {
                    val recvFps = receivedCount * 1000.0 / (now - lastStatsTime)
                    val dispFps = frameCount * 1000.0 / (now - lastStatsTime)
                    Log.i(TAG, "Recv: %.1f FPS, Display: %.1f FPS".format(recvFps, dispFps))
                    receivedCount = 0
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
     * Render a frame directly using lockHardwareCanvas.
     * Called from the receiver thread — lockHardwareCanvas paces to vsync.
     */
    private fun renderFrame(width: Int, height: Int, pixels: ByteArray) {
        val surface = outputSurface
        if (surface == null || !surface.isValid) return

        try {
            // Reuse or create bitmap
            if (frameBitmap == null || bitmapWidth != width || bitmapHeight != height) {
                frameBitmap?.recycle()
                frameBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bitmapWidth = width
                bitmapHeight = height
                Log.i(TAG, "Created frame bitmap: ${width}x${height}")
            }

            // Copy pixels to bitmap
            frameBitmap!!.copyPixelsFromBuffer(ByteBuffer.wrap(pixels, 0, width * height * 4))

            // lockHardwareCanvas blocks until vsync — natural frame pacing
            val canvas = try {
                surface.lockHardwareCanvas()
            } catch (e: Exception) {
                try {
                    surface.lockCanvas(null)
                } catch (e2: Exception) {
                    return
                }
            } ?: return

            // Scale to fit surface, centered
            val scaleX = canvas.width.toFloat() / width
            val scaleY = canvas.height.toFloat() / height
            val scale = minOf(scaleX, scaleY)
            val offsetX = (canvas.width - width * scale) / 2
            val offsetY = (canvas.height - height * scale) / 2

            canvas.drawColor(Color.BLACK)
            canvas.save()
            canvas.translate(offsetX, offsetY)
            canvas.scale(scale, scale)
            canvas.drawBitmap(frameBitmap!!, 0f, 0f, paint)
            canvas.restore()

            surface.unlockCanvasAndPost(canvas)
            frameCount++
        } catch (e: Exception) {
            Log.e(TAG, "Failed to render frame: ${e.message}")
        }
    }

    /**
     * Save a frame for debugging.
     */
    private fun saveDebugFrame(width: Int, height: Int, pixels: ByteArray) {
        try {
            val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))

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

        frameBitmap?.recycle()
        frameBitmap = null

        Log.i(TAG, "Frame socket server stopped")
    }

    fun isRunning(): Boolean = running.get()
}
