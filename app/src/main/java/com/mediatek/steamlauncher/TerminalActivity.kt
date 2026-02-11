package com.mediatek.steamlauncher

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.*

/**
 * Interactive terminal for running commands in the FEX x86-64 environment.
 * Executes each command separately via FEXLoader for reliability.
 */
class TerminalActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "TerminalActivity"
        private const val MAX_OUTPUT_LINES = 500
    }

    private lateinit var tvOutput: TextView
    private lateinit var scrollView: ScrollView
    private lateinit var etCommand: EditText
    private lateinit var btnSend: Button
    private lateinit var vulkanSurface: SurfaceView
    private lateinit var btnDisplay: Button

    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }
    private val handler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private val outputBuffer = StringBuilder()
    private var lineCount = 0
    private var isRunning = false
    private var currentJob: Job? = null
    private var currentProcess: Process? = null
    private var currentDir: String = ""  // initialized in onCreate from app.getFexHomeDir()
    private var frameSocketServer: FrameSocketServer? = null
    private var isDisplayMode = false
    private var surfaceReady = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_terminal)

        tvOutput = findViewById(R.id.tvOutput)
        scrollView = findViewById(R.id.scrollView)
        etCommand = findViewById(R.id.etCommand)
        btnSend = findViewById(R.id.btnSend)
        vulkanSurface = findViewById(R.id.vulkanSurface)
        btnDisplay = findViewById(R.id.btnDisplay)

        currentDir = app.getFexHomeDir()

        // Refresh paths that go stale after APK reinstall (nativeLibDir changes)
        app.containerManager.refreshNativeLibPaths()

        // Start VortekRenderer for Vulkan passthrough (doesn't need a surface)
        startVortekRenderer()

        // Start frame socket server for vkcube frame capture
        startFrameSocketServer()

        // Request 120Hz display refresh rate
        request120Hz()

        // Wire up SurfaceView for real-time Vulkan display
        vulkanSurface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                // Request 120Hz on the surface itself
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    holder.surface.setFrameRate(120f, android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
                }
                if (isDisplayMode) {
                    frameSocketServer?.setOutputSurface(holder.surface)
                    Log.i(TAG, "Vulkan display surface created and connected")
                }
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                if (isDisplayMode) {
                    frameSocketServer?.setOutputSurface(holder.surface)
                    Log.i(TAG, "Vulkan display surface changed: ${width}x${height}")
                }
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                frameSocketServer?.setOutputSurface(null)
                Log.i(TAG, "Vulkan display surface destroyed")
            }
        })

        setupUI()
        showWelcome()
    }

    private fun setupUI() {
        // Send button
        btnSend.setOnClickListener {
            sendCommand()
        }

        // Enter key sends command
        etCommand.setOnEditorActionListener { _, actionId, event ->
            if (actionId == EditorInfo.IME_ACTION_SEND ||
                (event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)) {
                sendCommand()
                true
            } else {
                false
            }
        }

        // Display toggle button
        btnDisplay.setOnClickListener {
            toggleDisplayMode()
        }

        // Clear button
        findViewById<Button>(R.id.btnClear).setOnClickListener {
            outputBuffer.clear()
            lineCount = 0
            tvOutput.text = ""
            showWelcome()
        }

        // Close button
        findViewById<Button>(R.id.btnClose).setOnClickListener {
            finish()
        }

        // Setup FEX button - runs full container setup
        findViewById<Button>(R.id.btnSetupFex).setOnClickListener {
            if (isRunning) {
                appendOutput("[Busy - wait for current command to finish]\n")
                return@setOnClickListener
            }
            setRunning(true)
            appendOutput("=== Setting up FEX-Emu Container ===\n")

            scope.launch {
                try {
                    app.containerManager.setupContainer { progress, message ->
                        handler.post {
                            appendOutput("[$progress%] $message\n")
                        }
                    }
                    handler.post {
                        appendOutput("\n=== Container Setup Complete! ===\n")
                        appendOutput("Try: uname -a\n\n")
                        setRunning(false)
                    }
                } catch (e: Exception) {
                    handler.post {
                        appendOutput("\n[ERROR: ${e.message}]\n")
                        setRunning(false)
                    }
                }
            }
        }

        findViewById<Button>(R.id.btnTestSem).setOnClickListener {
            executeCommand("echo 'Testing semaphores...' && python3 -c \"import multiprocessing; s = multiprocessing.Semaphore(1); s.acquire(); s.release(); print('Semaphore OK')\" 2>/dev/null || echo 'python3 not available, trying C test...'")
        }

        findViewById<Button>(R.id.btnVulkanInfo).setOnClickListener {
            executeCommand("VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json vulkaninfo --summary 2>&1 | head -50")
        }

        findViewById<Button>(R.id.btnLs).setOnClickListener {
            executeCommand("ls -la")
        }

        findViewById<Button>(R.id.btnPwd).setOnClickListener {
            executeCommand("echo \"PWD: \$(pwd)\" && echo \"HOME: \$HOME\" && echo \"USER: \$USER\" && echo \"ARCH: \$(uname -m)\"")
        }

        // Seccomp test — runs native ARM64 binary directly (NOT through FEX)
        // to identify which syscalls Android's seccomp filter blocks
        findViewById<Button>(R.id.btnSeccomp).setOnClickListener {
            val nativeLibDir = applicationInfo.nativeLibraryDir
            val testBinary = "$nativeLibDir/libseccomp_test.so"
            val ldsoPath = "$nativeLibDir/libld_linux_aarch64.so"
            executeNativeCommand(testBinary, mapOf("SECCOMP_TEST_LDSO" to ldsoPath))
        }

        // FEXServer diagnostic — tests each init step to find what fails
        findViewById<Button>(R.id.btnFexDiag).setOnClickListener {
            val nativeLibDir = applicationInfo.nativeLibraryDir
            val diagBinary = "$nativeLibDir/libfexserver_diag.so"
            val fexHomeDir = (application as SteamLauncherApp).getFexHomeDir()
            val tmpDir = (application as SteamLauncherApp).getTmpDir()
            executeNativeCommand(diagBinary, mapOf(
                "HOME" to fexHomeDir,
                "TMPDIR" to tmpDir
            ))
        }
    }

    private fun toggleDisplayMode() {
        isDisplayMode = !isDisplayMode

        if (isDisplayMode) {
            scrollView.visibility = View.GONE
            vulkanSurface.visibility = View.VISIBLE
            btnDisplay.text = "Terminal"

            // Connect surface if already ready
            if (surfaceReady) {
                frameSocketServer?.setOutputSurface(vulkanSurface.holder.surface)
            }
            Log.i(TAG, "Switched to Vulkan display mode")
        } else {
            vulkanSurface.visibility = View.GONE
            scrollView.visibility = View.VISIBLE
            btnDisplay.text = "Display"

            // Disconnect surface
            frameSocketServer?.setOutputSurface(null)
            Log.i(TAG, "Switched to terminal mode")
        }
    }

    private fun showWelcome() {
        appendOutput("=== FEX Terminal (x86-64) ===\n")
        appendOutput("Commands run in FEX-Emu x86-64 environment.\n")
        appendOutput("\n")
        appendOutput("Quick start:\n")
        appendOutput("  1. Press 'Setup FEX' to install the container\n")
        appendOutput("  2. Type 'uname -a' to verify x86-64 environment\n")
        appendOutput("  3. Press 'Vulkan Info' to test GPU passthrough\n")
        appendOutput("\n")
    }

    private fun sendCommand() {
        val text = etCommand.text.toString()
        if (text.isEmpty()) return

        etCommand.text.clear()

        if (isRunning && currentProcess != null) {
            // Pipe input to running process's stdin
            appendOutput("$text\n")
            scope.launch(Dispatchers.IO) {
                try {
                    currentProcess?.outputStream?.let { stdin ->
                        stdin.write((text + "\n").toByteArray())
                        stdin.flush()
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to write to stdin", e)
                }
            }
            return
        }

        executeCommand(text.trim())
    }

    private fun executeCommand(command: String) {
        if (isRunning) {
            appendOutput("[Busy - command queued]\n")
            return
        }

        appendOutput("$ $command\n")
        setRunning(true)

        // Detect cd commands to update working directory
        val isCdCommand = command.trim().let {
            it == "cd" || it.startsWith("cd ") || it.startsWith("cd\t")
        }

        // Wrap command: cd to current dir first, then run command.
        // If it's a cd command, also emit pwd at the end so we can capture the new dir.
        val wrappedCommand = if (isCdCommand) {
            "cd '$currentDir' && $command && pwd"
        } else {
            "cd '$currentDir' && $command"
        }

        currentJob = scope.launch {
            try {
                val env = buildEnvironment()

                // Execute command via FEXLoader
                val process = app.fexExecutor.execute(
                    command = wrappedCommand,
                    environment = env
                )
                currentProcess = process

                if (process == null) {
                    currentProcess = null
                    withContext(Dispatchers.Main) {
                        appendOutput("[ERROR: Failed to start process]\n")
                        setRunning(false)
                    }
                    return@launch
                }

                // Read output
                Log.d(TAG, "Starting to read process output...")
                val reader = process.inputStream.bufferedReader()
                val buffer = CharArray(1024)
                var totalRead = 0
                val fullOutput = StringBuilder()

                try {
                    while (isActive) {
                        Log.d(TAG, "Calling reader.read()...")
                        val count = reader.read(buffer)
                        Log.d(TAG, "reader.read() returned: $count")
                        if (count == -1) break

                        totalRead += count
                        val text = String(buffer, 0, count)
                        fullOutput.append(text)
                        Log.d(TAG, "Read text (${text.length} chars): ${text.take(200)}")

                        // For cd commands, don't show the trailing pwd output
                        if (!isCdCommand) {
                            withContext(Dispatchers.Main) {
                                appendOutput(text)
                            }
                        }
                    }
                } catch (e: Exception) {
                    if (isActive) {
                        Log.e(TAG, "Read error", e)
                    }
                }

                // For cd commands, extract the new directory from pwd output
                if (isCdCommand) {
                    val outputStr = fullOutput.toString()
                    // Find the last non-empty line before [FEX exit code: 0]
                    val lines = outputStr.lines()
                        .map { it.trim() }
                        .filter { it.isNotEmpty() && !it.startsWith("[FEX exit code:") }
                    val newDir = lines.lastOrNull()
                    if (newDir != null && newDir.startsWith("/")) {
                        currentDir = newDir
                        withContext(Dispatchers.Main) {
                            appendOutput("$currentDir\n")
                        }
                    } else {
                        // cd failed — show the output as-is
                        withContext(Dispatchers.Main) {
                            appendOutput(outputStr)
                        }
                    }
                }

                Log.d(TAG, "Read loop finished. Total bytes read: $totalRead")

                // Wait for process to complete
                val exitCode = withTimeoutOrNull(600_000) {
                    withContext(Dispatchers.IO) {
                        process.waitFor()
                    }
                }
                Log.d(TAG, "Process exit code: $exitCode")
                val completed = exitCode

                if (completed == null) {
                    process.destroyForcibly()
                    withContext(Dispatchers.Main) {
                        appendOutput("\n[Command timed out after 10min]\n")
                    }
                }

                currentProcess = null
                withContext(Dispatchers.Main) {
                    appendOutput("\n")
                    setRunning(false)
                }

            } catch (e: CancellationException) {
                currentProcess = null
                withContext(Dispatchers.Main) {
                    appendOutput("\n[Cancelled]\n")
                    setRunning(false)
                }
            } catch (e: Exception) {
                currentProcess = null
                Log.e(TAG, "Command execution failed", e)
                withContext(Dispatchers.Main) {
                    appendOutput("\n[ERROR: ${e.message}]\n")
                    setRunning(false)
                }
            }
        }
    }

    /**
     * Execute a native ARM64 binary directly (not through FEX).
     * Used for diagnostics that need to run in the app's seccomp context.
     */
    private fun executeNativeCommand(binaryPath: String, env: Map<String, String> = emptyMap()) {
        if (isRunning) {
            appendOutput("[Busy - wait for current command to finish]\n")
            return
        }

        appendOutput("$ [native] $binaryPath\n")
        setRunning(true)

        currentJob = scope.launch {
            try {
                val processBuilder = ProcessBuilder(binaryPath).apply {
                    redirectErrorStream(true)
                    environment().putAll(env)
                }

                val process = processBuilder.start()
                val reader = process.inputStream.bufferedReader()
                val buffer = CharArray(1024)

                try {
                    while (isActive) {
                        val count = reader.read(buffer)
                        if (count == -1) break
                        val text = String(buffer, 0, count)
                        withContext(Dispatchers.Main) {
                            appendOutput(text)
                        }
                    }
                } catch (e: Exception) {
                    if (isActive) Log.e(TAG, "Native read error", e)
                }

                val exitCode = withTimeoutOrNull(30_000) {
                    withContext(Dispatchers.IO) { process.waitFor() }
                }

                if (exitCode == null) {
                    process.destroyForcibly()
                    withContext(Dispatchers.Main) { appendOutput("\n[Timed out]\n") }
                }

                withContext(Dispatchers.Main) {
                    appendOutput("\n")
                    setRunning(false)
                }
            } catch (e: Exception) {
                Log.e(TAG, "Native command failed", e)
                withContext(Dispatchers.Main) {
                    appendOutput("\n[ERROR: ${e.message}]\n")
                    setRunning(false)
                }
            }
        }
    }

    private fun setRunning(running: Boolean) {
        isRunning = running
        btnSend.text = if (running) "Send" else "Run"
    }

    private fun appendOutput(text: String) {
        lineCount += text.count { it == '\n' }

        if (lineCount > MAX_OUTPUT_LINES) {
            val lines = outputBuffer.toString().lines()
            val trimmedLines = lines.takeLast(MAX_OUTPUT_LINES / 2)
            outputBuffer.clear()
            outputBuffer.append("[...output trimmed...]\n")
            outputBuffer.append(trimmedLines.joinToString("\n"))
            lineCount = trimmedLines.size + 1
        }

        outputBuffer.append(text)
        tvOutput.text = outputBuffer.toString()

        handler.post {
            scrollView.fullScroll(ScrollView.FOCUS_DOWN)
        }
    }

    private fun buildEnvironment(): Map<String, String> {
        return mapOf(
            "DISPLAY" to ":0",
            "TERM" to "dumb",
            "PATH" to "/usr/local/bin:/usr/bin:/bin",
            "XDG_RUNTIME_DIR" to "/tmp",
            "TMPDIR" to "/tmp",
            "DEBIAN_FRONTEND" to "noninteractive",

            // Vortek Vulkan configuration (guest paths — FEX maps /tmp → rootfs/tmp/)
            "VORTEK_SERVER_PATH" to "/tmp/vortek.sock",
            "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/vortek_icd.json"
        )
    }

    /**
     * Start VortekRenderer for Vulkan passthrough.
     * Unlike SteamService, the Terminal doesn't have a surface, but VortekRenderer
     * doesn't need one — it just needs a socket path and context.
     */
    private fun request120Hz() {
        // Find a 120Hz display mode and set it on the window
        val display = if (android.os.Build.VERSION.SDK_INT >= 30) {
            this.display
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay
        } ?: return

        val modes = display.supportedModes
        val mode120 = modes.filter { it.refreshRate >= 119f }
            .maxByOrNull { it.refreshRate }

        if (mode120 != null) {
            window.attributes = window.attributes.apply {
                preferredDisplayModeId = mode120.modeId
            }
            Log.i(TAG, "Requested 120Hz display mode: ${mode120.physicalWidth}x${mode120.physicalHeight}@${mode120.refreshRate}")
        } else {
            Log.i(TAG, "120Hz not available. Modes: ${modes.map { "${it.refreshRate}Hz" }}")
        }
    }

    private fun startVortekRenderer() {
        if (!VortekRenderer.loadLibrary()) {
            Log.w(TAG, "Vortek renderer not available — Vulkan passthrough disabled")
            return
        }

        if (VortekRenderer.isRunning()) {
            Log.d(TAG, "VortekRenderer already running")
            return
        }

        val socketPath = "${app.getTmpDir()}/vortek.sock"

        // Delete stale socket
        val socketFile = java.io.File(socketPath)
        if (socketFile.exists()) {
            socketFile.delete()
        }

        if (VortekRenderer.start(socketPath, this)) {
            Log.i(TAG, "VortekRenderer started at: $socketPath")

            // Set a dummy WindowInfoProvider for headless Vulkan (e.g., vulkaninfo).
            // Without this, VortekRenderer rejects client connections.
            VortekRenderer.setWindowInfoProvider(object : com.winlator.xenvironment.components.VortekRendererComponent.WindowInfoProvider {
                override fun getWindowWidth(windowId: Int): Int = 1920
                override fun getWindowHeight(windowId: Int): Int = 1080
                override fun getWindowHardwareBuffer(windowId: Int): Long = 0
                override fun updateWindowContent(windowId: Int) {}
            })

            // Create .vortek/V0 symlink (expected by VORTEK_SERVER_PATH env var)
            try {
                val vortekDir = java.io.File(app.getTmpDir(), ".vortek")
                vortekDir.mkdirs()
                val symlinkFile = java.io.File(vortekDir, "V0")
                if (symlinkFile.exists()) {
                    symlinkFile.delete()
                }
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", "../vortek.sock", symlinkFile.absolutePath)
                ).waitFor()
                Log.i(TAG, "Created Vortek symlink: ${symlinkFile.absolutePath} -> ../vortek.sock")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to create Vortek symlink", e)
            }

            // Create V0 symlink in app data dir — libvulkan_vortek.so has hardcoded
            // path /data/data/com.mediatek.steamlauncher/V0
            try {
                val appDataDir = java.io.File(applicationInfo.dataDir)
                val v0Symlink = java.io.File(appDataDir, "V0")
                if (v0Symlink.exists()) {
                    v0Symlink.delete()
                }
                Runtime.getRuntime().exec(
                    arrayOf("ln", "-sf", socketPath, v0Symlink.absolutePath)
                ).waitFor()
                Log.i(TAG, "Created V0 symlink: ${v0Symlink.absolutePath} -> $socketPath")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to create V0 symlink in app data dir", e)
            }
        } else {
            Log.e(TAG, "Failed to start VortekRenderer")
        }
    }

    private fun startFrameSocketServer() {
        if (frameSocketServer != null) return
        frameSocketServer = FrameSocketServer().apply {
            if (start()) {
                Log.i(TAG, "Frame socket server started on TCP 19850")
            } else {
                Log.e(TAG, "Failed to start frame socket server")
            }
        }
    }

    override fun onDestroy() {
        currentProcess?.destroyForcibly()
        currentProcess = null
        currentJob?.cancel()
        scope.cancel()
        frameSocketServer?.setOutputSurface(null)
        frameSocketServer?.stop()
        frameSocketServer = null
        VortekRenderer.stop()
        super.onDestroy()
    }
}
