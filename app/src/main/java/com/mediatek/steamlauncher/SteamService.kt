package com.mediatek.steamlauncher

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
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

    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }

    inner class LocalBinder : Binder() {
        fun getService(): SteamService = this@SteamService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "SteamService created")
        acquireWakeLock()
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
                if (surfaceReady) {
                    Log.i(TAG, "Surface already ready, starting Steam immediately")
                    onSurfaceReady()
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
                if (surfaceReady) {
                    Log.i(TAG, "Surface already ready, starting Terminal immediately")
                    onSurfaceReady()
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

        // CRITICAL: TMPDIR must match host's TMPDIR for abstract X11 sockets to work
        // libXlorie creates abstract socket @$TMPDIR/.X11-unix/X0
        // X11 clients connect to @$TMPDIR/.X11-unix/X0
        // If TMPDIR differs, abstract socket paths won't match
        val hostTmpDir = app.getTmpDir()

        val env = mapOf(
            "DISPLAY" to ":0",
            "HOME" to "/home/user",
            "USER" to "user",
            "TERM" to "xterm-256color",
            "PATH" to "/usr/local/bin:/usr/bin:/bin",
            "XDG_RUNTIME_DIR" to hostTmpDir,
            "TMPDIR" to hostTmpDir
        )

        // Test X11 connection - check socket types
        prootProcess = app.prootExecutor.execute(
            command = """
                echo "=== X11 Connection Test ==="
                echo "DISPLAY=${'$'}DISPLAY"
                echo ""
                echo "Checking Unix sockets..."
                ls -la /tmp/.X11-unix/ 2>/dev/null || echo "No /tmp/.X11-unix"
                ls -la ${'$'}TMPDIR/.X11-unix/ 2>/dev/null || echo "No TMPDIR/.X11-unix"
                echo ""
                echo "Checking abstract sockets (via /proc/net/unix)..."
                cat /proc/net/unix 2>/dev/null | grep X11 | head -5 || echo "Cannot read /proc/net/unix"
                echo ""
                echo "Trying TCP connection to localhost:6000..."
                timeout 2 bash -c 'echo | nc -v localhost 6000' 2>&1 || echo "TCP connection failed"
                echo ""
                echo "Process staying alive..."
                sleep 120
            """.trimIndent(),
            environment = env,
            workingDir = "/home/user"
        )

        monitorProcess("TERMINAL")
    }

    private suspend fun startSteam() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Starting Steam...")

        val hostTmpDir = app.getTmpDir()

        val env = mapOf(
            "DISPLAY" to ":0",
            "HOME" to "/home/user",
            "USER" to "user",
            "PATH" to "/usr/local/bin:/usr/bin:/bin",
            "XDG_RUNTIME_DIR" to hostTmpDir,
            "TMPDIR" to hostTmpDir,
            "BOX64_LOG" to "1",
            "BOX64_DYNAREC" to "1"
        )

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
    fun onSurfaceReady() {
        Log.i(TAG, "Surface ready callback received")

        // Guard against multiple calls
        if (surfaceReady) {
            Log.d(TAG, "Surface already marked as ready, ignoring duplicate callback")
            return
        }
        surfaceReady = true

        val action = pendingAction
        if (action == null) {
            Log.w(TAG, "Surface ready but no pending action")
            return
        }

        // Clear pending action BEFORE launching to prevent duplicate starts
        pendingAction = null

        Log.i(TAG, "Surface ready, starting X11 server for action: $action")
        serviceScope.launch {
            startX11Server()
            when (action) {
                ACTION_START_STEAM -> startSteam()
                ACTION_START_TERMINAL -> startTerminal()
            }
        }
    }

    /**
     * Check if surface is ready
     */
    fun isSurfaceReady(): Boolean = surfaceReady

    override fun onDestroy() {
        Log.i(TAG, "SteamService destroying")
        prootProcess?.destroyForcibly()
        x11Server?.stop()
        wakeLock?.let { if (it.isHeld) it.release() }
        serviceScope.cancel()
        super.onDestroy()
    }
}
