package com.mediatek.steamlauncher

import android.content.SharedPreferences
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.preference.PreferenceManager
import com.mediatek.steamlauncher.databinding.ActivitySettingsBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Settings activity for configuring the Steam launcher.
 * Allows users to configure:
 * - Vulkan passthrough options
 * - Resolution and display settings
 * - Touch input mode
 * - Container management
 */
class SettingsActivity : AppCompatActivity() {

    private lateinit var binding: ActivitySettingsBinding
    private lateinit var prefs: SharedPreferences
    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)

        prefs = PreferenceManager.getDefaultSharedPreferences(this)

        setupToolbar()
        loadSettings()
        setupListeners()
        loadSystemInfo()
    }

    private fun setupToolbar() {
        setSupportActionBar(binding.toolbar)
        supportActionBar?.apply {
            setDisplayHomeAsUpEnabled(true)
            title = "Settings"
        }
    }

    private fun loadSettings() {
        binding.apply {
            // Display settings
            spinnerResolution.setSelection(
                prefs.getInt("resolution_preset", 0)
            )
            switchDxvkAsync.isChecked = prefs.getBoolean("dxvk_async", true)
            switchEsync.isChecked = prefs.getBoolean("esync_enabled", true)
            switchFsync.isChecked = prefs.getBoolean("fsync_enabled", true)

            // Input settings
            spinnerTouchMode.setSelection(
                prefs.getInt("touch_mode", 0)
            )
            switchGamepadSupport.isChecked = prefs.getBoolean("gamepad_enabled", true)

            // Performance settings
            switchBox64Dynarec.isChecked = prefs.getBoolean("box64_dynarec", true)
            switchBox86Dynarec.isChecked = prefs.getBoolean("box86_dynarec", true)

            // Debug settings
            switchVulkanValidation.isChecked = prefs.getBoolean("vulkan_validation", false)
            switchBox64Logging.isChecked = prefs.getBoolean("box64_logging", false)
        }
    }

    private fun setupListeners() {
        binding.apply {
            // Save button
            btnSave.setOnClickListener { saveSettings() }

            // Container management
            btnResetContainer.setOnClickListener { resetContainer() }
            btnInstallSteam.setOnClickListener { installSteam() }
            btnTestBox64.setOnClickListener { testBox64() }
            btnTestVulkan.setOnClickListener { testVulkan() }
            btnClearCache.setOnClickListener { clearCache() }

            // ADB phantom process workaround
            btnPhantomFix.setOnClickListener { showPhantomProcessFix() }
        }
    }

    private fun loadSystemInfo() {
        lifecycleScope.launch {
            val vulkanInfo = withContext(Dispatchers.IO) {
                VulkanBridge.getVulkanInfo(this@SettingsActivity)
            }

            val fexVersion = withContext(Dispatchers.IO) {
                app.fexExecutor.getFexVersion()
            }

            val containerStatus = withContext(Dispatchers.IO) {
                if (app.containerManager.isContainerReady()) {
                    "Installed"
                } else {
                    "Not installed"
                }
            }

            binding.apply {
                tvVulkanInfo.text = vulkanInfo
                tvProotVersion.text = "FEX: $fexVersion"
                tvContainerStatus.text = "Container: $containerStatus"
            }
        }
    }

    private fun saveSettings() {
        prefs.edit().apply {
            // Display
            putInt("resolution_preset", binding.spinnerResolution.selectedItemPosition)
            putBoolean("dxvk_async", binding.switchDxvkAsync.isChecked)
            putBoolean("esync_enabled", binding.switchEsync.isChecked)
            putBoolean("fsync_enabled", binding.switchFsync.isChecked)

            // Input
            putInt("touch_mode", binding.spinnerTouchMode.selectedItemPosition)
            putBoolean("gamepad_enabled", binding.switchGamepadSupport.isChecked)

            // Performance
            putBoolean("box64_dynarec", binding.switchBox64Dynarec.isChecked)
            putBoolean("box86_dynarec", binding.switchBox86Dynarec.isChecked)

            // Debug
            putBoolean("vulkan_validation", binding.switchVulkanValidation.isChecked)
            putBoolean("box64_logging", binding.switchBox64Logging.isChecked)

            apply()
        }

        Toast.makeText(this, "Settings saved", Toast.LENGTH_SHORT).show()
    }

    private fun resetContainer() {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Reset Container")
            .setMessage("This will delete the entire Linux container and all installed games. Are you sure?")
            .setPositiveButton("Reset") { _, _ ->
                lifecycleScope.launch {
                    withContext(Dispatchers.IO) {
                        app.containerManager.cleanup()
                    }
                    Toast.makeText(this@SettingsActivity, "Container reset. Please run setup again.", Toast.LENGTH_LONG).show()
                    loadSystemInfo()
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun installSteam() {
        if (!app.containerManager.isContainerReady()) {
            Toast.makeText(this, "Please set up the container first", Toast.LENGTH_SHORT).show()
            return
        }

        lifecycleScope.launch {
            binding.btnInstallSteam.isEnabled = false
            binding.btnInstallSteam.text = "Installing..."

            // Download and extract Steam entirely from Android side
            val result = withContext(Dispatchers.IO) {
                app.containerManager.downloadAndExtractSteam()
            }

            binding.btnInstallSteam.isEnabled = true
            binding.btnInstallSteam.text = "Install Steam"

            // Show result
            androidx.appcompat.app.AlertDialog.Builder(this@SettingsActivity)
                .setTitle("Steam Installation")
                .setMessage(result.take(2000))
                .setPositiveButton("OK", null)
                .show()
        }
    }

    private fun testBox64() {
        if (!app.containerManager.isContainerReady()) {
            Toast.makeText(this, "Container not ready. Run Setup first.", Toast.LENGTH_SHORT).show()
            return
        }

        lifecycleScope.launch {
            binding.btnTestBox64.isEnabled = false
            binding.btnTestBox64.text = "Testing..."

            val result = withContext(Dispatchers.IO) {
                val sb = StringBuilder()

                // Test FEX environment
                sb.appendLine("=== FEX-Emu Test ===")
                val fexResult = app.fexExecutor.executeBlocking(
                    "uname -a",
                    timeoutMs = 15000
                )
                sb.appendLine(fexResult.output.ifEmpty { "Exit code: ${fexResult.exitCode}" })

                // Check architecture
                sb.appendLine("\n=== Architecture ===")
                val archResult = app.fexExecutor.executeBlocking(
                    "echo \"Arch: \$(uname -m)\" && echo \"Kernel: \$(uname -r)\"",
                    timeoutMs = 10000
                )
                sb.appendLine(archResult.output.ifEmpty { "Exit code: ${archResult.exitCode}" })

                // Check basic tools
                sb.appendLine("\n=== Tools Check ===")
                val toolsResult = app.fexExecutor.executeBlocking(
                    "which bash tar ls cat && echo 'All tools found'",
                    timeoutMs = 10000
                )
                sb.appendLine(toolsResult.output.ifEmpty { "Exit code: ${toolsResult.exitCode}" })

                sb.toString()
            }

            binding.btnTestBox64.isEnabled = true
            binding.btnTestBox64.text = "Test Box64"

            androidx.appcompat.app.AlertDialog.Builder(this@SettingsActivity)
                .setTitle("FEX-Emu Test Results")
                .setMessage(result.take(3000))
                .setPositiveButton("OK", null)
                .show()
        }
    }

    private fun testVulkan() {
        if (!app.containerManager.isContainerReady()) {
            Toast.makeText(this, "Container not ready", Toast.LENGTH_SHORT).show()
            return
        }

        lifecycleScope.launch {
            binding.btnTestVulkan.isEnabled = false

            val result = withContext(Dispatchers.IO) {
                VulkanBridge.testVulkanInContainer(app.fexExecutor)
            }

            binding.btnTestVulkan.isEnabled = true

            androidx.appcompat.app.AlertDialog.Builder(this@SettingsActivity)
                .setTitle("Vulkan Test")
                .setMessage(result.take(2000))
                .setPositiveButton("OK", null)
                .show()
        }
    }

    private fun clearCache() {
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                cacheDir.deleteRecursively()
                cacheDir.mkdirs()
            }
            Toast.makeText(this@SettingsActivity, "Cache cleared", Toast.LENGTH_SHORT).show()
        }
    }

    private fun showPhantomProcessFix() {
        val adbCommand = """
            # Run these commands via ADB to prevent Android 12+ from killing background processes:

            adb shell "settings put global settings_enable_monitor_phantom_procs false"

            # Or set max phantom processes to a high value:
            adb shell "/system/bin/device_config set_sync_disabled_for_tests persistent"
            adb shell "/system/bin/device_config put activity_manager max_phantom_processes 2147483647"

            # Verify:
            adb shell "/system/bin/dumpsys activity settings | grep max_phantom"
        """.trimIndent()

        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Phantom Process Fix")
            .setMessage("Android 12+ kills background processes aggressively. Run these ADB commands to fix:\n\n$adbCommand")
            .setPositiveButton("Copy") { _, _ ->
                val clipboard = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
                val clip = android.content.ClipData.newPlainText("ADB Commands", adbCommand)
                clipboard.setPrimaryClip(clip)
                Toast.makeText(this, "Commands copied to clipboard", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("Close", null)
            .show()
    }

    override fun onSupportNavigateUp(): Boolean {
        onBackPressedDispatcher.onBackPressed()
        return true
    }
}
