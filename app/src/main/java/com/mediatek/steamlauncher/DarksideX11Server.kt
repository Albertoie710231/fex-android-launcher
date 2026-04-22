package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import au.com.darkside.xserver.Atom
import au.com.darkside.xserver.Client
import au.com.darkside.xserver.Property
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
            // Toggle to log every X11 request (very verbose — leave off unless
            // diagnosing a specific "server isn't responding as expected"
            // blocker). Flipping to true also activates GetProperty logging
            // in Property.java and InternAtom logging in Atom.java.
            Client.LOG_REQUESTS = false
            val srv = XServer(context, PORT, null)
            // Must initialize _rootWindow BEFORE listening for clients — the
            // protocol handshake writes screen info (ScreenView.write L1102)
            // which dereferences _rootWindow. In non-headless mode this would
            // happen in the View's onSizeChanged callback; here we call the
            // public entry point we exposed on ScreenView directly.
            srv.screen.initRootWindow(1920, 1080)
            // Install minimal EWMH properties on root so wine's X11 driver
            // detects "a window manager is running". Without these, wine
            // polls _NET_SUPPORTING_WM_CHECK / _NET_SUPPORTED /
            // _NET_ACTIVE_WINDOW and — on "no WM detected" — takes a
            // degraded path the Yamaneko engine never fully progresses
            // out of (observed 2026-04-22: 17x each of those GetProperty
            // calls returning empty on the BGM-loop black-screen run).
            installEwmhStub(srv, 1920, 1080)
            if (!srv.start()) {
                Log.e(TAG, "XServer.start() returned false")
                return false
            }
            xServer = srv
            isRunning = true
            Log.i(TAG, "Darkside X server listening on TCP :$PORT (display :0) with EWMH stub")
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

    /**
     * Diagnostic: composite Darkside's root + children into a Bitmap and
     * save to `out` as a PNG. Returns the File's absolute path on success,
     * null otherwise. Used to SEE the X11 pixmap content (launcher
     * dialogs, GDI-painted UI) that our DXVK-only display pipeline skips.
     */
    /**
     * Return the current composited root bitmap, or null if the server
     * isn't ready. Used by TerminalActivity's display render loop.
     */
    fun renderRoot(): android.graphics.Bitmap? =
        xServer?.screen?.renderRootToBitmap()

    /** Dump the whole mapped-window tree (root + descendants) to logcat. */
    fun dumpWindowTree() {
        val srv = xServer ?: return
        val root = srv.screen.rootWindow ?: return
        dumpOne(root, 0)
    }

    private fun dumpOne(w: au.com.darkside.xserver.Window, depth: Int) {
        val r = w.iRect
        Log.i(TAG, "  ".repeat(depth) + "win 0x${w.id.toString(16)} " +
                "mapped=${w.isMapped()} rect=(${r.left},${r.top})-(${r.right},${r.bottom}) " +
                "children=${w.children.size}")
        for (c in w.children) dumpOne(c, depth + 1)
    }

    fun renderRootToFile(out: java.io.File): String? {
        val srv = xServer ?: return null
        val bmp = srv.screen.renderRootToBitmap() ?: return null
        try {
            java.io.FileOutputStream(out).use { fos ->
                bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, fos)
            }
        } catch (t: Throwable) {
            Log.w(TAG, "renderRootToFile failed: ${t.message}")
            return null
        }
        return out.absolutePath
    }

    /**
     * Populate the root window with the minimum EWMH (Extended Window Manager
     * Hints) advertisement so X11 clients detect us as a compliant WM.
     *
     * The critical property is `_NET_SUPPORTING_WM_CHECK`: if it exists on
     * root AND points to a window that ALSO carries `_NET_SUPPORTING_WM_CHECK`
     * pointing back to itself (and has `_NET_WM_NAME`), EWMH clients treat
     * the server as WM-managed. Wine's X11 driver uses this to decide whether
     * to request WM-mediated window placement or self-manage. On the
     * self-manage path (no WM), game engines that wait for WM-driven activation
     * events never get them and park at the "play intro BGM, wait for title
     * screen load" state.
     *
     * We reuse the root window itself as the WM-check target (legal per
     * EWMH — many minimal WMs do this).
     *
     * All 32-bit property data is written little-endian because wine on
     * ARM64 Android always declares LSB-first byte order at connect-time.
     * Darkside stores property data as opaque bytes (no swap on
     * Change/GetProperty data field).
     */
    private fun installEwmhStub(srv: XServer, width: Int, height: Int) {
        val root = srv.screen.rootWindow ?: run {
            Log.w(TAG, "installEwmhStub: root window not yet created"); return
        }
        val rootId = root.id

        // Predefined X11 atom ids (from Atom.java _predefinedAtoms, 1-based):
        //   ATOM=4, CARDINAL=7, WINDOW=33.
        val aAtom = 4
        val aCardinal = 7
        val aWindow = 33

        // Intern-or-create helper. Darkside's findAtom/nextFreeAtomId/addAtom
        // together cover this.
        fun atom(name: String): Int {
            val existing = srv.findAtom(name)
            if (existing != null) return existing.id
            val a = Atom(srv.nextFreeAtomId(), name)
            srv.addAtom(a)
            return a.id
        }

        val aUtf8 = atom("UTF8_STRING")
        val aWmCheck = atom("_NET_SUPPORTING_WM_CHECK")
        val aSupported = atom("_NET_SUPPORTED")
        val aActive = atom("_NET_ACTIVE_WINDOW")
        val aWorkarea = atom("_NET_WORKAREA")
        val aWmName = atom("_NET_WM_NAME")
        val aClientList = atom("_NET_CLIENT_LIST")
        val aClientListStacking = atom("_NET_CLIENT_LIST_STACKING")
        val aDesktopGeometry = atom("_NET_DESKTOP_GEOMETRY")
        val aDesktopViewport = atom("_NET_DESKTOP_VIEWPORT")
        val aNumberOfDesktops = atom("_NET_NUMBER_OF_DESKTOPS")
        val aCurrentDesktop = atom("_NET_CURRENT_DESKTOP")
        val aWmState = atom("_NET_WM_STATE")

        fun addProp(pid: Int, type: Int, format: Byte, data: ByteArray) {
            val p = Property(pid, type, format)
            p.data = data
            root.addProperty(p)
        }

        // _NET_SUPPORTING_WM_CHECK on root -> root (we are the WM-indicator
        // window as well). Wine then does a second GetProperty on that
        // window for the same atom; since root.addProperty is the same
        // hashtable, the second query returns the same value — valid loop.
        addProp(aWmCheck, aWindow, 32, intLE(rootId))

        // _NET_WM_NAME on the indicator window — required to complete the
        // EWMH WM-detection handshake.
        addProp(aWmName, aUtf8, 8, "darkside-mini-wm".toByteArray(Charsets.UTF_8))

        // _NET_SUPPORTED: list of EWMH atoms we advertise (as ATOM[]).
        addProp(aSupported, aAtom, 32, intsLE(intArrayOf(
            aWmCheck, aSupported, aActive, aWorkarea, aWmName,
            aClientList, aClientListStacking, aDesktopGeometry,
            aDesktopViewport, aNumberOfDesktops, aCurrentDesktop, aWmState,
        )))

        // _NET_ACTIVE_WINDOW: 0 (no active window yet). Will be updated by
        // the auto-focus-on-map path when the game creates its window.
        addProp(aActive, aWindow, 32, intLE(0))

        // _NET_WORKAREA: [x,y,w,h] per-desktop. We have 1 desktop.
        addProp(aWorkarea, aCardinal, 32, intsLE(intArrayOf(0, 0, width, height)))

        addProp(aDesktopGeometry, aCardinal, 32, intsLE(intArrayOf(width, height)))
        addProp(aDesktopViewport, aCardinal, 32, intsLE(intArrayOf(0, 0)))
        addProp(aNumberOfDesktops, aCardinal, 32, intLE(1))
        addProp(aCurrentDesktop, aCardinal, 32, intLE(0))

        // Empty client lists — populated as clients map windows.
        addProp(aClientList, aWindow, 32, ByteArray(0))
        addProp(aClientListStacking, aWindow, 32, ByteArray(0))

        Log.i(TAG, "EWMH stub installed on root 0x${rootId.toString(16)} " +
                "(size=${width}x${height}, atoms: check=$aWmCheck supported=$aSupported " +
                "active=$aActive workarea=$aWorkarea)")
    }

    private fun intLE(v: Int): ByteArray = byteArrayOf(
        (v and 0xff).toByte(),
        ((v ushr 8) and 0xff).toByte(),
        ((v ushr 16) and 0xff).toByte(),
        ((v ushr 24) and 0xff).toByte(),
    )

    private fun intsLE(ints: IntArray): ByteArray {
        val b = ByteArray(ints.size * 4)
        for (i in ints.indices) {
            val v = ints[i]
            b[i * 4] = (v and 0xff).toByte()
            b[i * 4 + 1] = ((v ushr 8) and 0xff).toByte()
            b[i * 4 + 2] = ((v ushr 16) and 0xff).toByte()
            b[i * 4 + 3] = ((v ushr 24) and 0xff).toByte()
        }
        return b
    }
}
