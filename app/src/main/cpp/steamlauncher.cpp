/**
 * Steam Launcher JNI Bridge
 *
 * Main entry point for native code. Provides JNI functions for:
 * - X11 server management (Lorie)
 * - Vulkan passthrough configuration
 * - Input handling
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "lorie/lorie_server.h"
#include "vulkan_bridge/vulkan_bridge.h"

#define LOG_TAG "SteamLauncher-JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

// ============================================================================
// X11 Server (Lorie) JNI Functions
// ============================================================================

JNIEXPORT jlong JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeInit(
    JNIEnv *env,
    jobject thiz,
    jstring socket_path,
    jint display_num
) {
    const char *path = env->GetStringUTFChars(socket_path, nullptr);
    LOGI("Initializing X11 server at %s, display :%d", path, display_num);

    auto *server = new LorieServer(path, display_num);
    env->ReleaseStringUTFChars(socket_path, path);

    if (!server->initialize()) {
        LOGE("Failed to initialize X11 server");
        delete server;
        return 0;
    }

    return reinterpret_cast<jlong>(server);
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeRun(
    JNIEnv *env,
    jobject thiz,
    jlong ptr
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->run();
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeStop(
    JNIEnv *env,
    jobject thiz,
    jlong ptr
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->stop();
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeDestroy(
    JNIEnv *env,
    jobject thiz,
    jlong ptr
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        delete server;
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSetSurface(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jobject surface
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (!server) return;

    if (surface) {
        ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
        server->setSurface(window);
    } else {
        server->setSurface(nullptr);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeResizeSurface(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jint width,
    jint height
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->resize(width, height);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSendTouch(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jint action,
    jfloat x,
    jfloat y,
    jint pointer_id
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->sendTouchEvent(action, x, y, pointer_id);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSendKey(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jint key_code,
    jboolean is_down
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->sendKeyEvent(key_code, is_down);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSendMouseButton(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jint button,
    jboolean is_down,
    jfloat x,
    jfloat y
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->sendMouseButton(button, is_down, x, y);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSendMouseMotion(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jfloat x,
    jfloat y
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->sendMouseMotion(x, y);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSendScroll(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jfloat delta_x,
    jfloat delta_y
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        server->sendScroll(delta_x, delta_y);
    }
}

JNIEXPORT void JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeSetClipboard(
    JNIEnv *env,
    jobject thiz,
    jlong ptr,
    jstring text
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server && text) {
        const char *str = env->GetStringUTFChars(text, nullptr);
        server->setClipboard(str);
        env->ReleaseStringUTFChars(text, str);
    }
}

JNIEXPORT jstring JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeGetClipboard(
    JNIEnv *env,
    jobject thiz,
    jlong ptr
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);
    if (server) {
        std::string clipboard = server->getClipboard();
        return env->NewStringUTF(clipboard.c_str());
    }
    return env->NewStringUTF("");
}

JNIEXPORT jobject JNICALL
Java_com_mediatek_steamlauncher_X11Server_nativeGetDisplayInfo(
    JNIEnv *env,
    jobject thiz,
    jlong ptr
) {
    auto *server = reinterpret_cast<LorieServer*>(ptr);

    // Find the DisplayInfo class
    jclass displayInfoClass = env->FindClass("com/mediatek/steamlauncher/X11Server$DisplayInfo");
    if (!displayInfoClass) {
        LOGE("Failed to find DisplayInfo class");
        return nullptr;
    }

    // Find constructor
    jmethodID constructor = env->GetMethodID(displayInfoClass, "<init>", "(III)V");
    if (!constructor) {
        LOGE("Failed to find DisplayInfo constructor");
        return nullptr;
    }

    int width = 1920, height = 1080, depth = 24;
    if (server) {
        server->getDisplayInfo(width, height, depth);
    }

    return env->NewObject(displayInfoClass, constructor, width, height, depth);
}

// ============================================================================
// Library initialization
// ============================================================================

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("SteamLauncher native library loaded");
    return JNI_VERSION_1_6;
}

} // extern "C"
