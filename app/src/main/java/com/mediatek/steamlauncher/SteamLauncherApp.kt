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

        lateinit var instance: SteamLauncherApp
            private set
    }

    val containerManager: ContainerManager by lazy { ContainerManager(this) }
    val prootExecutor: ProotExecutor by lazy { ProotExecutor(this) }

    override fun onCreate() {
        super.onCreate()
        instance = this

        // Set up X11 environment BEFORE loading native libs
        setupX11Environment()

        createNotificationChannel()
        ensureTallocSymlink()

        Log.i(TAG, "SteamLauncher initialized")
    }

    /**
     * Set up environment variables for X11/libXlorie.
     */
    private fun setupX11Environment() {
        try {
            // XKB keyboard configuration path
            val xkbPath = "${filesDir.absolutePath}/rootfs/usr/share/X11/xkb"
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

    private fun ensureTallocSymlink() {
        val nativeLibDir = applicationInfo.nativeLibraryDir
        val libOverrideDir = File(filesDir, "lib-override").also { it.mkdirs() }
        val tallocSource = File(nativeLibDir, "libtalloc.so")
        val tallocDest = File(libOverrideDir, "libtalloc.so.2")

        try {
            tallocDest.delete()
            if (tallocSource.exists()) {
                tallocSource.copyTo(tallocDest, overwrite = true)
                tallocDest.setExecutable(true, false)
                tallocDest.setReadable(true, false)
                Log.i(TAG, "libtalloc.so.2 ready")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set up libtalloc", e)
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
    fun getRootfsDir(): String = "${filesDir.absolutePath}/rootfs"
    fun getTmpDir(): String = "${cacheDir.absolutePath}/tmp"
    fun getX11SocketDir(): String = "${cacheDir.absolutePath}/tmp/.X11-unix"
}
