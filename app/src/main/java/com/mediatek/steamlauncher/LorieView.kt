package com.mediatek.steamlauncher

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PixelFormat
import android.util.AttributeSet
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection

/**
 * Custom SurfaceView that renders X11 content and handles input.
 * This view receives rendering from the native X11 server and
 * translates Android touch/keyboard events to X11 events.
 */
class LorieView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : SurfaceView(context, attrs, defStyleAttr), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "LorieView"

        // Mouse button constants (X11)
        const val MOUSE_BUTTON_LEFT = 1
        const val MOUSE_BUTTON_MIDDLE = 2
        const val MOUSE_BUTTON_RIGHT = 3
        const val MOUSE_SCROLL_UP = 4
        const val MOUSE_SCROLL_DOWN = 5
    }

    var x11Server: X11Server? = null
        set(value) {
            field = value
            value?.attachSurface(this)
        }

    // Touch handling state
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var isTouchingScreen = false

    // Input mode (touch as mouse vs direct touch)
    var touchMode = TouchMode.MOUSE
        set(value) {
            field = value
            Log.d(TAG, "Touch mode changed to: $value")
        }

    // Scale factors for coordinate translation
    private var scaleX = 1.0f
    private var scaleY = 1.0f
    private var offsetX = 0f
    private var offsetY = 0f

    // Cursor position for mouse mode
    private var cursorX = 0f
    private var cursorY = 0f

    // Loading state
    private var isLoading = true
    private val loadingPaint = Paint().apply {
        color = Color.WHITE
        textSize = 48f
        textAlign = Paint.Align.CENTER
    }

    init {
        holder.addCallback(this)
        holder.setFormat(PixelFormat.RGBA_8888)

        // Enable hardware acceleration
        setLayerType(LAYER_TYPE_HARDWARE, null)

        // Make focusable for keyboard input
        isFocusable = true
        isFocusableInTouchMode = true
        requestFocus()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.d(TAG, "Surface created")
        isLoading = false
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.d(TAG, "Surface changed: ${width}x${height}")

        // Update scale factors
        x11Server?.getDisplayInfo()?.let { info ->
            if (info.width > 0 && info.height > 0) {
                scaleX = info.width.toFloat() / width
                scaleY = info.height.toFloat() / height
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.d(TAG, "Surface destroyed")
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (x11Server == null || !x11Server!!.isRunning()) {
            return super.onTouchEvent(event)
        }

        return when (touchMode) {
            TouchMode.MOUSE -> handleMouseTouch(event)
            TouchMode.DIRECT -> handleDirectTouch(event)
            TouchMode.TRACKPAD -> handleTrackpadTouch(event)
        }
    }

    private fun handleMouseTouch(event: MotionEvent): Boolean {
        val x = event.x * scaleX
        val y = event.y * scaleY

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = event.x
                lastTouchY = event.y
                isTouchingScreen = true

                // Update cursor position
                cursorX = x
                cursorY = y
                x11Server?.sendMouseMotion(cursorX, cursorY)

                // Delay the click to distinguish from drag
                postDelayed({
                    if (isTouchingScreen && !isDragging()) {
                        x11Server?.sendMouseButton(MOUSE_BUTTON_LEFT, true, cursorX, cursorY)
                    }
                }, 100)
            }

            MotionEvent.ACTION_MOVE -> {
                if (isTouchingScreen) {
                    val deltaX = event.x - lastTouchX
                    val deltaY = event.y - lastTouchY

                    cursorX += deltaX * scaleX
                    cursorY += deltaY * scaleY

                    // Clamp cursor position
                    x11Server?.getDisplayInfo()?.let { info ->
                        cursorX = cursorX.coerceIn(0f, info.width.toFloat())
                        cursorY = cursorY.coerceIn(0f, info.height.toFloat())
                    }

                    x11Server?.sendMouseMotion(cursorX, cursorY)

                    lastTouchX = event.x
                    lastTouchY = event.y
                }
            }

            MotionEvent.ACTION_UP -> {
                if (isTouchingScreen) {
                    x11Server?.sendMouseButton(MOUSE_BUTTON_LEFT, false, cursorX, cursorY)
                }
                isTouchingScreen = false
            }

            MotionEvent.ACTION_CANCEL -> {
                isTouchingScreen = false
            }
        }

        return true
    }

    private fun handleDirectTouch(event: MotionEvent): Boolean {
        val x = event.x * scaleX
        val y = event.y * scaleY

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                x11Server?.sendTouchEvent(MotionEvent.ACTION_DOWN, x, y, event.getPointerId(event.actionIndex))
            }

            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    val px = event.getX(i) * scaleX
                    val py = event.getY(i) * scaleY
                    x11Server?.sendTouchEvent(MotionEvent.ACTION_MOVE, px, py, event.getPointerId(i))
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                x11Server?.sendTouchEvent(MotionEvent.ACTION_UP, x, y, event.getPointerId(event.actionIndex))
            }

            MotionEvent.ACTION_CANCEL -> {
                x11Server?.sendTouchEvent(MotionEvent.ACTION_CANCEL, x, y, 0)
            }
        }

        return true
    }

    private fun handleTrackpadTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = event.x
                lastTouchY = event.y
                isTouchingScreen = true
            }

            MotionEvent.ACTION_MOVE -> {
                if (isTouchingScreen) {
                    val deltaX = (event.x - lastTouchX) * scaleX
                    val deltaY = (event.y - lastTouchY) * scaleY

                    cursorX += deltaX
                    cursorY += deltaY

                    x11Server?.getDisplayInfo()?.let { info ->
                        cursorX = cursorX.coerceIn(0f, info.width.toFloat())
                        cursorY = cursorY.coerceIn(0f, info.height.toFloat())
                    }

                    x11Server?.sendMouseMotion(cursorX, cursorY)

                    lastTouchX = event.x
                    lastTouchY = event.y
                }
            }

            MotionEvent.ACTION_UP -> {
                // Tap detection
                val dx = kotlin.math.abs(event.x - lastTouchX)
                val dy = kotlin.math.abs(event.y - lastTouchY)
                if (dx < 10 && dy < 10) {
                    // This was a tap, simulate click
                    x11Server?.sendMouseButton(MOUSE_BUTTON_LEFT, true, cursorX, cursorY)
                    postDelayed({
                        x11Server?.sendMouseButton(MOUSE_BUTTON_LEFT, false, cursorX, cursorY)
                    }, 50)
                }
                isTouchingScreen = false
            }
        }

        return true
    }

    private fun isDragging(): Boolean {
        val dx = kotlin.math.abs(lastTouchX - cursorX / scaleX)
        val dy = kotlin.math.abs(lastTouchY - cursorY / scaleY)
        return dx > 20 || dy > 20
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (x11Server?.isRunning() == true) {
            val x11KeyCode = translateKeyCode(keyCode)
            if (x11KeyCode != 0) {
                x11Server?.sendKeyEvent(x11KeyCode, true)
                return true
            }
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (x11Server?.isRunning() == true) {
            val x11KeyCode = translateKeyCode(keyCode)
            if (x11KeyCode != 0) {
                x11Server?.sendKeyEvent(x11KeyCode, false)
                return true
            }
        }
        return super.onKeyUp(keyCode, event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        // Handle gamepad/mouse motion
        if (event.source and android.view.InputDevice.SOURCE_CLASS_JOYSTICK != 0) {
            // Gamepad input
            return handleGamepadEvent(event)
        }

        if (event.source and android.view.InputDevice.SOURCE_MOUSE != 0) {
            // External mouse
            return handleExternalMouseEvent(event)
        }

        return super.onGenericMotionEvent(event)
    }

    private fun handleGamepadEvent(event: MotionEvent): Boolean {
        // Handle gamepad stick movement as mouse
        val leftX = event.getAxisValue(MotionEvent.AXIS_X)
        val leftY = event.getAxisValue(MotionEvent.AXIS_Y)

        if (kotlin.math.abs(leftX) > 0.1f || kotlin.math.abs(leftY) > 0.1f) {
            cursorX += leftX * 10 * scaleX
            cursorY += leftY * 10 * scaleY

            x11Server?.getDisplayInfo()?.let { info ->
                cursorX = cursorX.coerceIn(0f, info.width.toFloat())
                cursorY = cursorY.coerceIn(0f, info.height.toFloat())
            }

            x11Server?.sendMouseMotion(cursorX, cursorY)
            return true
        }

        return false
    }

    private fun handleExternalMouseEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE -> {
                val x = event.x * scaleX
                val y = event.y * scaleY
                x11Server?.sendMouseMotion(x, y)
                return true
            }

            MotionEvent.ACTION_SCROLL -> {
                val scrollX = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
                val scrollY = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                x11Server?.sendScroll(scrollX, scrollY)
                return true
            }
        }

        return false
    }

    private fun translateKeyCode(androidKeyCode: Int): Int {
        // Map Android key codes to X11 key codes
        return when (androidKeyCode) {
            KeyEvent.KEYCODE_A -> 38
            KeyEvent.KEYCODE_B -> 56
            KeyEvent.KEYCODE_C -> 54
            KeyEvent.KEYCODE_D -> 40
            KeyEvent.KEYCODE_E -> 26
            KeyEvent.KEYCODE_F -> 41
            KeyEvent.KEYCODE_G -> 42
            KeyEvent.KEYCODE_H -> 43
            KeyEvent.KEYCODE_I -> 31
            KeyEvent.KEYCODE_J -> 44
            KeyEvent.KEYCODE_K -> 45
            KeyEvent.KEYCODE_L -> 46
            KeyEvent.KEYCODE_M -> 58
            KeyEvent.KEYCODE_N -> 57
            KeyEvent.KEYCODE_O -> 32
            KeyEvent.KEYCODE_P -> 33
            KeyEvent.KEYCODE_Q -> 24
            KeyEvent.KEYCODE_R -> 27
            KeyEvent.KEYCODE_S -> 39
            KeyEvent.KEYCODE_T -> 28
            KeyEvent.KEYCODE_U -> 30
            KeyEvent.KEYCODE_V -> 55
            KeyEvent.KEYCODE_W -> 25
            KeyEvent.KEYCODE_X -> 53
            KeyEvent.KEYCODE_Y -> 29
            KeyEvent.KEYCODE_Z -> 52
            KeyEvent.KEYCODE_0 -> 19
            KeyEvent.KEYCODE_1 -> 10
            KeyEvent.KEYCODE_2 -> 11
            KeyEvent.KEYCODE_3 -> 12
            KeyEvent.KEYCODE_4 -> 13
            KeyEvent.KEYCODE_5 -> 14
            KeyEvent.KEYCODE_6 -> 15
            KeyEvent.KEYCODE_7 -> 16
            KeyEvent.KEYCODE_8 -> 17
            KeyEvent.KEYCODE_9 -> 18
            KeyEvent.KEYCODE_SPACE -> 65
            KeyEvent.KEYCODE_ENTER -> 36
            KeyEvent.KEYCODE_TAB -> 23
            KeyEvent.KEYCODE_ESCAPE -> 9
            KeyEvent.KEYCODE_DEL -> 22 // Backspace
            KeyEvent.KEYCODE_FORWARD_DEL -> 119 // Delete
            KeyEvent.KEYCODE_SHIFT_LEFT -> 50
            KeyEvent.KEYCODE_SHIFT_RIGHT -> 62
            KeyEvent.KEYCODE_CTRL_LEFT -> 37
            KeyEvent.KEYCODE_CTRL_RIGHT -> 105
            KeyEvent.KEYCODE_ALT_LEFT -> 64
            KeyEvent.KEYCODE_ALT_RIGHT -> 108
            KeyEvent.KEYCODE_DPAD_UP -> 111
            KeyEvent.KEYCODE_DPAD_DOWN -> 116
            KeyEvent.KEYCODE_DPAD_LEFT -> 113
            KeyEvent.KEYCODE_DPAD_RIGHT -> 114
            KeyEvent.KEYCODE_F1 -> 67
            KeyEvent.KEYCODE_F2 -> 68
            KeyEvent.KEYCODE_F3 -> 69
            KeyEvent.KEYCODE_F4 -> 70
            KeyEvent.KEYCODE_F5 -> 71
            KeyEvent.KEYCODE_F6 -> 72
            KeyEvent.KEYCODE_F7 -> 73
            KeyEvent.KEYCODE_F8 -> 74
            KeyEvent.KEYCODE_F9 -> 75
            KeyEvent.KEYCODE_F10 -> 76
            KeyEvent.KEYCODE_F11 -> 95
            KeyEvent.KEYCODE_F12 -> 96
            else -> 0
        }
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = android.text.InputType.TYPE_NULL
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN

        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                // Send each character as key press
                text.forEach { char ->
                    val keyCode = getKeyCodeForChar(char)
                    if (keyCode != 0) {
                        x11Server?.sendKeyEvent(keyCode, true)
                        x11Server?.sendKeyEvent(keyCode, false)
                    }
                }
                return true
            }

            private fun getKeyCodeForChar(char: Char): Int {
                return when {
                    char in 'a'..'z' -> 38 + (char - 'a')
                    char in 'A'..'Z' -> 38 + (char.lowercaseChar() - 'a')
                    char in '0'..'9' -> if (char == '0') 19 else 10 + (char - '1')
                    char == ' ' -> 65
                    char == '\n' -> 36
                    else -> 0
                }
            }
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        if (isLoading) {
            canvas.drawColor(Color.BLACK)
            canvas.drawText(
                "Starting X11 server...",
                width / 2f,
                height / 2f,
                loadingPaint
            )
        }
    }

    /**
     * Show right-click menu at current position (two-finger tap).
     */
    fun performRightClick() {
        x11Server?.let { server ->
            server.sendMouseButton(MOUSE_BUTTON_RIGHT, true, cursorX, cursorY)
            postDelayed({
                server.sendMouseButton(MOUSE_BUTTON_RIGHT, false, cursorX, cursorY)
            }, 50)
        }
    }

    /**
     * Show middle click (three-finger tap).
     */
    fun performMiddleClick() {
        x11Server?.let { server ->
            server.sendMouseButton(MOUSE_BUTTON_MIDDLE, true, cursorX, cursorY)
            postDelayed({
                server.sendMouseButton(MOUSE_BUTTON_MIDDLE, false, cursorX, cursorY)
            }, 50)
        }
    }

    enum class TouchMode {
        MOUSE,      // Touch moves cursor, tap clicks
        DIRECT,     // Touch is direct touch input
        TRACKPAD    // Touch area acts as trackpad
    }
}
