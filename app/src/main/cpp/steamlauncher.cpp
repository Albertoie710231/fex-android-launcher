/**
 * Steam Launcher JNI Bridge
 *
 * Provides Unix socket operations for X11 client bridging.
 * libXlorie creates the X11 socket but doesn't properly accept()
 * filesystem socket connections from external processes (proot).
 *
 * Solution: We create our own listening socket and accept connections,
 * then pass the FD to LorieView.connect(fd).
 */

#include <jni.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

#define LOG_TAG "SteamLauncher-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Static server socket FD (reused across accept calls)
static int g_server_fd = -1;

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("SteamLauncher native library loaded");
    return JNI_VERSION_1_6;
}

/**
 * Create a listening Unix socket at the given path.
 * This should be called BEFORE libXlorie starts so we own the socket.
 *
 * Returns: server FD on success, -1 on error
 */
JNIEXPORT jint JNICALL
Java_com_mediatek_steamlauncher_X11Server_createListeningSocket(JNIEnv *env, jclass clazz, jstring jSocketPath) {
    const char *socketPath = env->GetStringUTFChars(jSocketPath, nullptr);
    if (!socketPath) {
        LOGE("Failed to get socket path string");
        return -1;
    }

    // Close existing server socket if any
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    // Remove existing socket file
    unlink(socketPath);

    // Create socket
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        env->ReleaseStringUTFChars(jSocketPath, socketPath);
        return -1;
    }

    // Bind to path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to bind socket to %s: %s", socketPath, strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        env->ReleaseStringUTFChars(jSocketPath, socketPath);
        return -1;
    }

    // Make socket world-accessible (for proot)
    chmod(socketPath, 0777);

    // Start listening
    if (listen(g_server_fd, 5) < 0) {
        LOGE("Failed to listen on socket: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        unlink(socketPath);
        env->ReleaseStringUTFChars(jSocketPath, socketPath);
        return -1;
    }

    LOGI("Created listening socket at %s, fd=%d", socketPath, g_server_fd);
    env->ReleaseStringUTFChars(jSocketPath, socketPath);
    return g_server_fd;
}

/**
 * Accept a connection on our listening socket.
 * Blocks until a client connects (with timeout).
 *
 * Returns: client FD on success, -1 on error, -2 on timeout
 */
JNIEXPORT jint JNICALL
Java_com_mediatek_steamlauncher_X11Server_acceptUnixSocket(JNIEnv *env, jclass clazz, jstring jSocketPath) {
    (void)jSocketPath;  // We use the global server FD

    if (g_server_fd < 0) {
        LOGE("No listening socket available");
        return -1;
    }

    // Poll for incoming connection with timeout
    struct pollfd pfd;
    pfd.fd = g_server_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 5000);  // 5 second timeout

    if (ret < 0) {
        LOGE("Poll error: %s", strerror(errno));
        return -1;
    }

    if (ret == 0) {
        // Timeout, no connection
        return -2;
    }

    if (!(pfd.revents & POLLIN)) {
        LOGD("Poll returned but no POLLIN event");
        return -2;
    }

    // Accept the connection
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd < 0) {
        LOGE("Accept failed: %s", strerror(errno));
        return -1;
    }

    LOGI("Accepted X11 client connection, fd=%d", client_fd);
    return client_fd;
}

/**
 * Close the listening socket.
 */
JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_closeListeningSocket(JNIEnv *env, jclass clazz) {
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
        LOGI("Closed listening socket");
    }
}

} // extern "C"
