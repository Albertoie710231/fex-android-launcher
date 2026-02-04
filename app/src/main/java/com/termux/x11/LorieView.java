package com.termux.x11;

import android.annotation.SuppressLint;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.text.InputType;
import android.util.AttributeSet;
import android.util.Log;
import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;

import static java.nio.charset.StandardCharsets.UTF_8;

/**
 * SurfaceView that renders X11 content via libXlorie.
 *
 * IMPORTANT: This class MUST be in the com.termux.x11 package
 * because libXlorie.so has JNI bindings for this exact path.
 */
@Keep
@SuppressLint("WrongConstant")
public class LorieView extends SurfaceView {

    private static final String TAG = "LorieView";

    public interface Callback {
        void changed(int surfaceWidth, int surfaceHeight, int screenWidth, int screenHeight);
    }

    public interface SurfaceReadyCallback {
        void onSurfaceReady(int width, int height);
    }

    interface PixelFormat {
        int BGRA_8888 = 5;
    }

    private static int frameCount = 0;
    private static long lastLogTime = 0;
    private static final int FRAME_INTERVAL_MS = 16; // ~60fps
    private final Handler renderHandler = new Handler(Looper.getMainLooper());

    /**
     * Handler-based render loop (more reliable than Choreographer for this use case).
     */
    private final Runnable renderRunnable = new Runnable() {
        @Override
        public void run() {
            if (!frameCallbackRunning) {
                return;
            }

            // Schedule next frame first
            renderHandler.postDelayed(this, FRAME_INTERVAL_MS);

            if (!surfaceReady) {
                return;
            }

            frameCount++;
            long now = System.currentTimeMillis();
            if (now - lastLogTime > 2000) {
                boolean isConnected = connected();
                Log.d(TAG, "Render loop: frames=" + frameCount + ", connected=" + isConnected);
                lastLogTime = now;
            }

            // Pump frames to native renderer
            requestConnection();
        }
    };

    public static final int BUTTON_LEFT = 1;
    public static final int BUTTON_MIDDLE = 2;
    public static final int BUTTON_RIGHT = 3;
    public static final int BUTTON_SCROLL = 4;

    private ClipboardManager clipboard;
    private final InputMethodManager imm;
    private Callback mCallback;
    private SurfaceReadyCallback surfaceReadyCallback;

    private int screenWidth = 1920;
    private int screenHeight = 1080;
    private boolean surfaceReady = false;
    private boolean frameCallbackRunning = false;

    public TouchMode touchMode = TouchMode.TRACKPAD;
    private float lastTouchX = 0f;
    private float lastTouchY = 0f;
    private boolean isTouchDown = false;
    private float cursorX = 0f;
    private float cursorY = 0f;
    private float scaleX = 1.0f;
    private float scaleY = 1.0f;

    private final InputConnection inputConnection = new BaseInputConnection(this, false) {
        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            if (text != null && text.length() > 0) {
                sendTextEvent(text.toString().getBytes(UTF_8));
            }
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            return LorieView.this.dispatchKeyEvent(event);
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) {
                LorieView.this.sendKeyEvent(0, KeyEvent.KEYCODE_DEL, true);
                LorieView.this.sendKeyEvent(0, KeyEvent.KEYCODE_DEL, false);
            }
            return true;
        }
    };

    private final SurfaceHolder.Callback surfaceCallback = new SurfaceHolder.Callback() {
        @Override
        public void surfaceCreated(@NonNull SurfaceHolder holder) {
            holder.setFormat(PixelFormat.BGRA_8888);
            Log.d(TAG, "Surface created");
        }

        @Override
        public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
            Log.d(TAG, "Surface changed: " + width + "x" + height);
            LorieView.this.surfaceChanged(holder.getSurface());

            screenWidth = width;
            screenHeight = height;
            scaleX = 1.0f;
            scaleY = 1.0f;

            // Mark surface as ready
            surfaceReady = true;

            // Notify libXlorie of the new dimensions
            sendWindowChange(width, height, 60, "Steam Launcher");

            // CRITICAL: Request connection after surface is ready
            // This establishes the in-process rendering pipeline
            boolean connResult = requestConnection();
            Log.i(TAG, "requestConnection() in surfaceChanged returned: " + connResult);

            // Start Choreographer frame callback for continuous rendering
            startFrameCallback();

            if (mCallback != null) {
                mCallback.changed(width, height, screenWidth, screenHeight);
            }

            // Notify that surface is ready (for timing fix)
            if (surfaceReadyCallback != null) {
                surfaceReadyCallback.onSurfaceReady(width, height);
            }
        }

        @Override
        public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
            Log.d(TAG, "Surface destroyed");
            surfaceReady = false;
            stopFrameCallback();
            LorieView.this.surfaceChanged(null);
        }
    };

    public LorieView(Context context) {
        super(context);
        imm = (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
        init();
    }

    public LorieView(Context context, AttributeSet attrs) {
        super(context, attrs);
        imm = (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
        init();
    }

    public LorieView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        imm = (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
        init();
    }

    private void init() {
        Log.d(TAG, "Initializing LorieView");
        getHolder().addCallback(surfaceCallback);
        clipboard = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);

        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();

        nativeInit();
        Log.d(TAG, "LorieView initialized");
    }

    public void setCallback(Callback callback) {
        mCallback = callback;
    }

    public void setSurfaceReadyCallback(SurfaceReadyCallback callback) {
        surfaceReadyCallback = callback;
    }

    public boolean isSurfaceReady() {
        return surfaceReady;
    }

    /**
     * Start the render loop for continuous rendering.
     */
    private void startFrameCallback() {
        if (frameCallbackRunning) {
            return;
        }
        frameCallbackRunning = true;
        Log.i(TAG, "Render loop started");
        renderHandler.post(renderRunnable);
    }

    /**
     * Stop the render loop.
     */
    private void stopFrameCallback() {
        frameCallbackRunning = false;
        renderHandler.removeCallbacks(renderRunnable);
        Log.i(TAG, "Render loop stopped");
    }

    public void triggerCallback() {
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();

        Rect r = getHolder().getSurfaceFrame();
        if (r.width() > 0 && r.height() > 0) {
            post(() -> surfaceCallback.surfaceChanged(getHolder(), PixelFormat.BGRA_8888, r.width(), r.height()));
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (touchMode) {
            case MOUSE:
                return handleMouseTouch(event);
            case DIRECT:
                return handleDirectTouch(event);
            case TRACKPAD:
            default:
                return handleTrackpadTouch(event);
        }
    }

    private boolean handleMouseTouch(MotionEvent event) {
        float x = event.getX() * scaleX;
        float y = event.getY() * scaleY;

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                cursorX = x;
                cursorY = y;
                sendMouseEvent(cursorX, cursorY, 0, false, false);
                sendMouseEvent(cursorX, cursorY, BUTTON_LEFT, true, false);
                break;
            case MotionEvent.ACTION_MOVE:
                cursorX = x;
                cursorY = y;
                sendMouseEvent(cursorX, cursorY, 0, false, false);
                break;
            case MotionEvent.ACTION_UP:
                sendMouseEvent(cursorX, cursorY, BUTTON_LEFT, false, false);
                break;
        }
        return true;
    }

    private boolean handleDirectTouch(MotionEvent event) {
        int x = (int) (event.getX() * scaleX);
        int y = (int) (event.getY() * scaleY);

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                sendTouchEvent(MotionEvent.ACTION_DOWN, event.getPointerId(event.getActionIndex()), x, y);
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int px = (int) (event.getX(i) * scaleX);
                    int py = (int) (event.getY(i) * scaleY);
                    sendTouchEvent(MotionEvent.ACTION_MOVE, event.getPointerId(i), px, py);
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                sendTouchEvent(MotionEvent.ACTION_UP, event.getPointerId(event.getActionIndex()), x, y);
                break;
        }
        return true;
    }

    private boolean handleTrackpadTouch(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                lastTouchX = event.getX();
                lastTouchY = event.getY();
                isTouchDown = true;
                break;
            case MotionEvent.ACTION_MOVE:
                if (isTouchDown) {
                    float deltaX = (event.getX() - lastTouchX) * scaleX;
                    float deltaY = (event.getY() - lastTouchY) * scaleY;

                    cursorX = Math.max(0, Math.min(screenWidth, cursorX + deltaX));
                    cursorY = Math.max(0, Math.min(screenHeight, cursorY + deltaY));

                    sendMouseEvent(cursorX, cursorY, 0, false, false);

                    lastTouchX = event.getX();
                    lastTouchY = event.getY();
                }
                break;
            case MotionEvent.ACTION_UP:
                float dx = Math.abs(event.getX() - lastTouchX);
                float dy = Math.abs(event.getY() - lastTouchY);
                if (dx < 20 && dy < 20) {
                    sendMouseEvent(cursorX, cursorY, BUTTON_LEFT, true, false);
                    postDelayed(() -> sendMouseEvent(cursorX, cursorY, BUTTON_LEFT, false, false), 50);
                }
                isTouchDown = false;
                break;
        }
        return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return sendKeyEvent(event.getScanCode(), keyCode, true) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return sendKeyEvent(event.getScanCode(), keyCode, false) || super.onKeyUp(keyCode, event);
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN;
        return inputConnection;
    }

    public void performRightClick() {
        sendMouseEvent(cursorX, cursorY, BUTTON_RIGHT, true, false);
        postDelayed(() -> sendMouseEvent(cursorX, cursorY, BUTTON_RIGHT, false, false), 50);
    }

    public boolean sendKeyEvent(int scanCode, int keyCode, boolean keyDown) {
        return sendKeyEvent(scanCode, keyCode, keyDown, 0);
    }

    // Called from native code
    @Keep
    @SuppressWarnings("unused")
    void setClipboardText(String text) {
        clipboard.setPrimaryClip(ClipData.newPlainText("X11 clipboard", text));
    }

    @Keep
    @SuppressWarnings("unused")
    void requestClipboard() {
        CharSequence clip = clipboard.getText();
        sendClipboardEvent(clip != null ? clip.toString().getBytes(UTF_8) : new byte[0]);
    }

    @Keep
    @SuppressWarnings("unused")
    void resetIme() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            imm.invalidateInput(this);
        } else {
            imm.restartInput(this);
        }
    }

    @Keep
    @SuppressWarnings("unused")
    void reloadPreferences() {
        Log.d(TAG, "reloadPreferences called");
    }

    // Native methods - MUST match libXlorie.so JNI signatures
    private native void nativeInit();
    private native void surfaceChanged(Surface surface);

    public static native void connect(int fd);
    public static native boolean connected();
    static native void startLogcat(int fd);
    static native void setClipboardSyncEnabled(boolean enabled, boolean ignored);

    public native void sendClipboardAnnounce();
    public native void sendClipboardEvent(byte[] text);
    public static native void sendWindowChange(int width, int height, int framerate, String name);

    public native void sendMouseEvent(float x, float y, int whichButton, boolean buttonDown, boolean relative);
    public native void sendTouchEvent(int action, int id, int x, int y);
    public native void sendStylusEvent(float x, float y, int pressure, int tiltX, int tiltY, int orientation, int buttons, boolean eraser, boolean mouseMode);
    public static native void requestStylusEnabled(boolean enabled);
    public native boolean sendKeyEvent(int scanCode, int keyCode, boolean keyDown, int unused);
    public native void sendTextEvent(byte[] text);
    public static native boolean requestConnection();

    static {
        System.loadLibrary("Xlorie");
    }

    public enum TouchMode {
        MOUSE,
        DIRECT,
        TRACKPAD
    }
}
