package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import au.com.darkside.xserver.XServer

/**
 * Darkside-xserver-based X11 server. Replaces the libXlorie-backed
 * X11Server.kt because libXlorie never emits FocusIn events in our
 * headless pipeline — without FocusIn, wine can't dispatch
 * WM_ACTIVATEAPP to the game's message pump, and Ys IX parks forever
 * after the first present.
 *
 * Darkside's XServer is a pure-Java X11 implementation with full focus
 * event handling (see Window.java:1109 EventCode.sendFocusIn). Runs on
 * a TCP listener; wine connects via DISPLAY=127.0.0.1:0 rather than
 * the abstract unix socket.
 *
 * We don't need rendering — our DXVK-intercept Vulkan layer delivers
 * frames to FrameSocketServer independently. Darkside's built-in
 * ScreenView is created but unused.
 */
class DarksideX11Server(private val context: Context) {

    companion object {
        private const val TAG = "DarksideX11Server"
        /** X11 port 6000 = display :0. */
        private const val PORT = 6000
    }

    private var xServer: XServer? = null
    private var isRunning = false

    fun start(): Boolean {
        if (isRunning) return true
        return try {
            val srv = XServer(context, PORT, null)
            // Must initialize _rootWindow BEFORE listening for clients — the
            // protocol handshake writes screen info (ScreenView.write L1102)
            // which dereferences _rootWindow. In non-headless mode this would
            // happen in the View's onSizeChanged callback; here we call the
            // public entry point we exposed on ScreenView directly.
            srv.screen.initRootWindow(1920, 1080)
            if (!srv.start()) {
                Log.e(TAG, "XServer.start() returned false")
                return false
            }
            xServer = srv
            isRunning = true
            Log.i(TAG, "Darkside X server listening on TCP :$PORT (display :0)")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to start Darkside X server", t)
            false
        }
    }

    fun stop() {
        if (!isRunning) return
        try {
            xServer?.stop()
        } catch (_: Throwable) {}
        xServer = null
        isRunning = false
    }

    fun isRunning(): Boolean = isRunning
}
