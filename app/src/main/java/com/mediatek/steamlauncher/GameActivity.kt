package com.mediatek.steamlauncher

import android.annotation.SuppressLint
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import android.view.*
import android.widget.PopupMenu
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.mediatek.steamlauncher.databinding.ActivityGameBinding

/**
 * Full-screen activity that displays the X11 content and handles game input.
 * This activity:
 * - Shows the LorieView for X11 rendering
 * - Handles touch/keyboard/gamepad input
 * - Provides on-screen controls overlay
 * - Manages the connection to SteamService
 */
class GameActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "GameActivity"
    }

    private lateinit var binding: ActivityGameBinding
    private var steamService: SteamService? = null
    private var isBound = false

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as SteamService.LocalBinder
            steamService = binder.getService()
            isBound = true

            // Attach X11 server to view
            steamService?.getX11Server()?.let { x11Server ->
                binding.lorieView.x11Server = x11Server
            }

            Log.i(TAG, "Connected to SteamService")
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            steamService = null
            isBound = false
            Log.i(TAG, "Disconnected from SteamService")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Enable immersive fullscreen
        enableFullscreen()

        binding = ActivityGameBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupUI()
        bindToService()
    }

    private fun enableFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)

        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

        // Keep screen on
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupUI() {
        // Configure LorieView
        binding.lorieView.apply {
            touchMode = LorieView.TouchMode.MOUSE
        }

        // Menu button
        binding.btnMenu.setOnClickListener { showMenu(it) }

        // Keyboard toggle
        binding.btnKeyboard.setOnClickListener { toggleKeyboard() }

        // Mouse mode toggle
        binding.btnMouseMode.setOnClickListener { toggleMouseMode() }

        // Back button / exit
        binding.btnBack.setOnClickListener { confirmExit() }

        // Multi-touch gesture detection for right-click
        setupGestureDetection()
    }

    private fun setupGestureDetection() {
        val gestureDetector = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onLongPress(e: MotionEvent) {
                // Long press = right click
                binding.lorieView.performRightClick()
            }

            override fun onDoubleTap(e: MotionEvent): Boolean {
                // Double tap = double click (already handled by touch events)
                return true
            }
        })

        binding.lorieView.setOnTouchListener { v, event ->
            gestureDetector.onTouchEvent(event)
            false // Let the view handle the event too
        }
    }

    private fun showMenu(anchor: View) {
        PopupMenu(this, anchor).apply {
            menu.add(0, 1, 0, "Touch Mode: Mouse")
            menu.add(0, 2, 0, "Touch Mode: Direct")
            menu.add(0, 3, 0, "Touch Mode: Trackpad")
            menu.add(0, 4, 0, "Show Keyboard")
            menu.add(0, 5, 0, "Send Ctrl+Alt+Del")
            menu.add(0, 6, 0, "Send Escape")
            menu.add(0, 7, 0, "Exit")

            setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    1 -> binding.lorieView.touchMode = LorieView.TouchMode.MOUSE
                    2 -> binding.lorieView.touchMode = LorieView.TouchMode.DIRECT
                    3 -> binding.lorieView.touchMode = LorieView.TouchMode.TRACKPAD
                    4 -> toggleKeyboard()
                    5 -> sendCtrlAltDel()
                    6 -> sendEscape()
                    7 -> confirmExit()
                }
                true
            }
            show()
        }
    }

    private fun toggleKeyboard() {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager
        binding.lorieView.requestFocus()
        imm.toggleSoftInput(android.view.inputmethod.InputMethodManager.SHOW_FORCED, 0)
    }

    private fun toggleMouseMode() {
        binding.lorieView.touchMode = when (binding.lorieView.touchMode) {
            LorieView.TouchMode.MOUSE -> LorieView.TouchMode.TRACKPAD
            LorieView.TouchMode.TRACKPAD -> LorieView.TouchMode.DIRECT
            LorieView.TouchMode.DIRECT -> LorieView.TouchMode.MOUSE
        }

        val modeName = binding.lorieView.touchMode.name.lowercase()
        binding.btnMouseMode.text = modeName.replaceFirstChar { it.uppercase() }
    }

    private fun sendCtrlAltDel() {
        binding.lorieView.x11Server?.let { server ->
            // Ctrl
            server.sendKeyEvent(37, true)
            // Alt
            server.sendKeyEvent(64, true)
            // Delete
            server.sendKeyEvent(119, true)

            // Release in reverse order
            server.sendKeyEvent(119, false)
            server.sendKeyEvent(64, false)
            server.sendKeyEvent(37, false)
        }
    }

    private fun sendEscape() {
        binding.lorieView.x11Server?.let { server ->
            server.sendKeyEvent(9, true)
            server.sendKeyEvent(9, false)
        }
    }

    private fun confirmExit() {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Exit")
            .setMessage("Are you sure you want to exit? Steam will continue running in the background.")
            .setPositiveButton("Exit") { _, _ -> finish() }
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Stop Steam") { _, _ ->
                stopService(Intent(this, SteamService::class.java))
                finish()
            }
            .show()
    }

    private fun bindToService() {
        Intent(this, SteamService::class.java).also { intent ->
            bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        // Handle back button specially
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            confirmExit()
            return true
        }

        // Forward other keys to LorieView
        return binding.lorieView.onKeyDown(keyCode, event!!) || super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return true
        }
        return binding.lorieView.onKeyUp(keyCode, event!!) || super.onKeyUp(keyCode, event)
    }

    override fun onGenericMotionEvent(event: MotionEvent?): Boolean {
        event?.let {
            if (binding.lorieView.onGenericMotionEvent(it)) {
                return true
            }
        }
        return super.onGenericMotionEvent(event)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            enableFullscreen()
        }
    }

    override fun onResume() {
        super.onResume()
        enableFullscreen()
        binding.lorieView.requestFocus()
    }

    override fun onDestroy() {
        if (isBound) {
            unbindService(serviceConnection)
            isBound = false
        }
        super.onDestroy()
    }
}
