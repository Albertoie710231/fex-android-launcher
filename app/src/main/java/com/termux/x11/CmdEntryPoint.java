package com.termux.x11;

import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import androidx.annotation.Keep;

/**
 * Entry point for the X11 server.
 * This class interfaces with libXlorie.so to start and manage the X server.
 *
 * IMPORTANT: This class MUST be in the com.termux.x11 package
 * because libXlorie.so has JNI bindings for this exact path.
 */
@Keep
public class CmdEntryPoint {

    private static final String TAG = "CmdEntryPoint";
    private static Handler handler = new Handler(Looper.getMainLooper());
    private static CmdEntryPoint instance;
    private static boolean serverStarted = false;
    private static Thread listenerThread = null;

    static {
        try {
            System.loadLibrary("Xlorie");
            Log.i(TAG, "libXlorie loaded successfully");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load libXlorie", e);
        }
    }

    // Native methods - must match libXlorie.so JNI signatures
    public static native void start(String[] args);
    public native ParcelFileDescriptor getXConnection();
    public native ParcelFileDescriptor getLogcatOutput();
    public static native boolean connected();
    public native void listenForConnections();

    /**
     * Check if the server has already been started in this process
     */
    public static boolean isServerStarted() {
        return serverStarted && instance != null;
    }

    /**
     * Reset connection state (call before starting a new server)
     */
    public static void resetConnectionState() {
        synchronized (connectionLock) {
            connectCalled = false;
        }
        Log.i(TAG, "Connection state reset");
    }

    /**
     * Start the X11 server on display :0
     */
    public static boolean startServer() {
        // If server is already started, don't try to start again
        if (serverStarted) {
            Log.w(TAG, "X11 server already started, reusing existing instance");
            return true;
        }

        Log.i(TAG, "Starting X11 server on :0 with TCP enabled");
        try {
            instance = new CmdEntryPoint();
            // Pass display number and enable TCP listening
            // -listen tcp: enable TCP connections on port 6000
            // -ac: disable access control (allow any client to connect)
            start(new String[]{":0", "-listen", "tcp", "-ac"});

            // Start listening for connections in background thread
            listenerThread = new Thread(() -> {
                Log.d(TAG, "Starting connection listener thread");
                instance.listenForConnections();
                Log.d(TAG, "Connection listener thread ended");
                // If listener ends, mark server as not started so it can be restarted
                serverStarted = false;
            }, "X11-Listener");
            listenerThread.start();

            // Set initial screen dimensions so clients get valid geometry
            // (before any LorieView is attached)
            LorieView.sendWindowChange(1920, 1080, 60, "Steam Launcher");

            serverStarted = true;
            Log.i(TAG, "X11 server started (1920x1080, TCP + abstract socket)");
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Failed to start X11 server", e);
            serverStarted = false;
            return false;
        }
    }

    /**
     * Get X connection file descriptor
     */
    public static int getXConnectionFd() {
        if (instance == null) {
            Log.e(TAG, "Server not started");
            return -1;
        }
        try {
            ParcelFileDescriptor pfd = instance.getXConnection();
            if (pfd != null) {
                return pfd.detachFd();
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to get X connection fd", e);
        }
        return -1;
    }

    private static boolean connectCalled = false;
    private static final Object connectionLock = new Object();

    /**
     * Called by native code to send broadcast (client connected notification).
     * This is where we need to trigger the rendering pipeline.
     */
    @Keep
    @SuppressWarnings("unused")
    public void sendBroadcast() {
        // Only log occasionally to avoid spam
        synchronized (connectionLock) {
            if (!connectCalled) {
                Log.i(TAG, "sendBroadcast() called - X11 client connected!");
            }
        }

        // Post to main thread to establish connection
        handler.post(() -> establishConnection(0));
    }

    /**
     * Attempt to establish the connection with the LorieView.
     * Works without a LorieView — uses hardcoded dimensions for headless X11.
     * Game rendering goes through DXVK → Vulkan headless layer → TCP → SurfaceView,
     * NOT through X11/LorieView. X11 is only needed for windowing/protocol.
     */
    private void establishConnection(int attempt) {
        try {
            // Get dimensions from LorieView if available, otherwise use defaults
            int w = 1920;
            int h = 1080;
            LorieView view = MainActivity.getLorieView();
            if (view != null) {
                int vw = view.getWidth();
                int vh = view.getHeight();
                if (vw > 0 && vh > 0) {
                    w = vw;
                    h = vh;
                }
            }

            // Only call connect() once
            synchronized (connectionLock) {
                if (connectCalled) {
                    return;
                }

                Log.i(TAG, "Establishing X11 connection (" + w + "x" + h + ", LorieView=" + (view != null) + ")");

                // Send window change to configure screen dimensions
                LorieView.sendWindowChange(w, h, 60, "Steam Launcher");

                // Try to establish connection via fd
                ParcelFileDescriptor pfd = getXConnection();
                if (pfd != null) {
                    int fd = pfd.detachFd();
                    Log.i(TAG, "Got X connection fd: " + fd);

                    if (fd >= 0) {
                        LorieView.connect(fd);
                        Log.i(TAG, "After connect(), connected = " + LorieView.connected());
                    }
                } else {
                    Log.w(TAG, "getXConnection() returned null — X11 protocol still works via native listener");
                }

                // Request connection to start rendering
                boolean reqResult = LorieView.requestConnection();
                Log.i(TAG, "requestConnection() returned: " + reqResult);

                connectCalled = true;
            }

        } catch (Exception e) {
            Log.e(TAG, "establishConnection error", e);
        }
    }

    @Keep
    @SuppressWarnings("unused")
    public void sendBroadcast(String action) {
        Log.i(TAG, "sendBroadcast(action=" + action + ")");
        sendBroadcast();
    }
}
