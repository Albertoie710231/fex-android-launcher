package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import com.winlator.xconnector.UnixSocketConfig
import com.winlator.xenvironment.components.XServerComponent
import com.winlator.xenvironment.components.VortekRendererComponent
import com.winlator.xserver.ScreenInfo
import com.winlator.xserver.XServer
import com.winlator.winhandler.WinHandler

/**
 * Winlator XConnector-based X11 server.
 *
 * Replacement for DarksideX11Server. Wraps com.winlator.xserver.XServer
 * (a full Java X11 server with DRI3 + Present + MITSHM + Sync + XInput2
 * extensions implemented) and com.winlator.xconnector.XConnectorEpoll
 * (unix-socket listener with epoll event loop and ancillary-FD support
 * via libxconnectorpatch.so).
 *
 * Wine connects via DISPLAY=:0 which resolves to the unix socket
 * `$socketRoot/tmp/.X11-unix/X0`. Unlike Darkside (which listened on
 * TCP :6000 because Java can't receive SCM_RIGHTS ancillary data), this
 * server listens on a unix socket and can receive GPU buffer fds that
 * DRI3 PixmapFromBuffer needs.
 *
 * Lifecycle: start() creates the socket + X server + starts the epoll
 * thread. stop() tears down and unlinks the socket.
 */
class XConnectorX11Server(
    private val context: Context,
    width: Int = 1920,
    height: Int = 1080,
) {
    companion object { private const val TAG = "XConnectorX11Server" }

    private val screenInfo = ScreenInfo(width, height)
    val xServer: XServer = XServer(screenInfo).apply {
        // DesktopHelper's MapWindow listener calls xServer.getWinHandler()
        // and dereferences it; our stubbed WinHandler never actually does
        // anything but has to exist so the reference isn't null.
        setWinHandler(WinHandler())
    }

    private var socketConfig: UnixSocketConfig? = null
    private var component: XServerComponent? = null
    private var vortekComponent: VortekRendererComponent? = null
    private var running = false

    /** Root directory for unix sockets — we write the X11 socket to
     *  `$socketRoot/tmp/.X11-unix/X0`. Wine reads DISPLAY=:0 and
     *  looks for `/tmp/.X11-unix/X0` via the path-redirect shim. */
    fun start(socketRoot: String): Boolean {
        if (running) return true
        return try {
            val cfg = UnixSocketConfig.createSocket(socketRoot, UnixSocketConfig.XSERVER_PATH)
            socketConfig = cfg
            val comp = XServerComponent(xServer, cfg)
            comp.start()
            component = comp
            // Bring up VortekRendererComponent on its own unix socket.
            // Present.selectInput + DRI3 pixmapFromHardwareBuffer both
            // call into VortekRendererComponent via XServerView.queueEvent
            // for GPU texture lifecycle management. Without it the
            // window's content Drawable can't actually composite the
            // GPU-backed pixmaps that DRI3 imports — game renders into
            // swapchain images that never reach the screen.
            val vortekCfg = UnixSocketConfig.createSocket(socketRoot, UnixSocketConfig.VORTEK_SERVER_PATH)
            val vortekOpts = VortekRendererComponent.Options()
            val vortek = VortekRendererComponent(xServer, vortekCfg, vortekOpts, context)
            vortek.start()
            vortekComponent = vortek
            Log.i(TAG, "VortekRendererComponent listening on ${vortekCfg.path}")
            running = true
            Log.i(TAG, "XConnector X server listening on ${cfg.path}")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "XConnector start failed", t)
            false
        }
    }

    fun stop() {
        if (!running) return
        try { vortekComponent?.stop() } catch (_: Throwable) {}
        vortekComponent = null
        try { component?.stop() } catch (_: Throwable) {}
        component = null
        socketConfig = null
        running = false
    }

    fun isRunning(): Boolean = running
    fun socketPath(): String? = socketConfig?.path
}
