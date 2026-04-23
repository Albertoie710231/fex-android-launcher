package com.mediatek.steamlauncher

import android.content.Context

/**
 * RETIRED during XConnector port (2026-04-23).
 *
 * The old VortekRenderer was a Kotlin shim around an earlier partial-port
 * of Winlator's VortekRendererComponent. The full Winlator X server stack
 * is now imported — VortekRendererComponent integrates with XServer/
 * XServerView directly. This object remains only as an API shim so legacy
 * SteamService.kt call-sites compile; all methods are no-ops.
 */
object VortekRenderer {
    data class VortekInfo(val running: Boolean = false, val socketPath: String = "")
    fun loadLibrary(): Boolean = false
    fun isAvailable(): Boolean = false
    fun isRunning(): Boolean = false
    fun start(socketPath: String, context: Context): Boolean = false
    fun stop() {}
    fun setWindowInfoProvider(bridge: FramebufferBridge) {}
    fun getInfo(): VortekInfo = VortekInfo()
}
