package com.mediatek.steamlauncher

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Handles input from various sources and translates them to X11 events.
 * Supports:
 * - Touch input (as mouse or direct touch)
 * - Physical keyboard
 * - Gamepad/controller
 * - External mouse
 */
class InputHandler(private val x11Server: X11Server) {

    companion object {
        // Gamepad dead zone
        private const val STICK_DEADZONE = 0.15f

        // Mouse acceleration
        private const val MOUSE_SENSITIVITY = 1.5f

        // X11 key codes for gamepad buttons
        private const val KEY_ENTER = 36
        private const val KEY_ESCAPE = 9
        private const val KEY_SPACE = 65
        private const val KEY_TAB = 23
        private const val KEY_UP = 111
        private const val KEY_DOWN = 116
        private const val KEY_LEFT = 113
        private const val KEY_RIGHT = 114
    }

    // Cursor position
    private var cursorX = 0f
    private var cursorY = 0f

    // Display dimensions
    private var displayWidth = 1920
    private var displayHeight = 1080

    // Button state tracking
    private val buttonStates = mutableMapOf<Int, Boolean>()

    fun setDisplaySize(width: Int, height: Int) {
        displayWidth = width
        displayHeight = height
        cursorX = width / 2f
        cursorY = height / 2f
    }

    /**
     * Handle gamepad motion events (sticks, triggers).
     */
    fun handleGamepadMotion(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK == 0) {
            return false
        }

        // Left stick for cursor movement
        val leftX = event.getAxisValue(MotionEvent.AXIS_X)
        val leftY = event.getAxisValue(MotionEvent.AXIS_Y)

        if (abs(leftX) > STICK_DEADZONE || abs(leftY) > STICK_DEADZONE) {
            moveCursor(leftX * MOUSE_SENSITIVITY * 10, leftY * MOUSE_SENSITIVITY * 10)
        }

        // Right stick for scrolling
        val rightX = event.getAxisValue(MotionEvent.AXIS_Z)
        val rightY = event.getAxisValue(MotionEvent.AXIS_RZ)

        if (abs(rightY) > STICK_DEADZONE) {
            x11Server.sendScroll(0f, -rightY)
        }

        // Triggers
        val leftTrigger = event.getAxisValue(MotionEvent.AXIS_LTRIGGER)
        val rightTrigger = event.getAxisValue(MotionEvent.AXIS_RTRIGGER)

        // Left trigger = right click
        handleTrigger(MotionEvent.AXIS_LTRIGGER, leftTrigger > 0.5f, LorieView.MOUSE_BUTTON_RIGHT)

        // Right trigger = left click
        handleTrigger(MotionEvent.AXIS_RTRIGGER, rightTrigger > 0.5f, LorieView.MOUSE_BUTTON_LEFT)

        return true
    }

    private fun handleTrigger(axis: Int, isPressed: Boolean, mouseButton: Int) {
        val wasPressed = buttonStates[axis] ?: false
        if (isPressed != wasPressed) {
            buttonStates[axis] = isPressed
            x11Server.sendMouseButton(mouseButton, isPressed, cursorX, cursorY)
        }
    }

    /**
     * Handle gamepad key events (buttons).
     */
    fun handleGamepadKey(keyCode: Int, isDown: Boolean): Boolean {
        val x11KeyCode = when (keyCode) {
            // Face buttons
            KeyEvent.KEYCODE_BUTTON_A -> KEY_ENTER      // A = Enter/Confirm
            KeyEvent.KEYCODE_BUTTON_B -> KEY_ESCAPE     // B = Back/Cancel
            KeyEvent.KEYCODE_BUTTON_X -> KEY_SPACE      // X = Action
            KeyEvent.KEYCODE_BUTTON_Y -> KEY_TAB        // Y = Tab/Switch

            // D-Pad
            KeyEvent.KEYCODE_DPAD_UP -> KEY_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> KEY_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> KEY_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> KEY_RIGHT

            // Shoulder buttons as mouse clicks
            KeyEvent.KEYCODE_BUTTON_L1 -> {
                x11Server.sendMouseButton(LorieView.MOUSE_BUTTON_LEFT, isDown, cursorX, cursorY)
                return true
            }
            KeyEvent.KEYCODE_BUTTON_R1 -> {
                x11Server.sendMouseButton(LorieView.MOUSE_BUTTON_RIGHT, isDown, cursorX, cursorY)
                return true
            }

            // Stick clicks
            KeyEvent.KEYCODE_BUTTON_THUMBL -> {
                x11Server.sendMouseButton(LorieView.MOUSE_BUTTON_MIDDLE, isDown, cursorX, cursorY)
                return true
            }
            KeyEvent.KEYCODE_BUTTON_THUMBR -> {
                // R3 = Toggle some function
                return true
            }

            // Start/Select
            KeyEvent.KEYCODE_BUTTON_START -> KEY_ENTER
            KeyEvent.KEYCODE_BUTTON_SELECT -> KEY_ESCAPE

            else -> return false
        }

        x11Server.sendKeyEvent(x11KeyCode, isDown)
        return true
    }

    /**
     * Handle external mouse motion.
     */
    fun handleMouseMotion(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_MOUSE == 0) {
            return false
        }

        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE -> {
                cursorX = event.x
                cursorY = event.y
                x11Server.sendMouseMotion(cursorX, cursorY)
                return true
            }

            MotionEvent.ACTION_SCROLL -> {
                val scrollX = event.getAxisValue(MotionEvent.AXIS_HSCROLL)
                val scrollY = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                x11Server.sendScroll(scrollX, scrollY)
                return true
            }
        }

        return false
    }

    /**
     * Handle external mouse button events.
     */
    fun handleMouseButton(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_MOUSE == 0) {
            return false
        }

        val button = when (event.buttonState) {
            MotionEvent.BUTTON_PRIMARY -> LorieView.MOUSE_BUTTON_LEFT
            MotionEvent.BUTTON_SECONDARY -> LorieView.MOUSE_BUTTON_RIGHT
            MotionEvent.BUTTON_TERTIARY -> LorieView.MOUSE_BUTTON_MIDDLE
            else -> return false
        }

        val isDown = event.actionMasked == MotionEvent.ACTION_DOWN ||
                event.actionMasked == MotionEvent.ACTION_BUTTON_PRESS

        cursorX = event.x
        cursorY = event.y
        x11Server.sendMouseButton(button, isDown, cursorX, cursorY)

        return true
    }

    private fun moveCursor(deltaX: Float, deltaY: Float) {
        cursorX = (cursorX + deltaX).coerceIn(0f, displayWidth.toFloat())
        cursorY = (cursorY + deltaY).coerceIn(0f, displayHeight.toFloat())
        x11Server.sendMouseMotion(cursorX, cursorY)
    }

    /**
     * Get current cursor position.
     */
    fun getCursorPosition(): Pair<Float, Float> = Pair(cursorX, cursorY)

    /**
     * Set cursor position directly.
     */
    fun setCursorPosition(x: Float, y: Float) {
        cursorX = x.coerceIn(0f, displayWidth.toFloat())
        cursorY = y.coerceIn(0f, displayHeight.toFloat())
        x11Server.sendMouseMotion(cursorX, cursorY)
    }

    /**
     * Create a default gamepad mapping.
     */
    fun getDefaultGamepadMapping(): GamepadMapping {
        return GamepadMapping(
            a = GamepadAction.KeyPress(KEY_ENTER),
            b = GamepadAction.KeyPress(KEY_ESCAPE),
            x = GamepadAction.KeyPress(KEY_SPACE),
            y = GamepadAction.KeyPress(KEY_TAB),
            l1 = GamepadAction.MouseClick(LorieView.MOUSE_BUTTON_LEFT),
            r1 = GamepadAction.MouseClick(LorieView.MOUSE_BUTTON_RIGHT),
            l2 = GamepadAction.MouseClick(LorieView.MOUSE_BUTTON_RIGHT),
            r2 = GamepadAction.MouseClick(LorieView.MOUSE_BUTTON_LEFT),
            l3 = GamepadAction.MouseClick(LorieView.MOUSE_BUTTON_MIDDLE),
            r3 = GamepadAction.None,
            start = GamepadAction.KeyPress(KEY_ENTER),
            select = GamepadAction.KeyPress(KEY_ESCAPE),
            dpadUp = GamepadAction.KeyPress(KEY_UP),
            dpadDown = GamepadAction.KeyPress(KEY_DOWN),
            dpadLeft = GamepadAction.KeyPress(KEY_LEFT),
            dpadRight = GamepadAction.KeyPress(KEY_RIGHT),
            leftStick = GamepadAction.MouseMove,
            rightStick = GamepadAction.Scroll
        )
    }

    data class GamepadMapping(
        val a: GamepadAction,
        val b: GamepadAction,
        val x: GamepadAction,
        val y: GamepadAction,
        val l1: GamepadAction,
        val r1: GamepadAction,
        val l2: GamepadAction,
        val r2: GamepadAction,
        val l3: GamepadAction,
        val r3: GamepadAction,
        val start: GamepadAction,
        val select: GamepadAction,
        val dpadUp: GamepadAction,
        val dpadDown: GamepadAction,
        val dpadLeft: GamepadAction,
        val dpadRight: GamepadAction,
        val leftStick: GamepadAction,
        val rightStick: GamepadAction
    )

    sealed class GamepadAction {
        data class KeyPress(val keyCode: Int) : GamepadAction()
        data class MouseClick(val button: Int) : GamepadAction()
        data object MouseMove : GamepadAction()
        data object Scroll : GamepadAction()
        data object None : GamepadAction()
    }
}
