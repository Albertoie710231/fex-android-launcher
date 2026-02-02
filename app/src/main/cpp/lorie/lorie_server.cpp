/**
 * Lorie X11 Server Implementation
 *
 * This is a simplified X11 server that provides basic functionality for
 * running X11 applications on Android. It uses shared memory for efficient
 * frame buffer sharing and OpenGL ES for rendering.
 */

#include "lorie_server.h"
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cerrno>

#define LOG_TAG "LorieServer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Vertex shader for rendering the framebuffer
static const char* VERTEX_SHADER = R"(
    attribute vec4 aPosition;
    attribute vec2 aTexCoord;
    varying vec2 vTexCoord;
    void main() {
        gl_Position = aPosition;
        vTexCoord = aTexCoord;
    }
)";

// Fragment shader for rendering the framebuffer
static const char* FRAGMENT_SHADER = R"(
    precision mediump float;
    varying vec2 vTexCoord;
    uniform sampler2D uTexture;
    void main() {
        gl_FragColor = texture2D(uTexture, vTexCoord);
    }
)";

// Vertex data for a fullscreen quad
static const float QUAD_VERTICES[] = {
    // Position     // TexCoord
    -1.0f, -1.0f,   0.0f, 1.0f,
     1.0f, -1.0f,   1.0f, 1.0f,
    -1.0f,  1.0f,   0.0f, 0.0f,
     1.0f,  1.0f,   1.0f, 0.0f,
};

LorieServer::LorieServer(const char* socketPath, int displayNum)
    : mSocketPath(socketPath)
    , mDisplayNum(displayNum)
    , mServerSocket(-1)
    , mWindow(nullptr)
    , mWidth(1920)
    , mHeight(1080)
    , mDepth(24)
    , mEglDisplay(EGL_NO_DISPLAY)
    , mEglSurface(EGL_NO_SURFACE)
    , mEglContext(EGL_NO_CONTEXT)
    , mTexture(0)
    , mProgram(0)
    , mVbo(0)
    , mRunning(false)
    , mCursorX(0)
    , mCursorY(0)
{
    LOGI("LorieServer created: socket=%s, display=%d", socketPath, displayNum);
}

LorieServer::~LorieServer() {
    stop();
    cleanupEGL();

    if (mServerSocket >= 0) {
        close(mServerSocket);
    }

    if (mWindow) {
        ANativeWindow_release(mWindow);
    }
}

bool LorieServer::initialize() {
    LOGI("Initializing X11 server");

    // Allocate frame buffer
    mFrameBuffer.resize(mWidth * mHeight);
    std::fill(mFrameBuffer.begin(), mFrameBuffer.end(), 0xFF1a1a2e); // Dark blue background

    if (!createSocket()) {
        LOGE("Failed to create socket");
        return false;
    }

    LOGI("X11 server initialized successfully");
    return true;
}

bool LorieServer::createSocket() {
    // Create Unix domain socket
    mServerSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerSocket < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        return false;
    }

    // Make socket non-blocking
    int flags = fcntl(mServerSocket, F_GETFL, 0);
    fcntl(mServerSocket, F_SETFL, flags | O_NONBLOCK);

    // Bind to socket path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    std::string socketFile = mSocketPath + "/X" + std::to_string(mDisplayNum);
    strncpy(addr.sun_path, socketFile.c_str(), sizeof(addr.sun_path) - 1);

    // Remove existing socket file
    unlink(socketFile.c_str());

    if (bind(mServerSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to bind socket: %s", strerror(errno));
        close(mServerSocket);
        mServerSocket = -1;
        return false;
    }

    if (listen(mServerSocket, 5) < 0) {
        LOGE("Failed to listen on socket: %s", strerror(errno));
        close(mServerSocket);
        mServerSocket = -1;
        return false;
    }

    // Set socket permissions
    chmod(socketFile.c_str(), 0777);

    LOGI("X11 socket created: %s", socketFile.c_str());
    return true;
}

bool LorieServer::initEGL() {
    if (!mWindow) {
        LOGE("No native window set");
        return false;
    }

    // Get EGL display
    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mEglDisplay == EGL_NO_DISPLAY) {
        LOGE("Failed to get EGL display");
        return false;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(mEglDisplay, &major, &minor)) {
        LOGE("Failed to initialize EGL");
        return false;
    }
    LOGI("EGL initialized: %d.%d", major, minor);

    // Choose config
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint numConfigs;
    if (!eglChooseConfig(mEglDisplay, configAttribs, &mEglConfig, 1, &numConfigs) || numConfigs == 0) {
        LOGE("Failed to choose EGL config");
        return false;
    }

    // Create surface
    mEglSurface = eglCreateWindowSurface(mEglDisplay, mEglConfig, mWindow, nullptr);
    if (mEglSurface == EGL_NO_SURFACE) {
        LOGE("Failed to create EGL surface");
        return false;
    }

    // Create context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    mEglContext = eglCreateContext(mEglDisplay, mEglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (mEglContext == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL context");
        return false;
    }

    // Make current
    if (!eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext)) {
        LOGE("Failed to make EGL context current");
        return false;
    }

    // Create shader program
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &VERTEX_SHADER, nullptr);
    glCompileShader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &FRAGMENT_SHADER, nullptr);
    glCompileShader(fragmentShader);

    mProgram = glCreateProgram();
    glAttachShader(mProgram, vertexShader);
    glAttachShader(mProgram, fragmentShader);
    glLinkProgram(mProgram);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Create vertex buffer
    glGenBuffers(1, &mVbo);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTICES), QUAD_VERTICES, GL_STATIC_DRAW);

    // Create texture
    glGenTextures(1, &mTexture);
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    LOGI("EGL initialized successfully");
    return true;
}

void LorieServer::cleanupEGL() {
    if (mEglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (mEglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(mEglDisplay, mEglSurface);
            mEglSurface = EGL_NO_SURFACE;
        }

        if (mEglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(mEglDisplay, mEglContext);
            mEglContext = EGL_NO_CONTEXT;
        }

        eglTerminate(mEglDisplay);
        mEglDisplay = EGL_NO_DISPLAY;
    }

    if (mTexture) {
        glDeleteTextures(1, &mTexture);
        mTexture = 0;
    }

    if (mProgram) {
        glDeleteProgram(mProgram);
        mProgram = 0;
    }

    if (mVbo) {
        glDeleteBuffers(1, &mVbo);
        mVbo = 0;
    }
}

void LorieServer::run() {
    mRunning = true;
    LOGI("X11 server running");

    std::vector<pollfd> pollfds;

    while (mRunning) {
        pollfds.clear();
        pollfds.push_back({mServerSocket, POLLIN, 0});

        int ret = poll(pollfds.data(), pollfds.size(), 16); // ~60fps
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOGE("Poll error: %s", strerror(errno));
            break;
        }

        // Accept new connections
        if (pollfds[0].revents & POLLIN) {
            int clientFd = accept(mServerSocket, nullptr, nullptr);
            if (clientFd >= 0) {
                LOGI("New X11 client connected: fd=%d", clientFd);
                // Handle client in separate thread or add to poll list
                handleClient(clientFd);
            }
        }

        // Render frame
        renderFrame();
    }

    LOGI("X11 server stopped");
}

void LorieServer::stop() {
    mRunning = false;
}

void LorieServer::handleClient(int clientFd) {
    // In a full implementation, this would handle X11 protocol messages
    // For now, we just close the connection
    LOGD("Handling X11 client: fd=%d", clientFd);

    // Set non-blocking
    int flags = fcntl(clientFd, F_GETFL, 0);
    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

    // Read and process X11 protocol messages
    // This is where you'd implement X11 protocol handling
    // For Termux:X11 compatibility, you'd parse Xlib requests

    close(clientFd);
}

void LorieServer::renderFrame() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mEglDisplay == EGL_NO_DISPLAY || mEglSurface == EGL_NO_SURFACE) {
        return;
    }

    glViewport(0, 0, mWidth, mHeight);
    glClearColor(0.1f, 0.1f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Upload framebuffer to texture
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mWidth, mHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, mFrameBuffer.data());

    // Draw quad
    glUseProgram(mProgram);

    glBindBuffer(GL_ARRAY_BUFFER, mVbo);

    GLint posAttrib = glGetAttribLocation(mProgram, "aPosition");
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);

    GLint texAttrib = glGetAttribLocation(mProgram, "aTexCoord");
    glEnableVertexAttribArray(texAttrib);
    glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    eglSwapBuffers(mEglDisplay, mEglSurface);
}

void LorieServer::setSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mWindow) {
        cleanupEGL();
        ANativeWindow_release(mWindow);
    }

    mWindow = window;

    if (mWindow) {
        ANativeWindow_acquire(mWindow);
        mWidth = ANativeWindow_getWidth(mWindow);
        mHeight = ANativeWindow_getHeight(mWindow);
        mFrameBuffer.resize(mWidth * mHeight);
        std::fill(mFrameBuffer.begin(), mFrameBuffer.end(), 0xFF1a1a2e);
        initEGL();
    }
}

void LorieServer::resize(int width, int height) {
    std::lock_guard<std::mutex> lock(mMutex);

    if (width != mWidth || height != mHeight) {
        mWidth = width;
        mHeight = height;
        mFrameBuffer.resize(mWidth * mHeight);
        std::fill(mFrameBuffer.begin(), mFrameBuffer.end(), 0xFF1a1a2e);
        LOGI("Display resized to %dx%d", mWidth, mHeight);
    }
}

void LorieServer::sendTouchEvent(int action, float x, float y, int pointerId) {
    LOGD("Touch event: action=%d, x=%.1f, y=%.1f, pointer=%d", action, x, y, pointerId);
    // Forward to X11 clients
}

void LorieServer::sendKeyEvent(int keyCode, bool isDown) {
    LOGD("Key event: code=%d, down=%d", keyCode, isDown);
    // Forward to X11 clients
}

void LorieServer::sendMouseButton(int button, bool isDown, float x, float y) {
    LOGD("Mouse button: button=%d, down=%d, x=%.1f, y=%.1f", button, isDown, x, y);
    mCursorX = x;
    mCursorY = y;
    // Forward to X11 clients
}

void LorieServer::sendMouseMotion(float x, float y) {
    mCursorX = x;
    mCursorY = y;
    // Forward to X11 clients
}

void LorieServer::sendScroll(float deltaX, float deltaY) {
    LOGD("Scroll: dx=%.1f, dy=%.1f", deltaX, deltaY);
    // Forward to X11 clients
}

void LorieServer::setClipboard(const char* text) {
    std::lock_guard<std::mutex> lock(mClipboardMutex);
    mClipboard = text ? text : "";
}

std::string LorieServer::getClipboard() {
    std::lock_guard<std::mutex> lock(mClipboardMutex);
    return mClipboard;
}

void LorieServer::getDisplayInfo(int& width, int& height, int& depth) {
    std::lock_guard<std::mutex> lock(mMutex);
    width = mWidth;
    height = mHeight;
    depth = mDepth;
}
