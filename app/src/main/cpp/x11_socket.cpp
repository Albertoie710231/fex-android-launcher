/**
 * JNI helper for creating Unix domain sockets at filesystem paths.
 *
 * This is needed because Android's LocalServerSocket only supports abstract sockets,
 * but X11 clients expect filesystem-based sockets at /tmp/.X11-unix/X0.
 */

#include <jni.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <android/log.h>

#define TAG "X11Socket"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" {

/**
 * Create a Unix domain socket and bind it to the given path.
 * Returns the socket fd on success, -1 on failure.
 */
JNIEXPORT jint JNICALL
Java_com_mediatek_steamlauncher_X11SocketHelper_createUnixSocket(
        JNIEnv *env, jclass clazz, jstring path) {

    const char *socketPath = env->GetStringUTFChars(path, nullptr);
    if (!socketPath) {
        LOGE("Failed to get socket path string");
        return -1;
    }

    LOGI("Creating Unix socket at: %s", socketPath);

    // Remove existing socket file
    unlink(socketPath);

    // Create socket
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        env->ReleaseStringUTFChars(path, socketPath);
        return -1;
    }

    // Bind to path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("bind() failed: %s", strerror(errno));
        close(sockfd);
        env->ReleaseStringUTFChars(path, socketPath);
        return -1;
    }

    // Set permissions to world-accessible
    chmod(socketPath, 0777);

    // Listen for connections
    if (listen(sockfd, 5) < 0) {
        LOGE("listen() failed: %s", strerror(errno));
        close(sockfd);
        unlink(socketPath);
        env->ReleaseStringUTFChars(path, socketPath);
        return -1;
    }

    LOGI("Unix socket created and listening, fd=%d", sockfd);
    env->ReleaseStringUTFChars(path, socketPath);
    return sockfd;
}

/**
 * Accept a connection on the socket.
 * Returns the client fd on success, -1 on failure.
 */
JNIEXPORT jint JNICALL
Java_com_mediatek_steamlauncher_X11SocketHelper_acceptConnection(
        JNIEnv *env, jclass clazz, jint serverFd) {

    struct sockaddr_un clientAddr;
    socklen_t clientLen = sizeof(clientAddr);

    int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &clientLen);
    if (clientFd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGE("accept() failed: %s", strerror(errno));
        }
        return -1;
    }

    LOGI("Accepted client connection, fd=%d", clientFd);
    return clientFd;
}

/**
 * Close a socket.
 */
JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11SocketHelper_closeSocket(
        JNIEnv *env, jclass clazz, jint fd) {
    if (fd >= 0) {
        close(fd);
        LOGI("Closed socket fd=%d", fd);
    }
}

/**
 * Remove a socket file.
 */
JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11SocketHelper_unlinkSocket(
        JNIEnv *env, jclass clazz, jstring path) {

    const char *socketPath = env->GetStringUTFChars(path, nullptr);
    if (socketPath) {
        unlink(socketPath);
        env->ReleaseStringUTFChars(path, socketPath);
    }
}

/**
 * Read data from a socket into a byte array.
 * Returns bytes read, 0 on EOF, -1 on error.
 */
JNIEXPORT jint JNICALL
Java_com_mediatek_steamlauncher_X11SocketHelper_readSocket(
        JNIEnv *env, jclass clazz, jint fd, jbyteArray buffer, jint offset, jint length) {

    if (fd < 0 || !buffer) {
        return -1;
    }

    jbyte *buf = env->GetByteArrayElements(buffer, nullptr);
    if (!buf) {
        return -1;
    }

    ssize_t n = read(fd, buf + offset, length);

    env->ReleaseByteArrayElements(buffer, buf, 0);

    return (jint)n;
}

} // extern "C"
