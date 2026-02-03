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
 * Foreground service that manages the proot container lifecycle.
 * This service is critical for Android 12+ to prevent phantom process killing.
 *
 * The service maintains:
 * - PRoot process for Linux container
 * - X11 server (Lorie) for display
 * - Steam client process
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
    private var steamProcess: Process? = null

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

        when (intent?.action) {
            ACTION_START_STEAM -> {
                serviceScope.launch {
                    startX11Server()
                    startSteam()
                }
            }
            ACTION_START_TERMINAL -> {
                serviceScope.launch {
                    startX11Server()
                    startTerminal()
                }
            }
            ACTION_STOP -> {
                stopSelf()
            }
        }

        return START_STICKY
    }

    private fun createNotification(): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = PendingIntent.getService(
            this,
            1,
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
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    private fun acquireWakeLock() {
        val powerManager = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "SteamLauncher::ContainerWakeLock"
        ).apply {
            acquire(10 * 60 * 60 * 1000L) // 10 hours max
        }
    }

    private suspend fun startX11Server() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Starting X11 server")

        // Create X11 socket directory
        val x11Dir = File(app.getX11SocketDir())
        if (!x11Dir.exists()) {
            x11Dir.mkdirs()
        }

        x11Server = X11Server(this@SteamService).apply {
            start()
        }

        // Wait for X11 server to be ready
        delay(500)
        Log.i(TAG, "X11 server started")
    }

    private suspend fun startSteam() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Starting Steam via proot")

        val rootfsPath = app.getRootfsDir()
        val env = buildSteamEnvironment()
        val script = buildSteamLaunchScript()

        // Write launch script
        val scriptFile = File(rootfsPath, "tmp/launch_steam.sh")
        scriptFile.parentFile?.mkdirs()
        scriptFile.writeText(script)
        scriptFile.setExecutable(true)

        prootProcess = app.prootExecutor.execute(
            command = "/bin/bash /tmp/launch_steam.sh",
            environment = env,
            workingDir = "/home/user"
        )

        // Monitor the process
        serviceScope.launch {
            prootProcess?.let { process ->
                try {
                    val exitCode = process.waitFor()
                    Log.i(TAG, "Steam process exited with code: $exitCode")
                } catch (e: InterruptedException) {
                    Log.w(TAG, "Steam process interrupted")
                }
            }
        }
    }

    private suspend fun startTerminal() = withContext(Dispatchers.IO) {
        Log.i(TAG, "Starting terminal via proot")

        val env = buildTerminalEnvironment()

        prootProcess = app.prootExecutor.execute(
            command = "/bin/bash",
            environment = env,
            workingDir = "/home/user"
        )
    }

    private fun buildSteamEnvironment(): Map<String, String> {
        return mapOf(
            "DISPLAY" to ":0",
            "PULSE_SERVER" to "tcp:127.0.0.1:4713",

            // Box64/Box32 configuration (Box64 compiled with BOX32=ON)
            "BOX64_LOG" to "1",
            "BOX64_LD_LIBRARY_PATH" to "/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu",
            "BOX64_DYNAREC" to "1",

            // Steam configuration - use host libraries via Box32
            "STEAM_RUNTIME" to "0",
            "LD_LIBRARY_PATH" to "/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu",

            // Vulkan configuration
            "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/android_icd.json",
            "MESA_VK_WSI_PRESENT_MODE" to "fifo",

            // Proton/Wine configuration
            "PROTON_USE_WINED3D" to "0",
            "DXVK_ASYNC" to "1",

            // Fix common crashes
            "LD_PRELOAD" to "",

            // Paths
            "HOME" to "/home/user",
            "USER" to "user",
            "XDG_RUNTIME_DIR" to "/tmp",
            "TMPDIR" to "/tmp",
            "PATH" to "/usr/local/bin:/usr/bin:/bin"
        )
    }

    private fun buildTerminalEnvironment(): Map<String, String> {
        return mapOf(
            "DISPLAY" to ":0",
            "HOME" to "/home/user",
            "USER" to "user",
            "TERM" to "xterm-256color",
            "LANG" to "en_US.UTF-8",
            "PATH" to "/usr/local/bin:/usr/bin:/bin",

            // Box64/Box32 configuration
            "BOX64_LOG" to "1",
            "BOX64_LD_LIBRARY_PATH" to "/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu",
            "LD_LIBRARY_PATH" to "/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu"
        )
    }

    private fun buildSteamLaunchScript(): String {
        return """
            #!/bin/bash

            echo "=== Steam Launch Script ==="
            echo "Date: $(date)"
            echo "User: ${'$'}USER"
            echo "Display: ${'$'}DISPLAY"

            # Verify X11 is accessible
            if [ ! -S /tmp/.X11-unix/X0 ]; then
                echo "Warning: X11 socket not found at /tmp/.X11-unix/X0"
            fi

            # Verify Box64/Box32 are available
            echo "Checking Box64..."
            box64 --version 2>&1 || echo "Warning: box64 not available"

            # Check Steam installation - try multiple locations
            STEAM_BIN=""
            STEAM_DIR=""

            # First try the extracted bootstrap location
            if [ -f "${'$'}HOME/.local/share/Steam/ubuntu12_32/steam" ]; then
                STEAM_BIN="${'$'}HOME/.local/share/Steam/ubuntu12_32/steam"
                STEAM_DIR="${'$'}HOME/.local/share/Steam"
            elif [ -f "${'$'}HOME/.local/share/Steam/steam.sh" ]; then
                STEAM_BIN="${'$'}HOME/.local/share/Steam/steam.sh"
                STEAM_DIR="${'$'}HOME/.local/share/Steam"
            elif [ -f "${'$'}HOME/usr/lib/steam/bin_steam.sh" ]; then
                STEAM_BIN="${'$'}HOME/usr/lib/steam/bin_steam.sh"
                STEAM_DIR="${'$'}HOME/usr/lib/steam"
            fi

            if [ -z "${'$'}STEAM_BIN" ]; then
                echo "ERROR: Steam not found. Please install Steam first."
                echo "Searched locations:"
                echo "  - ~/.local/share/Steam/ubuntu12_32/steam"
                echo "  - ~/.local/share/Steam/steam.sh"
                echo "  - ~/usr/lib/steam/bin_steam.sh"
                exit 1
            fi

            echo "Found Steam at: ${'$'}STEAM_BIN"
            echo "Steam directory: ${'$'}STEAM_DIR"

            # Clean any stale lock files
            rm -f "${'$'}HOME/.steam/steam.pid" 2>/dev/null
            rm -f "${'$'}STEAM_DIR/.crash" 2>/dev/null

            echo "Starting Steam..."
            cd "${'$'}STEAM_DIR"

            # Launch Steam binary directly with Box32
            # Box32 = Box64 compiled with BOX32 support for 32-bit x86
            exec box32 "${'$'}STEAM_BIN" "${'$'}@" 2>&1
        """.trimIndent()
    }

    fun getX11Server(): X11Server? = x11Server

    fun isRunning(): Boolean = prootProcess?.isAlive == true

    fun sendCommand(command: String) {
        prootProcess?.outputStream?.let { output ->
            output.write("$command\n".toByteArray())
            output.flush()
        }
    }

    override fun onDestroy() {
        Log.i(TAG, "SteamService destroying")

        // Clean up processes
        steamProcess?.destroyForcibly()
        prootProcess?.destroyForcibly()
        x11Server?.stop()

        // Release wake lock
        wakeLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }

        serviceScope.cancel()
        super.onDestroy()
    }
}
