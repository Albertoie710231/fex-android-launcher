package com.mediatek.steamlauncher

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
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

    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }
    private val handler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private val outputBuffer = StringBuilder()
    private var lineCount = 0
    private var isRunning = false
    private var currentJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_terminal)

        tvOutput = findViewById(R.id.tvOutput)
        scrollView = findViewById(R.id.scrollView)
        etCommand = findViewById(R.id.etCommand)
        btnSend = findViewById(R.id.btnSend)

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
        val command = etCommand.text.toString().trim()
        if (command.isEmpty()) return
        if (isRunning) {
            appendOutput("[Busy - wait for current command to finish]\n")
            return
        }

        etCommand.text.clear()
        executeCommand(command)
    }

    private fun executeCommand(command: String) {
        if (isRunning) {
            appendOutput("[Busy - command queued]\n")
            return
        }

        appendOutput("$ $command\n")
        setRunning(true)

        currentJob = scope.launch {
            try {
                val env = buildEnvironment()

                // Execute command via FEXLoader
                val process = app.fexExecutor.execute(
                    command = command,
                    environment = env
                )

                if (process == null) {
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

                try {
                    while (isActive) {
                        Log.d(TAG, "Calling reader.read()...")
                        val count = reader.read(buffer)
                        Log.d(TAG, "reader.read() returned: $count")
                        if (count == -1) break

                        totalRead += count
                        val text = String(buffer, 0, count)
                        Log.d(TAG, "Read text (${text.length} chars): ${text.take(200)}")
                        withContext(Dispatchers.Main) {
                            appendOutput(text)
                        }
                    }
                } catch (e: Exception) {
                    if (isActive) {
                        Log.e(TAG, "Read error", e)
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

                withContext(Dispatchers.Main) {
                    appendOutput("\n")
                    setRunning(false)
                }

            } catch (e: CancellationException) {
                withContext(Dispatchers.Main) {
                    appendOutput("\n[Cancelled]\n")
                    setRunning(false)
                }
            } catch (e: Exception) {
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
        btnSend.isEnabled = !running
        btnSend.text = if (running) "..." else "Run"
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

            // Vortek Vulkan configuration
            "VORTEK_SERVER_PATH" to "/tmp/vortek.sock",
            "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/vortek_icd.json"
        )
    }

    override fun onDestroy() {
        currentJob?.cancel()
        scope.cancel()
        super.onDestroy()
    }
}
