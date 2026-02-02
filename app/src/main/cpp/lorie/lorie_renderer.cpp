/**
 * Lorie Renderer
 *
 * Handles OpenGL ES rendering for the X11 server.
 * Manages shaders, textures, and frame composition.
 */

#include <android/log.h>
#include <GLES2/gl2.h>
#include <string>

#define LOG_TAG "LorieRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace lorie {

class Renderer {
public:
    Renderer() : mProgram(0), mTexture(0), mVbo(0) {}

    bool initialize() {
        // Create shader program
        const char* vertexShaderSrc = R"(
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        )";

        const char* fragmentShaderSrc = R"(
            precision mediump float;
            varying vec2 vTexCoord;
            uniform sampler2D uTexture;
            uniform float uAlpha;
            void main() {
                vec4 color = texture2D(uTexture, vTexCoord);
                gl_FragColor = vec4(color.rgb, color.a * uAlpha);
            }
        )";

        GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSrc);
        GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);

        if (vertexShader == 0 || fragmentShader == 0) {
            return false;
        }

        mProgram = glCreateProgram();
        glAttachShader(mProgram, vertexShader);
        glAttachShader(mProgram, fragmentShader);
        glLinkProgram(mProgram);

        GLint linked;
        glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            glGetProgramInfoLog(mProgram, sizeof(log), nullptr, log);
            LOGE("Program linking failed: %s", log);
            return false;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        // Create vertex buffer for fullscreen quad
        const float vertices[] = {
            // Position    TexCoord
            -1.0f, -1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
            -1.0f,  1.0f,  0.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 0.0f,
        };

        glGenBuffers(1, &mVbo);
        glBindBuffer(GL_ARRAY_BUFFER, mVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // Create texture
        glGenTextures(1, &mTexture);
        glBindTexture(GL_TEXTURE_2D, mTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        LOGI("Renderer initialized");
        return true;
    }

    void destroy() {
        if (mProgram) {
            glDeleteProgram(mProgram);
            mProgram = 0;
        }
        if (mTexture) {
            glDeleteTextures(1, &mTexture);
            mTexture = 0;
        }
        if (mVbo) {
            glDeleteBuffers(1, &mVbo);
            mVbo = 0;
        }
    }

    void render(const void* pixels, int width, int height, float alpha = 1.0f) {
        glUseProgram(mProgram);

        // Update texture
        glBindTexture(GL_TEXTURE_2D, mTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        // Set uniforms
        GLint alphaLoc = glGetUniformLocation(mProgram, "uAlpha");
        glUniform1f(alphaLoc, alpha);

        // Draw quad
        glBindBuffer(GL_ARRAY_BUFFER, mVbo);

        GLint posLoc = glGetAttribLocation(mProgram, "aPosition");
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);

        GLint texLoc = glGetAttribLocation(mProgram, "aTexCoord");
        glEnableVertexAttribArray(texLoc);
        glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void*)(2 * sizeof(float)));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(posLoc);
        glDisableVertexAttribArray(texLoc);
    }

    void clear(float r, float g, float b, float a) {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

private:
    GLuint compileShader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint compiled;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[512];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOGE("Shader compilation failed: %s", log);
            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

    GLuint mProgram;
    GLuint mTexture;
    GLuint mVbo;
};

} // namespace lorie
