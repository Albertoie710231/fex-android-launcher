/**
 * Lorie X11 Server Header
 *
 * A minimal X11 server implementation for Android that:
 * - Listens on a Unix socket for X11 client connections
 * - Receives rendering commands and composites them
 * - Renders the result to an Android Surface via OpenGL ES
 * - Forwards input events to connected clients
 */

#ifndef LORIE_SERVER_H
#define LORIE_SERVER_H

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

class LorieServer {
public:
    LorieServer(const char* socketPath, int displayNum);
    ~LorieServer();

    bool initialize();
    void run();
    void stop();

    void setSurface(ANativeWindow* window);
    void resize(int width, int height);

    // Input events
    void sendTouchEvent(int action, float x, float y, int pointerId);
    void sendKeyEvent(int keyCode, bool isDown);
    void sendMouseButton(int button, bool isDown, float x, float y);
    void sendMouseMotion(float x, float y);
    void sendScroll(float deltaX, float deltaY);

    // Clipboard
    void setClipboard(const char* text);
    std::string getClipboard();

    // Display info
    void getDisplayInfo(int& width, int& height, int& depth);

private:
    bool createSocket();
    bool initEGL();
    void cleanupEGL();
    void renderFrame();
    void handleClient(int clientFd);

    std::string mSocketPath;
    int mDisplayNum;
    int mServerSocket;

    ANativeWindow* mWindow;
    int mWidth;
    int mHeight;
    int mDepth;

    // EGL context
    EGLDisplay mEglDisplay;
    EGLSurface mEglSurface;
    EGLContext mEglContext;
    EGLConfig mEglConfig;

    // Frame buffer
    std::vector<uint32_t> mFrameBuffer;
    GLuint mTexture;
    GLuint mProgram;
    GLuint mVbo;

    // Threading
    std::atomic<bool> mRunning;
    std::thread mServerThread;
    std::mutex mMutex;

    // Clipboard
    std::string mClipboard;
    std::mutex mClipboardMutex;

    // Cursor position
    float mCursorX;
    float mCursorY;
};

#endif // LORIE_SERVER_H
