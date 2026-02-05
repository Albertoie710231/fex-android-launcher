package com.mediatek.steamlauncher

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.Toast
import androidx.annotation.RequiresApi
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.termux.x11.LorieView

/**
 * Full-screen activity that displays X11 content via LorieView.
 */
class GameActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "GameActivity"
    }

    private lateinit var lorieView: LorieView
    private lateinit var vulkanSurfaceView: SurfaceView
    private var steamService: SteamService? = null
    private var isBound = false
    private val handler = Handler(Looper.getMainLooper())

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as SteamService.LocalBinder
            steamService = binder.getService()
            isBound = true
            Log.i(TAG, "Connected to SteamService")

            // If surface is already ready, notify service immediately
            if (lorieView.isSurfaceReady) {
                Log.i(TAG, "Surface already ready when service connected, notifying")

                // Pass the surface for Vortek rendering
                val holder = lorieView.holder
                val rect = holder.surfaceFrame
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    steamService?.setOutputSurface(holder.surface, rect.width(), rect.height())
                }

                steamService?.onSurfaceReady()
            }

            // Set Vulkan frame surface if ready
            if (vulkanSurfaceView.holder.surface.isValid) {
                steamService?.setVulkanFrameSurface(vulkanSurfaceView.holder.surface)
            }

            waitForX11Server()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            steamService = null
            isBound = false
        }
    }

    private fun waitForX11Server(attempts: Int = 0) {
        val x11Server = steamService?.getX11Server()
        if (x11Server != null && x11Server.isRunning()) {
            Log.i(TAG, "X11 server ready, attaching surface")
            x11Server.attachSurface(lorieView)
        } else if (attempts < 30) {
            Log.d(TAG, "Waiting for X11 server... attempt ${attempts + 1}")
            handler.postDelayed({ waitForX11Server(attempts + 1) }, 300)
        } else {
            Log.e(TAG, "Timeout waiting for X11 server")
            Toast.makeText(this, "X11 server failed to start", Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableFullscreen()
        setContentView(R.layout.activity_game)

        lorieView = findViewById(R.id.lorieView)
        lorieView.touchMode = LorieView.TouchMode.MOUSE

        // Set up Vulkan frame rendering surface
        vulkanSurfaceView = findViewById(R.id.vulkanSurfaceView)
        vulkanSurfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.i(TAG, "Vulkan surface created")
                steamService?.setVulkanFrameSurface(holder.surface)
                // Make visible when we have a surface
                vulkanSurfaceView.visibility = View.VISIBLE
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.i(TAG, "Vulkan surface changed: ${width}x${height}")
                steamService?.setVulkanFrameSurface(holder.surface)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.i(TAG, "Vulkan surface destroyed")
                steamService?.setVulkanFrameSurface(null)
            }
        })
        // Start visible to trigger surface creation
        vulkanSurfaceView.visibility = View.VISIBLE

        // Set up the MainActivity stub with our LorieView
        // Native libXlorie code looks for MainActivity.getInstance().lorieView
        com.termux.x11.MainActivity.instance = com.termux.x11.MainActivity()
        com.termux.x11.MainActivity.setLorieView(lorieView)
        com.termux.x11.MainActivity.win = window
        Log.i(TAG, "MainActivity stub configured with LorieView")

        // Set up surface ready callback to notify service when we're ready to render
        lorieView.setSurfaceReadyCallback { width, height ->
            Log.i(TAG, "Surface ready: ${width}x${height}, notifying service")

            // Get the Surface from LorieView's holder for Vortek rendering
            val surface = lorieView.holder.surface
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                steamService?.setOutputSurface(surface, width, height)
            }

            steamService?.onSurfaceReady()
        }

        setupControls()
        bindToService()
    }

    private fun enableFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)

        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
    }

    private fun setupControls() {
        findViewById<Button>(R.id.btnKeyboard).setOnClickListener {
            toggleKeyboard()
        }

        findViewById<Button>(R.id.btnMouseMode).setOnClickListener {
            toggleMouseMode()
        }

        findViewById<Button>(R.id.btnExit).setOnClickListener {
            confirmExit()
        }
    }

    private fun toggleKeyboard() {
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        lorieView.requestFocus()
        imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0)
    }

    private fun toggleMouseMode() {
        lorieView.touchMode = when (lorieView.touchMode) {
            LorieView.TouchMode.MOUSE -> LorieView.TouchMode.TRACKPAD
            LorieView.TouchMode.TRACKPAD -> LorieView.TouchMode.DIRECT
            LorieView.TouchMode.DIRECT -> LorieView.TouchMode.MOUSE
        }

        val btn = findViewById<Button>(R.id.btnMouseMode)
        btn.text = lorieView.touchMode.name.lowercase().replaceFirstChar { it.uppercase() }
        Toast.makeText(this, "Mode: ${lorieView.touchMode.name}", Toast.LENGTH_SHORT).show()
    }

    private fun confirmExit() {
        AlertDialog.Builder(this)
            .setTitle("Exit")
            .setMessage("Stop the container and exit?")
            .setPositiveButton("Exit") { _, _ ->
                stopService(Intent(this, SteamService::class.java))
                finish()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun bindToService() {
        Intent(this, SteamService::class.java).also { intent ->
            bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            confirmExit()
            return true
        }
        return lorieView.onKeyDown(keyCode, event!!) || super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) return true
        return lorieView.onKeyUp(keyCode, event!!) || super.onKeyUp(keyCode, event)
    }

    override fun onResume() {
        super.onResume()
        enableFullscreen()
        lorieView.requestFocus()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)

        // Clear the surface reference in the service
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            steamService?.setOutputSurface(null, 0, 0)
        }

        if (isBound) {
            unbindService(serviceConnection)
            isBound = false
        }
        super.onDestroy()
    }
}
