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

        Log.i(TAG, "Starting X11 server on :0");
        try {
            instance = new CmdEntryPoint();
            // Pass display and try to set resolution via arguments
            // libXlorie may accept -screen or other Xorg-style arguments
            start(new String[]{":0"});

            // Start listening for connections in background thread
            listenerThread = new Thread(() -> {
                Log.d(TAG, "Starting connection listener thread");
                instance.listenForConnections();
                Log.d(TAG, "Connection listener thread ended");
                // If listener ends, mark server as not started so it can be restarted
                serverStarted = false;
            }, "X11-Listener");
            listenerThread.start();

            serverStarted = true;
            Log.i(TAG, "X11 server started");
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
     * Retries if surface is not ready yet.
     */
    private void establishConnection(int attempt) {
        try {
            LorieView view = MainActivity.getLorieView();
            if (view == null) {
                if (attempt < 20) {
                    Log.w(TAG, "LorieView not ready, retrying in 100ms (attempt " + (attempt + 1) + ")");
                    handler.postDelayed(() -> establishConnection(attempt + 1), 100);
                } else {
                    Log.e(TAG, "LorieView not available after " + attempt + " attempts");
                }
                return;
            }

            int w = view.getWidth();
            int h = view.getHeight();

            // Check if surface has dimensions (is ready)
            if (w <= 0 || h <= 0) {
                if (attempt < 20) {
                    Log.w(TAG, "Surface not ready (size: " + w + "x" + h + "), retrying in 100ms (attempt " + (attempt + 1) + ")");
                    handler.postDelayed(() -> establishConnection(attempt + 1), 100);
                } else {
                    Log.e(TAG, "Surface not ready after " + attempt + " attempts");
                }
                return;
            }

            // Only call connect() once
            synchronized (connectionLock) {
                if (connectCalled) {
                    return;
                }

                Log.i(TAG, "Surface ready (" + w + "x" + h + "), establishing connection");

                // Try to establish connection via fd - use detachFd() to transfer ownership
                ParcelFileDescriptor pfd = getXConnection();
                if (pfd != null) {
                    int fd = pfd.detachFd();  // Use detachFd() instead of getFd()
                    Log.i(TAG, "Got X connection fd: " + fd);

                    if (fd >= 0) {
                        // Call connect() to establish the frame delivery pipeline
                        Log.i(TAG, "Calling LorieView.connect(" + fd + ")");
                        LorieView.connect(fd);
                        Log.i(TAG, "After connect(), connected = " + LorieView.connected());
                    }
                } else {
                    Log.w(TAG, "getXConnection() returned null");
                }

                // Send window change to configure dimensions
                Log.i(TAG, "Sending window change: " + w + "x" + h);
                LorieView.sendWindowChange(w, h, 60, "Steam Launcher");

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
