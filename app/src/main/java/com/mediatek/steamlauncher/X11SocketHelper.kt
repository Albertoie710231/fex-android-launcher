package com.mediatek.steamlauncher

import android.util.Log

/**
 * JNI helper for creating Unix domain sockets at filesystem paths.
 */
object X11SocketHelper {

    private const val TAG = "X11SocketHelper"

    init {
        try {
            System.loadLibrary("x11socket")
            Log.i(TAG, "libx11socket.so loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load libx11socket.so", e)
        }
    }

    /**
     * Create a Unix domain socket and bind it to the given filesystem path.
     * Returns the socket fd on success, -1 on failure.
     */
    @JvmStatic
    external fun createUnixSocket(path: String): Int

    /**
     * Accept a connection on the server socket.
     * Returns the client fd on success, -1 on failure.
     */
    @JvmStatic
    external fun acceptConnection(serverFd: Int): Int

    /**
     * Close a socket.
     */
    @JvmStatic
    external fun closeSocket(fd: Int)

    /**
     * Remove a socket file from the filesystem.
     */
    @JvmStatic
    external fun unlinkSocket(path: String)
}
