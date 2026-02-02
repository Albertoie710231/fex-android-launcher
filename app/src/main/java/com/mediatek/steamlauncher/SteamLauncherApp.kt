package com.mediatek.steamlauncher

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.Build
import android.util.Log

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

        createNotificationChannel()
        initializeNativeLibraries()

        Log.i(TAG, "SteamLauncher application initialized")
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

            val notificationManager = getSystemService(NotificationManager::class.java)
            notificationManager.createNotificationChannel(channel)
        }
    }

    private fun initializeNativeLibraries() {
        try {
            System.loadLibrary("steamlauncher")
            Log.i(TAG, "Native libraries loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load native libraries: ${e.message}")
        }
    }

    fun getAppDataPath(): String = filesDir.absolutePath
    fun getRootfsDir(): String = "${filesDir.absolutePath}/rootfs"
    fun getTmpDir(): String = "${cacheDir.absolutePath}/tmp"
    fun getX11SocketDir(): String = "${cacheDir.absolutePath}/tmp/.X11-unix"
}
