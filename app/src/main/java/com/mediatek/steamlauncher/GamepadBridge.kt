package com.mediatek.steamlauncher

import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * Feeds a physical Android gamepad to the game through libevshim.
 *
 * The patched libevshim.so (assets/productize) mmaps $filesDir/gp.mem and
 * exposes it to wine as an SDL virtual joystick (→ XInput). Layout matches
 * evshim's `struct gamepad_state` and GameNative's
 * WinHandler.sendMemoryFileState (little-endian):
 *   0  int16 lx, ly, rx, ry, lt, rt   (sticks ±32767, triggers -32767..32767)
 *  12  uint8 btn[15]                  (SDL GameController button order)
 *  27  uint8 hat                      (unused; d-pad is sent as buttons)
 */
class GamepadBridge(filesDir: File) {

    private val memFile = File(filesDir, "gp.mem")
    private var buffer: ByteBuffer? = null

    private var lx = 0f; private var ly = 0f
    private var rx = 0f; private var ry = 0f
    private var lt = 0f; private var rt = 0f
    private val buttons = ByteArray(15)
    private var hatX = 0f; private var hatY = 0f

    /** True if the event came from a gamepad and was consumed. */
    fun handleKey(event: KeyEvent): Boolean {
        if (!isGamepad(event.source)) return false
        // Android synthesizes fallback keys (DPAD_CENTER, DEL, BACK) for
        // gamepad buttons; the real BUTTON_* event already reached us.
        if (event.flags and KeyEvent.FLAG_FALLBACK != 0) return true
        val pressed = when (event.action) {
            KeyEvent.ACTION_DOWN -> true
            KeyEvent.ACTION_UP -> false
            else -> return true
        }
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_L2 -> lt = if (pressed) 1f else 0f
            KeyEvent.KEYCODE_BUTTON_R2 -> rt = if (pressed) 1f else 0f
            else -> {
                val idx = sdlButton(event.keyCode) ?: return false
                buttons[idx] = if (pressed) 1 else 0
            }
        }
        write()
        return true
    }

    /** True if the event came from a joystick and was consumed. */
    fun handleMotion(event: MotionEvent): Boolean {
        if (!isGamepad(event.source) || event.action != MotionEvent.ACTION_MOVE) return false
        lx = axis(event, MotionEvent.AXIS_X)
        ly = axis(event, MotionEvent.AXIS_Y)
        rx = axis(event, MotionEvent.AXIS_Z)
        ry = axis(event, MotionEvent.AXIS_RZ)
        val l = event.getAxisValue(MotionEvent.AXIS_LTRIGGER)
        val r = event.getAxisValue(MotionEvent.AXIS_RTRIGGER)
        lt = if (l != 0f) l else event.getAxisValue(MotionEvent.AXIS_BRAKE)
        rt = if (r != 0f) r else event.getAxisValue(MotionEvent.AXIS_GAS)
        // Controllers that report the d-pad as a hat instead of key events.
        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        if (hx != hatX || hy != hatY) {
            hatX = hx; hatY = hy
            buttons[11] = if (hy < -0.5f) 1 else 0
            buttons[12] = if (hy > 0.5f) 1 else 0
            buttons[13] = if (hx < -0.5f) 1 else 0
            buttons[14] = if (hx > 0.5f) 1 else 0
        }
        write()
        return true
    }

    private fun write() {
        val buf = buffer ?: map() ?: return
        buf.putShort(0, stick(lx)); buf.putShort(2, stick(ly))
        buf.putShort(4, stick(rx)); buf.putShort(6, stick(ry))
        buf.putShort(8, trigger(lt)); buf.putShort(10, trigger(rt))
        for (i in buttons.indices) buf.put(12 + i, buttons[i])
        buf.put(27, 0)
    }

    private fun map(): ByteBuffer? = try {
        RandomAccessFile(memFile, "rw").use { raf ->
            if (raf.length() < 64) raf.setLength(64)
            raf.channel.map(FileChannel.MapMode.READ_WRITE, 0, 64)
                .order(ByteOrder.LITTLE_ENDIAN)
                .also { buffer = it; Log.i(TAG, "mapped ${memFile.path}") }
        }
    } catch (t: Throwable) {
        Log.e(TAG, "map ${memFile.path} failed", t)
        null
    }

    companion object {
        private const val TAG = "GamepadBridge"
        private const val DEAD_ZONE = 0.1f

        fun isGamepad(source: Int): Boolean =
            (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK

        private fun axis(event: MotionEvent, axis: Int): Float {
            val v = event.getAxisValue(axis)
            val flat = event.device?.getMotionRange(axis, event.source)?.flat ?: 0f
            return if (abs(v) > maxOf(flat, DEAD_ZONE)) v else 0f
        }

        private fun stick(v: Float): Short = (v.coerceIn(-1f, 1f) * 32767).toInt().toShort()

        // Same curve as GameNative: sqrt for an earlier trigger response.
        private fun trigger(v: Float): Short =
            ((sqrt(v.coerceIn(0f, 1f)) * 65534f).roundToInt() - 32767).toShort()

        private fun sdlButton(keyCode: Int): Int? = when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> 0
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK -> 1
            KeyEvent.KEYCODE_BUTTON_X -> 2
            KeyEvent.KEYCODE_BUTTON_Y -> 3
            KeyEvent.KEYCODE_BUTTON_SELECT -> 4
            KeyEvent.KEYCODE_BUTTON_MODE -> 5
            KeyEvent.KEYCODE_BUTTON_START -> 6
            KeyEvent.KEYCODE_BUTTON_THUMBL -> 7
            KeyEvent.KEYCODE_BUTTON_THUMBR -> 8
            KeyEvent.KEYCODE_BUTTON_L1 -> 9
            KeyEvent.KEYCODE_BUTTON_R1 -> 10
            KeyEvent.KEYCODE_DPAD_UP -> 11
            KeyEvent.KEYCODE_DPAD_DOWN -> 12
            KeyEvent.KEYCODE_DPAD_LEFT -> 13
            KeyEvent.KEYCODE_DPAD_RIGHT -> 14
            else -> null
        }
    }
}
