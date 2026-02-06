package com.mediatek.steamlauncher

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.Build
import android.system.Os
import android.util.Log
import java.io.File

class SteamLauncherApp : Application() {

    companion object {
        const val TAG = "SteamLauncher"
        const val NOTIFICATION_CHANNEL_ID = "steam_service_channel"
        const val NOTIFICATION_CHANNEL_NAME = "Steam Container Service"

        /** FEX x86-64 rootfs name (matches fex-emu.gg download) */
        const val FEX_ROOTFS_NAME = "Ubuntu_22_04"

        lateinit var instance: SteamLauncherApp
            private set
    }

    val containerManager: ContainerManager by lazy { ContainerManager(this) }
    val fexExecutor: FexExecutor by lazy { FexExecutor(this) }

    override fun onCreate() {
        super.onCreate()
        instance = this

        // Set up X11 environment BEFORE loading native libs
        setupX11Environment()

        createNotificationChannel()

        Log.i(TAG, "SteamLauncher initialized")
    }

    /**
     * Set up environment variables for X11/libXlorie.
     */
    private fun setupX11Environment() {
        try {
            // XKB keyboard configuration path - use FEX rootfs if available, fallback to system
            val fexXkbPath = "${getFexRootfsDir()}/usr/share/X11/xkb"
            val xkbPath = if (File(fexXkbPath).exists()) fexXkbPath else "/usr/share/X11/xkb"
            Os.setenv("XKB_CONFIG_ROOT", xkbPath, true)
            Log.i(TAG, "XKB_CONFIG_ROOT=$xkbPath")

            // TMPDIR for X11 socket
            val tmpDir = getTmpDir()
            Os.setenv("TMPDIR", tmpDir, true)
            File(tmpDir).mkdirs()
            File(getX11SocketDir()).mkdirs()
            Log.i(TAG, "TMPDIR=$tmpDir")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set X11 environment", e)
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                NOTIFICATION_CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Keeps the Steam container running"
                setShowBadge(false)
                enableVibration(false)
                setSound(null, null)
            }
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }

    fun getAppDataPath(): String = filesDir.absolutePath

    /** FEX binary directory (FEXLoader, FEXInterpreter, bundled glibc) */
    fun getFexDir(): String = "${filesDir.absolutePath}/fex"

    /** Extracted x86-64 rootfs directory */
    fun getFexRootfsDir(): String = "${filesDir.absolutePath}/fex-rootfs/$FEX_ROOTFS_NAME"

    /** Home directory for FEX config and user data */
    fun getFexHomeDir(): String = "${filesDir.absolutePath}/fex-home"

    /** Temp directory on Android filesystem */
    fun getTmpDir(): String = "${cacheDir.absolutePath}/tmp"

    /** X11 socket directory */
    fun getX11SocketDir(): String = "${cacheDir.absolutePath}/tmp/.X11-unix"

    /** @deprecated Use getFexRootfsDir() instead. Kept for migration. */
    fun getRootfsDir(): String = "${filesDir.absolutePath}/rootfs"
}
