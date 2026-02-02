package com.mediatek.steamlauncher

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.mediatek.steamlauncher.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.all { it.value }
        if (allGranted) {
            checkContainerStatus()
        } else {
            showPermissionDeniedDialog()
        }
    }

    private val manageStorageLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (Environment.isExternalStorageManager()) {
                checkContainerStatus()
            } else {
                showPermissionDeniedDialog()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupUI()
        checkPermissions()
    }

    private fun setupUI() {
        binding.apply {
            btnLaunchSteam.setOnClickListener { launchSteam() }
            btnSetup.setOnClickListener { startContainerSetup() }
            btnSettings.setOnClickListener { openSettings() }
            btnTerminal.setOnClickListener { openTerminal() }

            // Initially disable launch button until container is ready
            btnLaunchSteam.isEnabled = false
        }
    }

    private fun checkPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                showStoragePermissionDialog()
                return
            }
        } else {
            val permissions = arrayOf(
                Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            )
            val notGranted = permissions.filter {
                ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
            }
            if (notGranted.isNotEmpty()) {
                requestPermissionLauncher.launch(notGranted.toTypedArray())
                return
            }
        }

        // Check notification permission for Android 13+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(
                    this,
                    Manifest.permission.POST_NOTIFICATIONS
                ) != PackageManager.PERMISSION_GRANTED
            ) {
                requestPermissionLauncher.launch(arrayOf(Manifest.permission.POST_NOTIFICATIONS))
                return
            }
        }

        checkContainerStatus()
    }

    private fun showStoragePermissionDialog() {
        AlertDialog.Builder(this)
            .setTitle("Storage Permission Required")
            .setMessage("This app needs full storage access to manage the Linux container and game files.")
            .setPositiveButton("Grant Permission") { _, _ ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                        data = Uri.parse("package:$packageName")
                    }
                    manageStorageLauncher.launch(intent)
                }
            }
            .setNegativeButton("Cancel") { _, _ ->
                showPermissionDeniedDialog()
            }
            .setCancelable(false)
            .show()
    }

    private fun showPermissionDeniedDialog() {
        AlertDialog.Builder(this)
            .setTitle("Permission Required")
            .setMessage("Storage permission is required for the app to function. Please grant the permission in Settings.")
            .setPositiveButton("OK") { _, _ -> finish() }
            .setCancelable(false)
            .show()
    }

    private fun checkContainerStatus() {
        lifecycleScope.launch {
            updateStatus("Checking container status...")

            val isReady = withContext(Dispatchers.IO) {
                app.containerManager.isContainerReady()
            }

            if (isReady) {
                updateStatus("Container ready")
                binding.btnLaunchSteam.isEnabled = true
                binding.btnSetup.text = "Reinstall Container"
                checkVulkanSupport()
            } else {
                updateStatus("Container not installed")
                binding.btnLaunchSteam.isEnabled = false
                binding.btnSetup.text = "Setup Container"
            }
        }
    }

    private fun checkVulkanSupport() {
        lifecycleScope.launch {
            val vulkanInfo = withContext(Dispatchers.IO) {
                VulkanBridge.getVulkanInfo(this@MainActivity)
            }

            binding.tvVulkanInfo.text = vulkanInfo
            Log.i(TAG, "Vulkan info: $vulkanInfo")
        }
    }

    private fun startContainerSetup() {
        lifecycleScope.launch {
            binding.apply {
                btnSetup.isEnabled = false
                btnLaunchSteam.isEnabled = false
                progressBar.visibility = View.VISIBLE
            }

            try {
                app.containerManager.setupContainer(
                    progressCallback = { progress, message ->
                        runOnUiThread {
                            binding.progressBar.progress = progress
                            updateStatus(message)
                        }
                    }
                )

                updateStatus("Container setup complete!")
                binding.btnLaunchSteam.isEnabled = true
                checkVulkanSupport()

            } catch (e: Exception) {
                Log.e(TAG, "Container setup failed", e)
                updateStatus("Setup failed: ${e.message}")
                Toast.makeText(this@MainActivity, "Setup failed: ${e.message}", Toast.LENGTH_LONG).show()
            } finally {
                binding.apply {
                    btnSetup.isEnabled = true
                    progressBar.visibility = View.GONE
                }
            }
        }
    }

    private fun launchSteam() {
        if (!app.containerManager.isContainerReady()) {
            Toast.makeText(this, "Container not ready. Please run setup first.", Toast.LENGTH_SHORT).show()
            return
        }

        // Start foreground service first
        val serviceIntent = Intent(this, SteamService::class.java).apply {
            action = SteamService.ACTION_START_STEAM
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent)
        } else {
            startService(serviceIntent)
        }

        // Launch game activity
        val gameIntent = Intent(this, GameActivity::class.java)
        startActivity(gameIntent)
    }

    private fun openSettings() {
        val intent = Intent(this, SettingsActivity::class.java)
        startActivity(intent)
    }

    private fun openTerminal() {
        if (!app.containerManager.isContainerReady()) {
            Toast.makeText(this, "Container not ready", Toast.LENGTH_SHORT).show()
            return
        }

        // Start service with terminal action
        val serviceIntent = Intent(this, SteamService::class.java).apply {
            action = SteamService.ACTION_START_TERMINAL
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent)
        } else {
            startService(serviceIntent)
        }

        val gameIntent = Intent(this, GameActivity::class.java).apply {
            putExtra("mode", "terminal")
        }
        startActivity(gameIntent)
    }

    private fun updateStatus(message: String) {
        binding.tvStatus.text = message
        Log.d(TAG, "Status: $message")
    }

    override fun onResume() {
        super.onResume()
        if (::binding.isInitialized) {
            checkContainerStatus()
        }
    }

    companion object {
        private const val TAG = "MainActivity"
    }
}
