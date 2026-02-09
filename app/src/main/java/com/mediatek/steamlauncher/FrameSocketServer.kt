package com.mediatek.steamlauncher

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Choreographer
import android.view.Surface
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * TCP socket server for receiving Vulkan frames from the FEX container.
 *
 * Uses TCP on localhost instead of Unix sockets for FEX container compatibility.
 * Uses Choreographer for vsync-aligned frame display.
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
    private var receivedCount = 0L
    private var lastStatsTime = System.currentTimeMillis()

    // Latest frame buffer (triple buffering)
    private data class FrameData(val width: Int, val height: Int, val pixels: ByteArray)
    private val latestFrame = AtomicReference<FrameData?>(null)

    // Choreographer for vsync-aligned display
    private val handler = Handler(Looper.getMainLooper())
    private var choreographerCallback: Choreographer.FrameCallback? = null

    /**
     * Set the output surface for rendering frames.
     */
    fun setOutputSurface(surface: Surface?) {
        outputSurface = surface
        Log.i(TAG, "Output surface set: ${surface != null}")

        // Start/stop choreographer based on surface availability
        if (surface != null && running.get()) {
            startChoreographer()
        } else {
            stopChoreographer()
        }
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
     * Start choreographer for vsync-aligned rendering.
     */
    private fun startChoreographer() {
        if (choreographerCallback != null) return

        choreographerCallback = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (!running.get()) return

                // Render latest frame if available
                renderLatestFrame()

                // Schedule next frame
                if (running.get() && outputSurface != null) {
                    Choreographer.getInstance().postFrameCallback(this)
                }
            }
        }

        handler.post {
            Choreographer.getInstance().postFrameCallback(choreographerCallback!!)
        }
        Log.i(TAG, "Choreographer started")
    }

    /**
     * Stop choreographer.
     */
    private fun stopChoreographer() {
        choreographerCallback?.let {
            handler.post {
                Choreographer.getInstance().removeFrameCallback(it)
            }
        }
        choreographerCallback = null
        Log.i(TAG, "Choreographer stopped")
    }

    /**
     * Receive frames from client (just stores them, doesn't display).
     */
    private fun receiveFrames(socket: Socket) {
        Log.i(TAG, "Receiving frames from ${socket.inetAddress}")

        // Ensure choreographer is running if surface is available
        if (outputSurface != null) {
            handler.post { startChoreographer() }
        }

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

                // Store frame for choreographer to display (copy the buffer)
                val frameCopy = pixelBuffer.copyOf(pixelSize)
                latestFrame.set(FrameData(width, height, frameCopy))

                // Save first frame to file for debugging
                if (receivedCount == 0L) {
                    saveDebugFrame(width, height, pixelBuffer)
                }

                // Stats
                receivedCount++
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

        // Note: don't stop choreographer here — its lifecycle is managed by setOutputSurface().
        // Stopping it here races with new receiver threads and kills the choreographer prematurely.
        Log.i(TAG, "Frame receiver ended")
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

    // Reusable bitmap to avoid allocations
    private var frameBitmap: Bitmap? = null
    private var bitmapWidth = 0
    private var bitmapHeight = 0

    /**
     * Render the latest frame (called from Choreographer on main thread at vsync).
     */
    private fun renderLatestFrame() {
        // Get and clear the latest frame atomically
        val frame = latestFrame.getAndSet(null) ?: return

        val surface = outputSurface
        if (surface == null || !surface.isValid) {
            return
        }

        try {
            val width = frame.width
            val height = frame.height
            val pixels = frame.pixels

            // Reuse or create bitmap
            if (frameBitmap == null || bitmapWidth != width || bitmapHeight != height) {
                frameBitmap?.recycle()
                frameBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bitmapWidth = width
                bitmapHeight = height
                Log.i(TAG, "Created frame bitmap: ${width}x${height}")
            }

            // Copy pixels to bitmap
            frameBitmap!!.copyPixelsFromBuffer(ByteBuffer.wrap(pixels))

            // Draw to surface
            val canvas = surface.lockCanvas(null)
            if (canvas != null) {
                // Scale to fit surface
                val scaleX = canvas.width.toFloat() / width
                val scaleY = canvas.height.toFloat() / height
                val scale = minOf(scaleX, scaleY)

                // Center the image
                val offsetX = (canvas.width - width * scale) / 2
                val offsetY = (canvas.height - height * scale) / 2

                canvas.drawColor(android.graphics.Color.BLACK)
                canvas.save()
                canvas.translate(offsetX, offsetY)
                canvas.scale(scale, scale)
                canvas.drawBitmap(frameBitmap!!, 0f, 0f, paint)
                canvas.restore()

                surface.unlockCanvasAndPost(canvas)
                frameCount++
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to render frame: ${e.message}")
        }
    }

    /**
     * Stop the server.
     */
    fun stop() {
        Log.i(TAG, "Stopping frame socket server")
        running.set(false)

        stopChoreographer()

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
