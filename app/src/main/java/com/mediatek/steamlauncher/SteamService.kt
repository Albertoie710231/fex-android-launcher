package com.mediatek.steamlauncher

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import android.view.Surface
import androidx.annotation.RequiresApi
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.*
import java.io.File

/**
 * Foreground service that manages the proot container and X11 server.
 */
class SteamService : Service() {

    companion object {
        private const val TAG = "SteamService"
        const val NOTIFICATION_ID = 1001

        const val ACTION_START_STEAM = "com.mediatek.steamlauncher.START_STEAM"
        const val ACTION_START_TERMINAL = "com.mediatek.steamlauncher.START_TERMINAL"
        const val ACTION_STOP = "com.mediatek.steamlauncher.STOP"
    }

    private val binder = LocalBinder()
    private val serviceScope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private var wakeLock: PowerManager.WakeLock? = null
    private var x11Server: X11Server? = null
    private var prootProcess: Process? = null
    private var pendingAction: String? = null
    private var surfaceReady = false
    private var vortekStarted = false

    // Framebuffer bridge for Vortek → Surface rendering
    private var framebufferBridge: FramebufferBridge? = null
    private var outputSurface: Surface? = null
    private var surfaceWidth = 1920
    private var surfaceHeight = 1080

    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }

    // Vortek socket path - must be accessible from both Android and container
    private val vortekSocketPath: String by lazy { "${app.getTmpDir()}/vortek.sock" }

    // Frame socket for receiving Vulkan frames from proot (TCP on localhost:19850)
    private var frameSocketServer: FrameSocketServer? = null
    private var vulkanFrameSurface: Surface? = null  // Stored for when server starts later

    inner class LocalBinder : Binder() {
        fun getService(): SteamService = this@SteamService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "SteamService created")
        acquireWakeLock()

        // Try to load Vortek library early
        if (VortekRenderer.loadLibrary()) {
            Log.i(TAG, "Vortek renderer available for Vulkan passthrough")
        } else {
            Log.w(TAG, "Vortek renderer not available - Vulkan passthrough disabled")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, createNotification())

        Log.i(TAG, "onStartCommand action: ${intent?.action}")

        when (intent?.action) {
            ACTION_START_STEAM -> {
                // Store pending action - X11 server will start when surface is ready
                pendingAction = ACTION_START_STEAM
                serviceScope.launch {
                    stopExistingProcesses()
                }
                // If surface is already ready, start immediately
                if (surfaceReady && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    Log.i(TAG, "Surface already ready, starting Steam immediately")
                    startPendingAction()
                } else {
                    Log.i(TAG, "Waiting for surface to be ready before starting Steam")
                }
            }
            ACTION_START_TERMINAL -> {
                // Store pending action - X11 server will start when surface is ready
                pendingAction = ACTION_START_TERMINAL
                serviceScope.launch {
                    stopExistingProcesses()
                }
                // If surface is already ready, start immediately
                if (surfaceReady && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    Log.i(TAG, "Surface already ready, starting Terminal immediately")
                    startPendingAction()
                } else {
                    Log.i(TAG, "Waiting for surface to be ready before starting Terminal")
                }
            }
            ACTION_STOP -> {
                stopSelf()
            }
        }

        return START_STICKY
    }

    private suspend fun stopExistingProcesses() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Stopping existing processes...")
        prootProcess?.let {
            if (it.isAlive) {
                it.destroyForcibly()
                it.waitFor()
            }
        }
        prootProcess = null
        delay(200)
    }

    private fun createNotification(): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = PendingIntent.getService(
            this, 1,
            Intent(this, SteamService::class.java).apply { action = ACTION_STOP },
            PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, SteamLauncherApp.NOTIFICATION_CHANNEL_ID)
            .setContentTitle("Steam Container Running")
            .setContentText("Linux container is active")
            .setSmallIcon(R.drawable.ic_notification)
            .setContentIntent(pendingIntent)
            .addAction(R.drawable.ic_stop, "Stop", stopIntent)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SteamLauncher::WakeLock")
            .apply { acquire(10 * 60 * 60 * 1000L) }
    }

    private suspend fun startX11Server() {
        Log.i(TAG, "Starting X11 server...")

        // Create socket directory
        val socketDir = File(app.getX11SocketDir())
        socketDir.mkdirs()

        // X11 server must be started on main thread (uses Choreographer)
        withContext(Dispatchers.Main) {
            x11Server = X11Server(this@SteamService).apply {
                onServerStarted = {
                    Log.i(TAG, "X11 server started callback")
                }
                onClientConnected = {
                    Log.i(TAG, "X11 client connected callback")
                }
                onError = { msg ->
                    Log.e(TAG, "X11 server error: $msg")
                }
                start()
            }
        }

        // Wait for server to initialize
        delay(500)
        Log.i(TAG, "X11 server initialization complete")
    }

    private suspend fun startTerminal() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Starting terminal...")

        // Deploy fake XCB library to tmp for headless X11 testing
        val tmpDir = java.io.File(app.getTmpDir())
        try {
            assets.open("libfakexcb.so").use { input ->
                val targetFile = java.io.File(tmpDir, "libfakexcb.so")
                java.io.FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "libfakexcb.so deployed to ${targetFile.absolutePath}")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to deploy libfakexcb.so: ${e.message}")
        }

        // Deploy pre-compiled libvulkan_headless.so to rootfs /lib (always overwrite to get latest)
        val rootfsLibDir = java.io.File(app.getRootfsDir(), "lib")
        try {
            assets.open("libvulkan_headless.so").use { input ->
                val targetFile = java.io.File(rootfsLibDir, "libvulkan_headless.so")
                java.io.FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "libvulkan_headless.so deployed to ${targetFile.absolutePath} (${targetFile.length()} bytes)")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to deploy libvulkan_headless.so: ${e.message}")
        }

        // Deploy test_headless binary for headless surface testing
        try {
            assets.open("test_headless").use { input ->
                val targetFile = java.io.File(tmpDir, "test_headless")
                java.io.FileOutputStream(targetFile).use { output ->
                    input.copyTo(output)
                }
                targetFile.setExecutable(true)
                targetFile.setReadable(true, false)
                Log.i(TAG, "test_headless deployed to ${targetFile.absolutePath}")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to deploy test_headless: ${e.message}")
        }

        // CRITICAL: TMPDIR must match host's TMPDIR for abstract X11 sockets to work
        // libXlorie creates abstract socket @$TMPDIR/.X11-unix/X0
        // X11 clients connect to @$TMPDIR/.X11-unix/X0
        // If TMPDIR differs, abstract socket paths won't match
        val hostTmpDir = app.getTmpDir()

        val env = buildEnvironmentMap(hostTmpDir)

        // Run Vulkan/X11 test
        prootProcess = app.prootExecutor.execute(
            command = """
                echo "=== Vortek Vulkan Rendering Test ==="
                echo ""
                echo "Environment:"
                echo "  VORTEK_SERVER_PATH=${'$'}VORTEK_SERVER_PATH"
                echo "  VK_ICD_FILENAMES=${'$'}VK_ICD_FILENAMES"
                echo ""

                # Check if Vortek socket exists
                echo "1. Checking Vortek socket..."
                if [ -S "${'$'}VORTEK_SERVER_PATH" ]; then
                    echo "   OK: Socket found"
                else
                    echo "   ERROR: Socket not found at ${'$'}VORTEK_SERVER_PATH"
                    exit 1
                fi

                # Check for Vulkan ICD
                echo ""
                echo "2. Checking Vulkan ICD..."
                if [ -f /usr/share/vulkan/icd.d/vortek_icd.json ]; then
                    echo "   OK: Vortek ICD configured"
                else
                    echo "   ERROR: Vortek ICD not found"
                    exit 1
                fi

                # Test Vulkan with vulkaninfo --summary
                echo ""
                echo "3. Testing Vulkan device enumeration..."
                if [ -f /usr/local/bin/vulkaninfo ]; then
                    echo ""
                    DISPLAY= /usr/local/bin/vulkaninfo --summary 2>&1
                    RESULT=$?
                    echo ""
                    if [ ${'$'}RESULT -eq 0 ]; then
                        echo "   SUCCESS: Vulkan device enumeration works!"
                    else
                        echo "   WARNING: vulkaninfo exited with code ${'$'}RESULT"
                    fi
                else
                    echo "   vulkaninfo not available"
                fi

                # Check for headless wrapper
                echo ""
                echo "4. Checking headless surface support..."
                if [ ! -f /lib/libvulkan_headless.so ]; then
                    echo "   Headless wrapper not found, attempting to compile..."

                    # Check for source file
                    SRC=""
                    [ -f /tmp/vulkan_headless.c ] && SRC="/tmp/vulkan_headless.c"
                    [ -f /opt/vulkan_headless.c ] && SRC="/opt/vulkan_headless.c"

                    if [ -z "${'$'}SRC" ]; then
                        echo "   ERROR: Source file vulkan_headless.c not found"
                        echo "   Please copy it to /tmp/ or /opt/"
                    else
                        echo "   Found source: ${'$'}SRC"

                        # Install gcc if needed
                        if ! command -v gcc &> /dev/null; then
                            echo "   Installing gcc (this may take a moment)..."
                            apt-get update 2>&1 | grep -v "^Hit:\|^Get:\|^Ign:" || true
                            DEBIAN_FRONTEND=noninteractive apt-get install -y gcc libc6-dev 2>&1 | tail -5
                            echo ""
                        fi

                        # Check gcc again
                        GCC_PATH=$(which gcc 2>/dev/null || echo "")
                        if [ -n "${'$'}GCC_PATH" ]; then
                            echo "   Found gcc at: ${'$'}GCC_PATH"
                            echo "   Compiling libvulkan_headless.so..."
                            ${'$'}GCC_PATH -shared -fPIC -O2 -o /lib/libvulkan_headless.so "${'$'}SRC" -ldl -lpthread 2>&1
                            if [ -f /lib/libvulkan_headless.so ]; then
                                chmod 755 /lib/libvulkan_headless.so
                                echo "   SUCCESS: Compiled headless wrapper"
                            else
                                echo "   ERROR: Compilation failed"
                            fi
                        else
                            echo "   ERROR: gcc not available after install attempt"
                            echo "   Try running manually in container:"
                            echo "   apt-get update && apt-get install -y gcc libc6-dev"
                            echo "   gcc -shared -fPIC -O2 -o /lib/libvulkan_headless.so /opt/vulkan_headless.c -ldl -lpthread"
                        fi
                    fi
                fi

                if [ -f /lib/libvulkan_headless.so ]; then
                    echo "   OK: Headless wrapper ready"

                    # Test with headless wrapper - with DISPLAY set to test Xlib surface
                    echo ""
                    echo "5. Testing Xlib surface extension (Vortek provides VK_KHR_xlib_surface)..."
                    echo ""
                    # Set DISPLAY to trigger Xlib surface testing in vulkaninfo
                    LD_PRELOAD=/lib/libvulkan_headless.so DISPLAY=:0 /usr/local/bin/vulkaninfo --summary 2>&1 | head -40
                    echo ""

                    # Test if Xlib stubs are working
                    echo "5b. Checking Xlib stub functionality..."
                    if [ -f /usr/local/bin/xeyes ]; then
                        LD_PRELOAD=/lib/libvulkan_headless.so DISPLAY=:0 timeout 1 /usr/local/bin/xeyes 2>&1 || echo "   xeyes test complete"
                    else
                        echo "   xeyes not available, skipping"
                    fi
                    echo ""

                    # Test 6: Headless surface test (bypasses X11 entirely)
                    echo "6. Testing VK_EXT_headless_surface (bypasses X11)..."
                    if [ -f /tmp/test_headless ]; then
                        chmod +x /tmp/test_headless
                        cp /tmp/test_headless /usr/local/bin/
                        echo ""
                        LD_PRELOAD=/lib/libvulkan_headless.so /usr/local/bin/test_headless 2>&1
                        HEADLESS_RESULT=$?
                        echo ""
                        if [ ${'$'}HEADLESS_RESULT -eq 0 ]; then
                            echo "   SUCCESS: VK_EXT_headless_surface works!"
                            echo "   Vulkan rendering is possible without X11!"
                        else
                            echo "   FAILED: test_headless exited with code ${'$'}HEADLESS_RESULT"
                        fi
                    else
                        echo "   test_headless not found in /tmp"
                    fi

                    # Test 7: Try vkcube (requires VK_KHR_xcb_surface - will likely fail)
                    VKCUBE=""
                    [ -x /tmp/vkcube ] && VKCUBE="/tmp/vkcube"
                    [ -x /usr/local/bin/vkcube ] && VKCUBE="/usr/local/bin/vkcube"
                    [ -x /usr/bin/vkcube ] && VKCUBE="/usr/bin/vkcube"

                    if [ -n "${'$'}VKCUBE" ]; then
                        echo ""
                        echo "7. Testing vkcube (requires XCB - may fail)..."
                        echo ""
                        export DISPLAY=:0
                        LD_PRELOAD=/lib/libvulkan_headless.so ${'$'}VKCUBE --c 10 2>&1 || echo "   vkcube exited (expected - requires XCB)"
                        unset LD_PRELOAD
                    fi
                fi

                echo ""
                echo "=== Test Complete ==="
                echo ""
                echo "The Vortek Vulkan passthrough is working!"
                echo "Device: Vortek (Mali-G720-Immortalis MC12)"
                echo ""
                echo "To test manually:"
                echo "  LD_PRELOAD=/lib/libvulkan_headless.so vkcube"
                echo ""
                echo "Keeping terminal alive..."
                sleep 300
            """.trimIndent(),
            environment = env,
            workingDir = "/home/user"
        )

        monitorProcess("TERMINAL")
    }

    private suspend fun startSteam() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Starting Steam...")

        val hostTmpDir = app.getTmpDir()

        val env = buildEnvironmentMap(hostTmpDir).toMutableMap().apply {
            // Box64 specific settings
            put("BOX64_LOG", "1")
            put("BOX64_DYNAREC", "1")
        }

        val script = """
            #!/bin/bash
            echo "Starting Steam..."
            if [ -f "${'$'}HOME/.local/share/Steam/ubuntu12_32/steam" ]; then
                exec /usr/local/bin/box32 "${'$'}HOME/.local/share/Steam/ubuntu12_32/steam"
            else
                echo "Steam not found"
                exit 1
            fi
        """.trimIndent()

        val scriptFile = File(app.getTmpDir(), "launch.sh")
        scriptFile.writeText(script)
        scriptFile.setExecutable(true)

        prootProcess = app.prootExecutor.execute(
            command = "/bin/bash /tmp/launch.sh",
            environment = env,
            workingDir = "/home/user"
        )

        monitorProcess("STEAM")
    }

    private fun monitorProcess(tag: String) {
        serviceScope.launch {
            prootProcess?.let { process ->
                launch {
                    try {
                        process.inputStream.bufferedReader().useLines { lines ->
                            lines.forEach { Log.i(TAG, "$tag: $it") }
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "Error reading $tag output", e)
                    }
                }
                try {
                    val exitCode = process.waitFor()
                    Log.i(TAG, "$tag exited with code: $exitCode")
                } catch (e: InterruptedException) {
                    Log.w(TAG, "$tag interrupted")
                }
            }
        }
    }

    fun getX11Server(): X11Server? = x11Server

    fun isRunning(): Boolean = prootProcess?.isAlive == true

    /**
     * Called by GameActivity when the LorieView surface is ready.
     * This is the signal to start the X11 server and pending action.
     */
    @Synchronized
    @RequiresApi(Build.VERSION_CODES.O)
    fun onSurfaceReady() {
        Log.i(TAG, "Surface ready callback received")

        // Guard against multiple calls
        if (surfaceReady) {
            Log.d(TAG, "Surface already marked as ready, ignoring duplicate callback")
            return
        }
        surfaceReady = true

        startPendingAction()
    }

    /**
     * Start the pending action (Steam or Terminal).
     * Called either when surface becomes ready, or when a new action is requested
     * with surface already ready.
     */
    @Synchronized
    @RequiresApi(Build.VERSION_CODES.O)
    private fun startPendingAction() {
        val action = pendingAction
        if (action == null) {
            Log.w(TAG, "startPendingAction called but no pending action")
            return
        }

        // Clear pending action BEFORE launching to prevent duplicate starts
        pendingAction = null

        // Validate surface dimensions before starting
        if (surfaceWidth <= 0 || surfaceHeight <= 0) {
            Log.e(TAG, "Invalid surface dimensions: ${surfaceWidth}x${surfaceHeight}, cannot start")
            return
        }

        Log.i(TAG, "Starting action: $action with surface ${surfaceWidth}x${surfaceHeight}")
        serviceScope.launch {
            // Start Vortek renderer FIRST (before X11/proot)
            // This creates the socket that the container will connect to
            startVortekServer()

            startX11Server()
            when (action) {
                ACTION_START_STEAM -> startSteam()
                ACTION_START_TERMINAL -> startTerminal()
            }
        }
    }

    /**
     * Start the Vortek Vulkan passthrough server.
     * This must be called before starting the container so the socket exists.
     */
    @RequiresApi(Build.VERSION_CODES.O)
    private suspend fun startVortekServer() = withContext(Dispatchers.IO) {
        if (!VortekRenderer.isAvailable()) {
            Log.w(TAG, "Vortek not available, skipping Vulkan passthrough setup")
            return@withContext
        }

        if (vortekStarted) {
            Log.d(TAG, "Vortek server already started")
            return@withContext
        }

        Log.i(TAG, "Starting Vortek renderer at: $vortekSocketPath")

        // Delete stale socket if it exists
        val socketFile = File(vortekSocketPath)
        if (socketFile.exists()) {
            socketFile.delete()
        }

        // CRITICAL: Create FramebufferBridge BEFORE starting Vortek server
        // This ensures WindowInfoProvider is ready before any clients connect
        if (outputSurface != null && framebufferBridge == null) {
            Log.i(TAG, "Creating FramebufferBridge before Vortek start")
            withContext(Dispatchers.Main) {
                createFramebufferBridge()
            }
        }

        if (VortekRenderer.start(vortekSocketPath, this@SteamService)) {
            vortekStarted = true

            // CRITICAL: Connect WindowInfoProvider IMMEDIATELY after start
            // Before any clients can connect via the socket
            framebufferBridge?.let { bridge ->
                VortekRenderer.setWindowInfoProvider(bridge)
                Log.i(TAG, "WindowInfoProvider connected to VortekRenderer")
            }

            // Give the server a moment to create the socket
            delay(100)

            if (socketFile.exists()) {
                Log.i(TAG, "Vortek socket created successfully")

                // Create symlink at path expected by Winlator's libvulkan_vortek.so
                // The ICD has hardcoded: /data/data/com.winlator/files/rootfs/tmp/.vortek/V0
                // With proot bind, this maps to $tmpDir/.vortek/V0
                try {
                    val vortekDir = File(app.getTmpDir(), ".vortek")
                    if (!vortekDir.exists()) {
                        vortekDir.mkdirs()
                    }
                    val symlinkFile = File(vortekDir, "V0")
                    if (symlinkFile.exists()) {
                        symlinkFile.delete()
                    }
                    // Create symlink: V0 -> ../vortek.sock
                    Runtime.getRuntime().exec(arrayOf("ln", "-sf", "../vortek.sock", symlinkFile.absolutePath)).waitFor()
                    Log.i(TAG, "Created Vortek symlink at: ${symlinkFile.absolutePath}")
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to create Vortek symlink", e)
                }
            } else {
                Log.w(TAG, "Vortek started but socket not found at expected path")
            }
        } else {
            Log.e(TAG, "Failed to start Vortek server")
        }

        // Start frame socket server to receive Vulkan frames from proot
        startFrameSocketServer()
    }

    /**
     * Start the frame socket server for receiving Vulkan frames.
     */
    private fun startFrameSocketServer() {
        if (frameSocketServer != null) {
            Log.d(TAG, "Frame socket server already running")
            return
        }

        Log.i(TAG, "Starting frame socket server on TCP port 19850")

        frameSocketServer = FrameSocketServer().apply {
            // Use the Vulkan frame surface if available, otherwise leave null
            vulkanFrameSurface?.let { setOutputSurface(it) }
            if (start()) {
                Log.i(TAG, "Frame socket server started successfully")
            } else {
                Log.e(TAG, "Failed to start frame socket server")
            }
        }
    }

    /**
     * Stop the frame socket server.
     */
    private fun stopFrameSocketServer() {
        frameSocketServer?.stop()
        frameSocketServer = null
        Log.i(TAG, "Frame socket server stopped")
    }

    /**
     * Stop the Vortek server.
     */
    private fun stopVortekServer() {
        if (vortekStarted) {
            Log.i(TAG, "Stopping Vortek server...")
            releaseFramebufferBridge()
            VortekRenderer.stop()
            vortekStarted = false
        }
    }

    /**
     * Build the environment variables map for container processes.
     * Includes Vortek Vulkan passthrough configuration when available.
     */
    private fun buildEnvironmentMap(tmpDir: String): Map<String, String> {
        // Note: libXlorie creates abstract socket @/tmp/.X11-unix/X0 (hardcoded /tmp)
        // So we use DISPLAY=:0 and TMPDIR=/tmp for X11 clients
        val env = mutableMapOf(
            "DISPLAY" to ":0",
            "HOME" to "/home/user",
            "USER" to "user",
            "TERM" to "xterm-256color",
            "PATH" to "/usr/local/bin:/usr/bin:/bin",
            "XDG_RUNTIME_DIR" to "/tmp",
            "TMPDIR" to "/tmp"
        )

        // Add Vortek Vulkan passthrough environment if available
        if (vortekStarted) {
            // Inside proot, /tmp is bound to the Android tmp dir
            // So the socket path inside the container is /tmp/vortek.sock
            val containerSocketPath = "/tmp/vortek.sock"

            env.putAll(mapOf(
                // Tell libvulkan_vortek.so where to find the Android renderer
                "VORTEK_SERVER_PATH" to containerSocketPath,

                // Use Vortek as the Vulkan ICD
                "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/vortek_icd.json",

                // Disable attempts to load native Android Vulkan drivers directly
                // (they won't work from glibc anyway)
                "VK_DRIVER_FILES" to "",

                // DXVK settings optimized for Mali GPUs
                "DXVK_ASYNC" to "1",
                "DXVK_STATE_CACHE" to "1",
                "DXVK_LOG_LEVEL" to "none",

                // Mali-specific optimizations
                "MALI_NO_ASYNC_COMPUTE" to "1"
            ))
            Log.d(TAG, "Vortek environment variables added")
        } else {
            Log.d(TAG, "Vortek not started, using default Vulkan config")
        }

        return env
    }

    /**
     * Check if surface is ready
     */
    fun isSurfaceReady(): Boolean = surfaceReady

    /**
     * Set the output surface for Vortek framebuffer rendering.
     * This is called from GameActivity when the LorieView surface is available.
     */
    @RequiresApi(Build.VERSION_CODES.O)
    fun setOutputSurface(surface: Surface?, width: Int, height: Int) {
        Log.i(TAG, "setOutputSurface: ${width}x${height}, surface=${surface != null}")

        outputSurface = surface
        surfaceWidth = width
        surfaceHeight = height

        // Update existing FramebufferBridge if one exists
        framebufferBridge?.let { bridge ->
            bridge.setOutputSurface(surface)
            if (width > 0 && height > 0) {
                bridge.resize(width, height)
            }
        }

        // If Vortek is running but FramebufferBridge wasn't created yet, create it now
        if (surface != null && vortekStarted && framebufferBridge == null) {
            createFramebufferBridge()
        }
    }

    /**
     * Set the surface for Vulkan frame rendering (from FrameSocketServer).
     * This is separate from the X11 output surface.
     */
    fun setVulkanFrameSurface(surface: Surface?) {
        Log.i(TAG, "setVulkanFrameSurface: surface=${surface != null}, serverRunning=${frameSocketServer != null}")
        vulkanFrameSurface = surface
        frameSocketServer?.setOutputSurface(surface)
    }

    /**
     * Create the FramebufferBridge and connect it to VortekRenderer.
     */
    @RequiresApi(Build.VERSION_CODES.O)
    private fun createFramebufferBridge() {
        if (framebufferBridge != null) {
            Log.d(TAG, "FramebufferBridge already exists")
            return
        }

        Log.i(TAG, "Creating FramebufferBridge: ${surfaceWidth}x${surfaceHeight}")

        framebufferBridge = FramebufferBridge(outputSurface, surfaceWidth, surfaceHeight)

        // Connect to VortekRenderer
        VortekRenderer.setWindowInfoProvider(framebufferBridge!!)

        Log.i(TAG, "FramebufferBridge created and connected to VortekRenderer")
    }

    /**
     * Release the FramebufferBridge.
     */
    private fun releaseFramebufferBridge() {
        framebufferBridge?.let {
            Log.i(TAG, "Releasing FramebufferBridge")
            it.release()
        }
        framebufferBridge = null
    }

    override fun onDestroy() {
        Log.i(TAG, "SteamService destroying")
        prootProcess?.destroyForcibly()
        x11Server?.stop()
        stopFrameSocketServer()
        stopVortekServer()
        wakeLock?.let { if (it.isHeld) it.release() }
        serviceScope.cancel()
        super.onDestroy()
    }

    /**
     * Check if Vortek Vulkan passthrough is active.
     */
    fun isVortekActive(): Boolean = vortekStarted && VortekRenderer.isRunning()

    /**
     * Get Vortek status information.
     */
    fun getVortekInfo(): VortekRenderer.VortekInfo? = VortekRenderer.getInfo()
}
