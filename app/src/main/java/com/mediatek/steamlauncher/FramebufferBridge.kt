package com.mediatek.steamlauncher

import android.view.Surface

/**
 * RETIRED during XConnector port (2026-04-23). Placeholder — legacy API only.
 */
class FramebufferBridge(
    @Suppress("UNUSED_PARAMETER") outputSurface: Surface?,
    @Suppress("UNUSED_PARAMETER") width: Int,
    @Suppress("UNUSED_PARAMETER") height: Int,
) {
    fun setOutputSurface(@Suppress("UNUSED_PARAMETER") s: Surface?) {}
    fun resize(@Suppress("UNUSED_PARAMETER") w: Int, @Suppress("UNUSED_PARAMETER") h: Int) {}
    fun release() {}
}
