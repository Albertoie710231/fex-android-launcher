/**
 * FramebufferBridge JNI - Native code for HardwareBuffer management
 *
 * This provides the bridge between Java's HardwareBuffer and native
 * AHardwareBuffer pointers that libvortekrenderer.so expects.
 */

#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define LOG_TAG "FramebufferBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

/**
 * Get the native AHardwareBuffer pointer from a Java HardwareBuffer.
 * This is what libvortekrenderer.so expects for rendering targets.
 */
JNIEXPORT jlong JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_getNativeHardwareBuffer(
        JNIEnv *env,
        jobject /* this */,
        jobject hardwareBuffer) {

    if (hardwareBuffer == nullptr) {
        LOGE("getNativeHardwareBuffer: null HardwareBuffer");
        return 0;
    }

    // Convert Java HardwareBuffer to native AHardwareBuffer
    AHardwareBuffer *nativeBuffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
    if (nativeBuffer == nullptr) {
        LOGE("getNativeHardwareBuffer: failed to get native buffer");
        return 0;
    }

    // Acquire a reference so the buffer isn't freed while in use
    AHardwareBuffer_acquire(nativeBuffer);

    LOGI("getNativeHardwareBuffer: got native buffer %p", nativeBuffer);
    return reinterpret_cast<jlong>(nativeBuffer);
}

/**
 * Release a native AHardwareBuffer reference.
 */
JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_releaseNativeHardwareBuffer(
        JNIEnv *env,
        jobject /* this */,
        jlong nativePtr) {

    if (nativePtr == 0) {
        return;
    }

    AHardwareBuffer *nativeBuffer = reinterpret_cast<AHardwareBuffer*>(nativePtr);
    AHardwareBuffer_release(nativeBuffer);
    LOGI("releaseNativeHardwareBuffer: released %p", nativeBuffer);
}

/**
 * Lock a HardwareBuffer for CPU access and return a ByteBuffer.
 */
JNIEXPORT jobject JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_lockHardwareBuffer(
        JNIEnv *env,
        jobject /* this */,
        jobject hardwareBuffer) {

    if (hardwareBuffer == nullptr) {
        return nullptr;
    }

    AHardwareBuffer *nativeBuffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
    if (nativeBuffer == nullptr) {
        return nullptr;
    }

    // Get buffer description
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(nativeBuffer, &desc);

    // Lock for CPU read access
    void *data = nullptr;
    int result = AHardwareBuffer_lock(nativeBuffer,
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
            -1, nullptr, &data);

    if (result != 0 || data == nullptr) {
        LOGE("lockHardwareBuffer: lock failed with result %d", result);
        return nullptr;
    }

    // Calculate buffer size (assuming RGBA8888)
    size_t size = desc.stride * desc.height * 4;

    // Create a direct ByteBuffer pointing to the locked memory
    return env->NewDirectByteBuffer(data, size);
}

/**
 * Unlock a previously locked HardwareBuffer.
 */
JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_unlockHardwareBuffer(
        JNIEnv *env,
        jobject /* this */,
        jobject hardwareBuffer) {

    if (hardwareBuffer == nullptr) {
        return;
    }

    AHardwareBuffer *nativeBuffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
    if (nativeBuffer == nullptr) {
        return;
    }

    AHardwareBuffer_unlock(nativeBuffer, nullptr);
}

/**
 * Create a HardwareBuffer with specified dimensions and format.
 * Returns the Java HardwareBuffer object.
 */
JNIEXPORT jobject JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_00024Companion_createHardwareBuffer(
        JNIEnv *env,
        jobject /* companion */,
        jint width,
        jint height,
        jint format,
        jlong usage) {

    AHardwareBuffer_Desc desc = {};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = format;  // AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1
    desc.usage = usage;

    AHardwareBuffer *nativeBuffer = nullptr;
    int result = AHardwareBuffer_allocate(&desc, &nativeBuffer);

    if (result != 0 || nativeBuffer == nullptr) {
        LOGE("createHardwareBuffer: allocation failed with result %d", result);
        return nullptr;
    }

    // Convert to Java HardwareBuffer
    jobject javaBuffer = AHardwareBuffer_toHardwareBuffer(env, nativeBuffer);

    // Release our reference (Java now owns it)
    AHardwareBuffer_release(nativeBuffer);

    LOGI("createHardwareBuffer: created %dx%d buffer", width, height);
    return javaBuffer;
}

/**
 * Get native Surface (ANativeWindow) pointer.
 */
JNIEXPORT jlong JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_getNativeSurfacePtr(
        JNIEnv *env,
        jobject /* this */,
        jobject surface) {

    if (surface == nullptr) {
        return 0;
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        LOGE("getNativeSurfacePtr: failed to get ANativeWindow");
        return 0;
    }

    // Acquire a reference
    ANativeWindow_acquire(window);

    return reinterpret_cast<jlong>(window);
}

/**
 * Release a native Surface reference.
 */
JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_FramebufferBridge_releaseNativeSurface(
        JNIEnv *env,
        jobject /* this */,
        jlong nativePtr) {

    if (nativePtr == 0) {
        return;
    }

    ANativeWindow *window = reinterpret_cast<ANativeWindow*>(nativePtr);
    ANativeWindow_release(window);
}

} // extern "C"
