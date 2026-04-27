package com.mediatek.steamlauncher

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.winlator.xserver.XKeycode as K
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
    // LorieView removed — causes ANR when instantiated in TerminalActivity

    private val app: SteamLauncherApp by lazy { application as SteamLauncherApp }
    private val protonManager: ProtonManager by lazy { ProtonManager(this) }
    private val handler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private val outputBuffer = StringBuilder()
    private var lineCount = 0
    private var isRunning = false
    private var currentJob: Job? = null
    private var currentProcess: Process? = null
    private var currentDir: String = ""  // initialized in onCreate from app.getFexHomeDir()
    private var frameSocketServer: FrameSocketServer? = null
    private var x11Server: X11Server? = null
    private var darksideX11: DarksideX11Server? = null
    private var xConnectorX11: XConnectorX11Server? = null
    private var xServerView: com.winlator.widget.XServerView? = null
    private var framebufferBridge: FramebufferBridge? = null
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
                // Connect FramebufferBridge to the surface for Vortek rendering
                framebufferBridge?.setOutputSurface(holder.surface)
                if (isDisplayMode) {
                    // Darkside render loop owns the surface in display mode —
                    // do NOT wire frameSocketServer here (would flicker).
                    Log.i(TAG, "Display surface created; Darkside render loop owns it")
                }
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                framebufferBridge?.setOutputSurface(holder.surface)
                if (isDisplayMode) {
                    Log.i(TAG, "Display surface changed: ${width}x${height} (Darkside owns)")
                }
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
                framebufferBridge?.setOutputSurface(null)
                frameSocketServer?.setOutputSurface(null)
                Log.i(TAG, "Vulkan display surface destroyed")
            }
        })

        // Touch routing: the listener is attached to XServerView (the
        // GLSurfaceView added on top of vulkanSurface) when it's created —
        // see wireXServerViewInput() below. vulkanSurface itself is hidden
        // once the X server starts.

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
            executeCommand("VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/fex_thunk_icd.json vulkaninfo --summary 2>&1 | head -50")
        }

        // vkcube: start X11 for xcb_connect(), switch to display mode, run with LD_PRELOAD
        // for guest-side xcb_surface injection + frame capture via TCP 19850
        findViewById<Button>(R.id.btnVkcube).setOnClickListener {
            // Start X11 if needed (vkcube calls xcb_connect())
            if (x11Server?.isRunning() != true) {
                x11Server = X11Server(this).apply {
                    onServerStarted = { handler.post { appendOutput("[X11 started for vkcube]\n") } }
                    onError = { msg -> handler.post { appendOutput("[X11 error: $msg]\n") } }
                    start()
                }
            }
            // Switch to display mode to show frames on SurfaceView
            if (!isDisplayMode) toggleDisplayMode()
            executeCommand("export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/fex_thunk_icd.json; export LD_PRELOAD=/lib/libvulkan_headless.so; vkcube 2>&1")
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

        // --- Proton-GE / Wine buttons ---

        // Setup Proton-GE: download, extract, init Wine prefix
        findViewById<Button>(R.id.btnSetupProton).setOnClickListener {
            executeCommand(protonManager.getSetupCommand())
        }

        // Wine version test: verify wine64 binary works under FEX
        findViewById<Button>(R.id.btnWineTest).setOnClickListener {
            if (!isDisplayMode) toggleDisplayMode()
            executeCommand(protonManager.getVkCmdBufTestCommand())
        }

        // Check rootfs dependencies for Wine/Proton
        findViewById<Button>(R.id.btnCheckDeps).setOnClickListener {
            executeCommand(protonManager.getDependencyCheckCommand())
        }

        // Start libXlorie X11 server (native ARM64 — NOT inside FEX)
        // Uses abstract socket @/tmp/.X11-unix/X0. Wine connects via DISPLAY=:0
        findViewById<Button>(R.id.btnStartXvfb).setOnClickListener {
            if (x11Server?.isRunning() == true) {
                appendOutput("[X11 server already running on display :0 (abstract socket)]\n")
                return@setOnClickListener
            }
            appendOutput("Starting libXlorie X11 server (native ARM64)...\n")
            x11Server = X11Server(this).apply {
                onServerStarted = {
                    handler.post {
                        appendOutput("[X11 server started on display :0 (abstract socket)]\n")
                        appendOutput("Wine commands will use DISPLAY=:0\n")
                    }
                }
                onError = { msg ->
                    handler.post {
                        appendOutput("[X11 server error: $msg]\n")
                    }
                }
                if (start()) {
                    appendOutput("[X11 server initializing...]\n")
                } else {
                    appendOutput("[ERROR: Failed to start X11 server]\n")
                }
            }
        }

        // Initialize Wine prefix (wineboot -u)
        findViewById<Button>(R.id.btnWineBoot).setOnClickListener {
            executeCommand(protonManager.getWineBootCommand())
        }

        // GameNative-style native Bionic Wine: run wine --version directly,
        // no FEX, no proot. Proves the Bionic Wine launch path works from
        // our app process before we scaffold wineserver/notepad/DXVK.
        findViewById<Button>(R.id.btnWineNativeVersion).setOnClickListener {
            appendOutput("=== Native Bionic Wine — wine --version ===\n")
            val pipeline = NativeWinePipeline(this)
            if (!pipeline.wineBinaryExists()) {
                appendOutput("[ERROR: libwine_native.so not found in nativeLibraryDir]\n")
                return@setOnClickListener
            }
            scope.launch {
                val result = pipeline.wineVersion()
                handler.post {
                    appendOutput("exit=${result.exitCode}\n")
                    if (result.stdout.isNotEmpty()) appendOutput("stdout: ${result.stdout}")
                    if (result.stderr.isNotEmpty()) appendOutput("stderr: ${result.stderr}")
                    appendOutput("===========================================\n")
                }
            }
        }

        // Native wineserver smoke test with LD_PRELOAD path-redirect shim.
        // Proves the baked /data/data/app.gamenative/... paths in Pepelespooder's
        // wineserver binary can be rewritten to our app dir without recompiling Wine.
        findViewById<Button>(R.id.btnWineServerSmoke).setOnClickListener {
            appendOutput("=== wineserver smoke (redirect shim) ===\n")
            val pipeline = NativeWinePipeline(this)
            scope.launch {
                val result = pipeline.wineServerSmoke()
                handler.post {
                    appendOutput("exit=${result.exitCode}\n")
                    if (result.stdout.isNotEmpty()) appendOutput("stdout: ${result.stdout}\n")
                    if (result.stderr.isNotEmpty()) appendOutput("stderr:\n${result.stderr}")
                    appendOutput("===========================================\n")
                }
            }
        }

        // Native wineboot: create/update the Wine prefix.
        // wineserver auto-starts as a child, both go through the redirect shim.
        findViewById<Button>(R.id.btnWineBootNative).setOnClickListener {
            appendOutput("=== wine wineboot --init (native) ===\n")
            val pipeline = NativeWinePipeline(this)
            scope.launch {
                val result = pipeline.wineBootInit()
                handler.post {
                    appendOutput("exit=${result.exitCode}\n")
                    if (result.stdout.isNotEmpty()) appendOutput("stdout:\n${result.stdout}")
                    if (result.stderr.isNotEmpty()) appendOutput("stderr:\n${result.stderr}")
                    appendOutput("===========================================\n")
                }
            }
        }

        // Quick prefix sanity: run a real Windows cmd.exe inside our native
        // wine. If the prefix is functional this prints a version banner.
        findViewById<Button>(R.id.btnWineCmdNative).setOnClickListener {
            appendOutput("=== wine cmd /c ver (native) ===\n")
            val pipeline = NativeWinePipeline(this)
            scope.launch {
                val result = pipeline.wineRun(listOf("cmd", "/c", "ver"), timeoutMs = 20000)
                handler.post {
                    appendOutput("exit=${result.exitCode}\n")
                    if (result.stdout.isNotEmpty()) appendOutput("stdout:\n${result.stdout}")
                    if (result.stderr.isNotEmpty()) appendOutput("stderr:\n${result.stderr}")
                    appendOutput("===========================================\n")
                }
            }
        }

        // Native wine + null graphics driver test. Skips X11 entirely —
        // wine's null_user_driver provides fake CreateWindow. For actual
        // rendering, DXVK → Vulkan → VK_LAYER_HEADLESS_surface → TCP →
        // FrameSocketServer → SurfaceView (same pipeline the FEX x86 side
        // uses; see display_architecture.md in MEDIATEK-DIRVERS-TEST memory).
        // First click: writes HKCU\Software\Wine\Drivers\Graphics=null,
        // then launches notepad. If notepad stays alive past timeout, the
        // null driver accepted the CreateWindow call.
        findViewById<Button>(R.id.btnWineNotepadNative).setOnClickListener {
            appendOutput("=== wine notepad.exe (native, null driver) ===\n")
            val pipeline = NativeWinePipeline(this)
            scope.launch {
                handler.post { appendOutput("[configuring HKCU\\Software\\Wine\\Drivers\\Graphics=null...]\n") }
                val reg = pipeline.wineRun(
                    args = listOf(
                        "reg", "add",
                        "HKCU\\Software\\Wine\\Drivers",
                        "/v", "Graphics", "/d", "null", "/f",
                    ),
                    timeoutMs = 15000,
                )
                handler.post { appendOutput("reg exit=${reg.exitCode} ${reg.stdout}${reg.stderr}\n") }
                val result = pipeline.wineRun(
                    args = listOf("notepad.exe"),
                    timeoutMs = 6000,
                    extraEnv = mapOf(
                        "WINEDEBUG" to "err+all,fixme-all,trace-all,+loaddll,+vulkan",
                        "VK_LOADER_DEBUG" to "all",
                    ),
                )
                handler.post {
                    appendOutput("notepad exit=${result.exitCode}\n")
                    if (result.stdout.isNotEmpty()) appendOutput("stdout:\n${result.stdout}")
                    if (result.stderr.isNotEmpty()) appendOutput("stderr:\n${result.stderr}")
                    if (result.exitCode == -99) {
                        appendOutput("[notepad process still running at 6s timeout; GUI state not verified]\n")
                    }
                    appendOutput("===========================================\n")
                }
            }
        }

        // Launch Ys IX through the NATIVE Bionic wine pipeline (not FEX).
        // Wine's Z: is mapped to "/" so the Windows path below points at the
        // game files under the app's private dir. x86-64 PE execution on
        // ARM64 wine requires libwow64fex.dll (present in Pepelespooder's
        // aarch64-windows tree). This is the first end-to-end test of the
        // native pipeline with a real game binary.
        findViewById<Button>(R.id.btnYsIXNative).setOnClickListener {
            appendOutput("=== wine ys9.exe (native Bionic) ===\n")
            // 2026-04-23: switched from DarksideX11Server to
            // XConnectorX11Server. Darkside was a pure-Java X11 server
            // without DRI3/Present extensions — wine's VK_KHR_xlib_surface
            // refused to initialize ("No DRI3 support detected") and we
            // had to route DXVK through our fake headless Vulkan layer,
            // which Mali's driver optimized to all-black frames. Winlator's
            // XConnector (imported from GameNative) has full DRI3 + Present
            // support via libxconnectorpatch.so ancillary-FD passing, so
            // DXVK can present natively through VK_KHR_xlib_surface.
            if (xConnectorX11 == null || !xConnectorX11!!.isRunning()) {
                xConnectorX11 = XConnectorX11Server(this).apply {
                    // Socket goes to imagefs_bionic/tmp/.X11-unix/X0 so our
                    // libpathredirect rule that maps com.winlator.cmod/files/imagefs/
                    // → imagefs_bionic can let wine reach it when DISPLAY=:0 triggers
                    // a path through that shim.
                    val socketRoot = "${filesDir.absolutePath}/imagefs_bionic"
                    if (start(socketRoot)) {
                        handler.post {
                            appendOutput("[XConnector X11 listening on ${socketPath()}]\n")
                        }
                        // Attach XServerView to vulkanSurface's parent FrameLayout
                        // so the X server's GL compositor renders on-screen.
                        try {
                            val container = vulkanSurface.parent as android.widget.FrameLayout
                            val xsv = com.winlator.widget.XServerView(this@TerminalActivity, xServer)
                            xServer.setRenderer(xsv.renderer)
                            runOnUiThread {
                                vulkanSurface.visibility = android.view.View.GONE
                                container.addView(xsv)
                                xServerView = xsv
                                wireXServerViewInput(xsv)
                            }
                        } catch (t: Throwable) {
                            Log.e(TAG, "XServerView attach failed", t)
                        }
                    } else {
                        handler.post { appendOutput("[XConnector X11 start failed]\n") }
                    }
                }
            }
            // Reconfigure ColdClient for Ys IX (switched back after Sekiro
            // test). steam_appid.txt + ColdClientLoader.ini are per-game;
            // ensureColdClient treats them as preserve-on-change.
            reconfigureColdClient(
                appId = "1351630",
                exePath = "steamapps\\common\\Ys IX Monstrum Nox\\ys9.exe",
                exeRunDir = "steamapps\\common\\Ys IX Monstrum Nox",
            )

            val pipeline = NativeWinePipeline(this)
            // 2026-04-22: Launch via ColdClient Steam-emulator loader
            // (matches GameNative's architecture). The loader reads
            // `C:\Program Files (x86)\Steam\ColdClientLoader.ini` for
            // the actual target exe + AppId, sets up the emulated Steam
            // client runtime, then spawns ys9.exe as a subprocess.
            // Without this, our bare stubs made Ys IX enter "DRM-free
            // mode" which still parks at BGM-loop.
            val gameWindowsPath =
                "C:\\Program Files (x86)\\Steam\\steamclient_loader_x64.exe"
            scope.launch {
                // Explorer /desktop wrapper is REQUIRED. Without it wine has
                // no graphics driver registered for window creation (directly
                // launching ys9.exe fails with `nodrv_CreateWindow "The
                // explorer process failed to start."` → `!Err! CreateSwapChain
                // Error.`). Our headless Vulkan layer doesn't replace
                // winex11.drv's CreateWindow path — only the Vulkan surface.
                val r = pipeline.wineRun(
                    args = listOf("explorer", "/desktop=shell,1920x1080", gameWindowsPath),
                    timeoutMs = 300_000,
                    extraEnv = mapOf(
                        "WINEDEBUG" to "err+all,fixme-all,+seh,+loaddll,+x11drv",
                        // Stubs from the old FEX pipeline's /opt/stubs/ dir,
                        // deployed into game-dir + prefix system32. Each "=n"
                        // picks up the stub instead of wine's builtin or the
                        // publisher's real DLL:
                        //   - Galaxy64: GOG Galaxy IPC blocks main thread before
                        //     the render loop starts (explicit comment in
                        //     ProtonManager.kt line 933). THIS was the post-
                        //     audio blocker.
                        //   - steam_api64: real DLL blocks on Steam runtime.
                        //   - GFSDK_SSAO: NVIDIA GameWorks SSAO hits paths DXVK
                        //     doesn't implement identically.
                        //
                        //   - xaudio2_7: wine-builtin (b) tested 2026-04-22 with
                        //     null ALSA in place — same BGM-loop, no crash, no
                        //     progress. XAudio2 is NOT the blocker. Reverted to
                        //     stub (n) so we stay on the known-stable path.
                        // 2026-04-22: Galaxy64= (disabled) CRASHED the game —
                        // confirmed Galaxy64.dll is a STATIC IMPORT. Game
                        // cannot start without a DLL providing its exports.
                        // So we must keep Galaxy64=n (our stub). Next test
                        // would be: modify the stub to return FAILURE from
                        // Init(), forcing the game onto its "Galaxy absent"
                        // code path while still satisfying the static import.
                        "WINEDLLOVERRIDES" to
                            "d3d11,d3d10core,d3d9,d3d8,dxgi=n;mscoree,mshtml=;" +
                            // xaudio2 rule-out RESULTS (2026-04-22 final):
                            //   =n (our stub):   game reaches BGM-loop, CSound
                            //                    threads spawn, audio flows,
                            //                    parks waiting on something.
                            //   =b (wine builtin, with PA daemon + winepulse.drv):
                            //                    game launches silently, NO
                            //                    CSound threads, no BGM, but
                            //                    MainThread still alternates
                            //                    futex/wchan=0 — parks in a
                            //                    DIFFERENT way.
                            // Conclusion: AUDIO IS NOT THE BGM-LOOP BLOCKER.
                            // Game parks either way. Real blocker is elsewhere
                            // (wine-internal IPC, X11, or a wine-patch diff
                            // between our proton-10 and GN's proton-9).
                            // Stub kept as stable baseline.
                            // 2026-04-22 night: wine-9 specifically — try =b
                            // (wine builtin xaudio2 → FAudio → winepulse). GN
                            // runtime diff shows GN has FAudio_AudioCli +
                            // audio_client_ma/ti threads, ours doesn't. wine-10
                            // FAudio previously crashed — maybe wine-9 works.
                            "xaudio2_7=b;xapofx1_5=b;" +
                            "Galaxy64=n;steam_api64=n;" +
                            "steamclient=n;steamclient64=n;" +
                            "GFSDK_SSAO_D3D11=n",
                        // DISPLAY=:0 resolves via libpathredirect to the
                        // XConnector unix socket at $tmpRoot/tmp/.X11-unix/X0.
                        "DISPLAY" to ":0",
                        // HEADLESS_DIAG_CLEAR=1 would make the layer clobber
                        // every presented image with solid green before copy
                        // — used 2026-04-21 to prove the capture+surface path
                        // is fine and the game's all-black frames are the
                        // game's actual output. Leave off by default.
                    ),
                    // 2026-04-22 late: tested proton-9 + ColdClient. Same
                    // parked state as proton-10. Reverted to p10 because
                    // our services.exe/explorer.exe DebugInfo NOP patches
                    // + winevulkan assert patch are wine-10 specific. p9
                    // run left those overwritten by wine-9's wineboot.
                    useProton9 = true,
                )
                try {
                    java.io.File(filesDir, "ys9_stdout.log").writeText(r.stdout)
                    java.io.File(filesDir, "ys9_stderr.log").writeText(r.stderr)
                } catch (_: Throwable) {}
                handler.post {
                    appendOutput("ys9 exit=${r.exitCode}\n")
                    appendOutput("stdout bytes=${r.stdout.length}, stderr bytes=${r.stderr.length}\n")
                    appendOutput("full logs: files/ys9_stdout.log, files/ys9_stderr.log\n")
                    if (r.exitCode == -99) {
                        appendOutput("[ys9 process still running at timeout]\n")
                    }
                    appendOutput("===========================================\n")
                }
            }
        }

        // Launch Sekiro through the native Bionic chimera pipeline — parallel
        // to btnYsIXNative, different exe + AppId. First attempt at a
        // second game on this pipeline. DRM side: ColdClient Steam emulator
        // bypasses Steam entirely, which means EAC never initializes either
        // (EAC hooks through Steam's runtime; no Steam = no EAC).
        findViewById<Button>(R.id.btnLaunchSekiroN).setOnClickListener {
            appendOutput("=== wine sekiro.exe (native Bionic) ===\n")
            if (xConnectorX11 == null || !xConnectorX11!!.isRunning()) {
                xConnectorX11 = XConnectorX11Server(this).apply {
                    val socketRoot = "${filesDir.absolutePath}/imagefs_bionic"
                    if (start(socketRoot)) {
                        handler.post { appendOutput("[XConnector X11 listening on ${socketPath()}]\n") }
                        try {
                            val container = vulkanSurface.parent as android.widget.FrameLayout
                            val xsv = com.winlator.widget.XServerView(this@TerminalActivity, xServer)
                            xServer.setRenderer(xsv.renderer)
                            runOnUiThread {
                                vulkanSurface.visibility = android.view.View.GONE
                                container.addView(xsv)
                                xServerView = xsv
                                wireXServerViewInput(xsv)
                            }
                        } catch (t: Throwable) {
                            Log.e(TAG, "XServerView attach failed", t)
                        }
                    } else {
                        handler.post { appendOutput("[XConnector X11 start failed]\n") }
                    }
                }
            }
            reconfigureColdClient(
                appId = "814380",
                exePath = "steamapps\\common\\Sekiro\\sekiro.exe",
                exeRunDir = "steamapps\\common\\Sekiro",
            )

            val pipeline = NativeWinePipeline(this)
            val gameWindowsPath =
                "C:\\Program Files (x86)\\Steam\\steamclient_loader_x64.exe"
            scope.launch {
                // Strip SteamStub DRM from sekiro.exe before launch. Matches
                // what GameNative does on game import. Without this,
                // ColdClient + steam_api64 stubs don't satisfy the SteamStub
                // wrapper's own auth check (seen: "Application load error
                // 3:0000065432" dialog). Steamless produces
                // `sekiro.exe.unpacked.exe` which is DRM-free.
                val sekiroHostExe = java.io.File(
                    filesDir,
                    "proton10/prefix/.wine/drive_c/Program Files (x86)/Steam/steamapps/common/Sekiro/sekiro.exe",
                ).absolutePath
                val sekiroGuestExe =
                    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sekiro\\sekiro.exe"
                val unpackedGuestExe = pipeline.unpackWithSteamless(
                    guestExePath = sekiroGuestExe,
                    hostExeAbs = sekiroHostExe,
                    useProton9 = true,
                )
                if (unpackedGuestExe != sekiroGuestExe) {
                    // Replace sekiro.exe with the unpacked version so the
                    // ColdClient loader (which is configured to launch
                    // sekiro.exe) runs the DRM-free build. Back up original.
                    val hostUnpacked = java.io.File("$sekiroHostExe.unpacked.exe")
                    val hostBackup = java.io.File("$sekiroHostExe.steamstub")
                    if (hostUnpacked.exists()) {
                        if (!hostBackup.exists()) {
                            java.io.File(sekiroHostExe).copyTo(hostBackup, overwrite = false)
                        }
                        hostUnpacked.copyTo(java.io.File(sekiroHostExe), overwrite = true)
                        handler.post { appendOutput("[Steamless: swapped sekiro.exe with unpacked]\n") }
                    }
                } else {
                    handler.post { appendOutput("[Steamless: no unpacking happened, using original sekiro.exe]\n") }
                }

                val r = pipeline.wineRun(
                    args = listOf("explorer", "/desktop=shell,1920x1080", gameWindowsPath),
                    timeoutMs = 300_000,
                    extraEnv = mapOf(
                        "WINEDEBUG" to "err+all,fixme-all,+seh,+loaddll,+x11drv",
                        // Sekiro-specific overrides: drop Galaxy64 (Sekiro is
                        // Steam-only, no GOG). Keep DXVK on n, steam_api64 stub.
                        // GFSDK_SSAO isn't an NVIDIA-only SSAO in Sekiro's case;
                        // start conservative and iterate if it fails.
                        "WINEDLLOVERRIDES" to
                            "d3d11,d3d10core,d3d9,d3d8,dxgi=n;mscoree,mshtml=;" +
                            "xaudio2_7=b;xapofx1_5=b;" +
                            "steam_api64=n;" +
                            "steamclient=n;steamclient64=n",
                        "DISPLAY" to ":0",
                    ),
                    useProton9 = true,
                )
                try {
                    java.io.File(filesDir, "sekiro_stdout.log").writeText(r.stdout)
                    java.io.File(filesDir, "sekiro_stderr.log").writeText(r.stderr)
                } catch (_: Throwable) {}
                handler.post {
                    appendOutput("sekiro exit=${r.exitCode}\n")
                    appendOutput("stdout bytes=${r.stdout.length}, stderr bytes=${r.stderr.length}\n")
                    appendOutput("full logs: files/sekiro_stdout.log, files/sekiro_stderr.log\n")
                    if (r.exitCode == -99) appendOutput("[sekiro still running at timeout]\n")
                    appendOutput("===========================================\n")
                }
            }
        }

        // Launch DS3 through the same native-Bionic ColdClient pipeline
        // as Sekiro N. Restored after a tree-restoration during RE4 work
        // dropped the wiring; game files + Steamless products survived.
        // Per state_ds3.md: AppId 374320, USE_CPU_BCN=all required (else
        // white-screen on Mali), FORCE_OPTIMIZATION_BARRIERS + COMPOSITE.
        findViewById<Button>(R.id.btnLaunchDS3).setOnClickListener {
            appendOutput("=== wine DarkSoulsIII.exe (native Bionic) ===\n")
            if (xConnectorX11 == null || !xConnectorX11!!.isRunning()) {
                xConnectorX11 = XConnectorX11Server(this).apply {
                    val socketRoot = "${filesDir.absolutePath}/imagefs_bionic"
                    if (start(socketRoot)) {
                        handler.post { appendOutput("[XConnector X11 listening on ${socketPath()}]\n") }
                        try {
                            val container = vulkanSurface.parent as android.widget.FrameLayout
                            val xsv = com.winlator.widget.XServerView(this@TerminalActivity, xServer)
                            xServer.setRenderer(xsv.renderer)
                            runOnUiThread {
                                vulkanSurface.visibility = android.view.View.GONE
                                container.addView(xsv)
                                xServerView = xsv
                                wireXServerViewInput(xsv)
                            }
                        } catch (t: Throwable) {
                            Log.e(TAG, "XServerView attach failed", t)
                        }
                    } else {
                        handler.post { appendOutput("[XConnector X11 start failed]\n") }
                    }
                }
            }
            reconfigureColdClient(
                appId = "374320",
                exePath = "steamapps\\common\\DARK SOULS III\\Game\\DarkSoulsIII.exe",
                exeRunDir = "steamapps\\common\\DARK SOULS III\\Game",
            )

            val pipeline = NativeWinePipeline(this)
            val gameWindowsPath =
                "C:\\Program Files (x86)\\Steam\\steamclient_loader_x64.exe"
            scope.launch {
                val ds3HostExe = java.io.File(
                    filesDir,
                    "proton10/prefix/.wine/drive_c/Program Files (x86)/Steam/steamapps/common/DARK SOULS III/Game/DarkSoulsIII.exe",
                ).absolutePath
                val ds3GuestExe =
                    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DARK SOULS III\\Game\\DarkSoulsIII.exe"
                val unpackedGuestExe = pipeline.unpackWithSteamless(
                    guestExePath = ds3GuestExe,
                    hostExeAbs = ds3HostExe,
                    useProton9 = true,
                )
                if (unpackedGuestExe != ds3GuestExe) {
                    val hostUnpacked = java.io.File("$ds3HostExe.unpacked.exe")
                    val hostBackup = java.io.File("$ds3HostExe.steamstub")
                    if (hostUnpacked.exists()) {
                        if (!hostBackup.exists()) {
                            java.io.File(ds3HostExe).copyTo(hostBackup, overwrite = false)
                        }
                        hostUnpacked.copyTo(java.io.File(ds3HostExe), overwrite = true)
                        handler.post { appendOutput("[Steamless: swapped DarkSoulsIII.exe with unpacked]\n") }
                    }
                } else {
                    handler.post { appendOutput("[Steamless: no unpacking happened, using original DarkSoulsIII.exe]\n") }
                }

                val r = pipeline.wineRun(
                    args = listOf("explorer", "/desktop=shell,1920x1080", gameWindowsPath),
                    timeoutMs = 600_000,
                    extraEnv = mapOf(
                        "WINEDEBUG" to "err+all,fixme-all,+seh,+loaddll,+x11drv",
                        "WINEDLLOVERRIDES" to
                            "d3d11,d3d10core,d3d9,d3d8,dxgi=n;mscoree,mshtml=;" +
                            "xaudio2_7=b;xapofx1_5=b;" +
                            "steam_api64=n;" +
                            "steamclient=n;steamclient64=n",
                        "DISPLAY" to ":0",
                        "USE_CPU_BCN" to "all",
                        "FORCE_OPTIMIZATION_BARRIERS" to "1",
                        "FORCE_SPEC_COMPOSITE_CONSTANTS" to "1",
                    ),
                    useProton9 = true,
                )
                try {
                    java.io.File(filesDir, "ds3_stdout.log").writeText(r.stdout)
                    java.io.File(filesDir, "ds3_stderr.log").writeText(r.stderr)
                } catch (_: Throwable) {}
                handler.post {
                    appendOutput("ds3 exit=${r.exitCode}\n")
                    appendOutput("stdout bytes=${r.stdout.length}, stderr bytes=${r.stderr.length}\n")
                    appendOutput("full logs: files/ds3_stdout.log, files/ds3_stderr.log\n")
                    if (r.exitCode == -99) appendOutput("[ds3 still running at timeout]\n")
                    appendOutput("===========================================\n")
                }
            }
        }

        // Quick test: run Wine notepad (needs X11 for windowing)
        findViewById<Button>(R.id.btnNotepad).setOnClickListener {
            // Start X11 if needed (notepad needs X11 for its window)
            if (x11Server?.isRunning() != true) {
                x11Server = X11Server(this).apply {
                    onServerStarted = { handler.post { appendOutput("[X11 started for notepad]\n") } }
                    onError = { msg -> handler.post { appendOutput("[X11 error: $msg]\n") } }
                    start()
                }
            }
            executeCommand(protonManager.getNotepadTestCommand())
        }

        // Launch Ys IX game — OLD FEX x86-64 pipeline, but routed through
        // the NEW XConnector X server (same server used by YsIX N). X11
        // plumbing is architecture-agnostic, so wine-under-FEX can connect
        // to XConnector over a unix socket just like native ARM64 wine.
        // Expected: game launches, X11 handshake works, rendering shows
        // the FEX thunk's VB[0] vertex corruption (exploded menu) — the
        // point of this experiment is to validate X11 integration, not
        // fix the corruption.
        findViewById<Button>(R.id.btnLaunchGame).setOnClickListener {
            if (xConnectorX11 == null || !xConnectorX11!!.isRunning()) {
                xConnectorX11 = XConnectorX11Server(this).apply {
                    val socketRoot = "${filesDir.absolutePath}/imagefs_bionic"
                    if (start(socketRoot)) {
                        handler.post {
                            appendOutput("[XConnector X11 listening on ${socketPath()}]\n")
                        }
                        try {
                            val container = vulkanSurface.parent as android.widget.FrameLayout
                            val xsv = com.winlator.widget.XServerView(this@TerminalActivity, xServer)
                            xServer.setRenderer(xsv.renderer)
                            runOnUiThread {
                                vulkanSurface.visibility = android.view.View.GONE
                                container.addView(xsv)
                                xServerView = xsv
                                wireXServerViewInput(xsv)
                            }
                        } catch (t: Throwable) {
                            Log.e(TAG, "XServerView attach failed", t)
                        }
                    } else {
                        handler.post { appendOutput("[XConnector X11 start failed]\n") }
                    }
                }
            }
            // Tell FexExecutor to symlink /tmp/.X11-unix/X0 inside the FEX
            // rootfs at XConnector's socket. Runs inside ensureSocketSymlinks
            // on the very next executeCommand call.
            val xconnSocket = "${filesDir.absolutePath}/imagefs_bionic/tmp/.X11-unix/X0"
            app.fexExecutor.x11SocketTargetOverride = xconnSocket
            appendOutput("[FEX X11 socket target: $xconnSocket]\n")
            executeCommand(protonManager.getLaunchCommand(
                "/home/user/Steam/steamapps/common/Ys IX Monstrum Nox/ys9.exe",
                useHeadlessLayer = false,
            ))
        }

        // Ys IX Dump Mode: capture frames as PPM files (no TCP/FrameSocketServer)
        findViewById<Button>(R.id.btnDumpGame).setOnClickListener {
            // Start X11 if needed (Wine still needs it for CreateWindowExA)
            if (x11Server?.isRunning() != true) {
                x11Server = X11Server(this).apply {
                    onServerStarted = { handler.post { appendOutput("[X11 started for dump mode]\n") } }
                    onError = { msg -> handler.post { appendOutput("[X11 error: $msg]\n") } }
                    start()
                }
            }
            // Do NOT start FrameSocketServer or toggle display mode
            executeCommand(protonManager.getDumpModeLaunchCommand(
                "/home/user/Steam/steamapps/common/Ys IX Monstrum Nox/ys9.exe"
            ))
        }

        // Show Steam login dialog, then run a command
        fun showLoginDialog(title: String, onLogin: (loginArgs: String) -> Unit) {
            val layout = android.widget.LinearLayout(this).apply {
                orientation = android.widget.LinearLayout.VERTICAL
                setPadding(50, 30, 50, 10)
            }
            val userInput = EditText(this).apply {
                hint = "Username"
                setText("sakatepongolas")
            }
            val passInput = EditText(this).apply {
                hint = "Password"
                inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
            }
            layout.addView(userInput)
            layout.addView(passInput)
            android.app.AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage("Enter Steam credentials")
                .setView(layout)
                .setPositiveButton("Login") { _, _ ->
                    val user = userInput.text.toString().trim()
                    val pass = passInput.text.toString().trim()
                    val loginArgs = if (user.isNotEmpty() && pass.isNotEmpty()) "$user $pass" else ""
                    onLogin(loginArgs)
                }
                .setNegativeButton("Cancel", null)
                .show()
        }

        fun ensureX11() {
            if (x11Server?.isRunning() != true) {
                x11Server = X11Server(this).apply {
                    onServerStarted = { handler.post { appendOutput("[X11 started]\n") } }
                    onError = { msg -> handler.post { appendOutput("[X11 error: $msg]\n") } }
                    start()
                }
            }
        }

        // Launch Steam client (login only)
        findViewById<Button>(R.id.btnSteam).setOnClickListener {
            ensureX11()
            showLoginDialog("Steam Login") { loginArgs ->
                executeCommand(protonManager.getSteamCommand(loginArgs))
            }
        }

        // Launch RE4 via steam://rungameid/ — Steam handles DRM + Proton
        // Pre-downloads AppID 228980 (Steamworks Common Redistributables) natively
        // to avoid FEX filesystem overlay issues with Steam's download pipeline
        findViewById<Button>(R.id.btnLaunchRE4).setOnClickListener {
            ensureX11()
            showLoginDialog("RE4 — Steam Login") { loginArgs ->
                if (!isDisplayMode) toggleDisplayMode()

                val parts = loginArgs.trim().split("\\s+".toRegex(), limit = 2)
                val user = parts.getOrElse(0) { "" }
                val pass = parts.getOrElse(1) { "" }

                val downloader = app.contentDownloader

                // Check if 228980 already downloaded
                if (downloader.isAppInstalled(228980)) {
                    appendOutput("[228980 already installed, launching RE4]\n")
                    executeCommand(protonManager.getSteamRunGameCommand(
                        loginArgs = loginArgs,
                        appId = "2050650"
                    ))
                    return@showLoginDialog
                }

                appendOutput("[Pre-downloading AppID 228980 via JavaSteam...]\n")
                val authenticator = DialogAuthenticator(this@TerminalActivity, handler)
                downloader.onProgress = { msg -> handler.post { appendOutput("[DL] $msg\n") } }
                downloader.onError = { msg -> handler.post { appendOutput("[DL ERROR] $msg\n") } }
                downloader.onComplete = {
                    handler.post {
                        appendOutput("[228980 download complete, launching RE4]\n")
                        executeCommand(protonManager.getSteamRunGameCommand(
                            loginArgs = loginArgs,
                            appId = "2050650"
                        ))
                    }
                }

                scope.launch {
                    downloader.downloadApp(
                        appId = 228980,
                        installDir = "Steamworks Shared",
                        username = user,
                        password = pass,
                        authenticator = authenticator
                    )
                }
            }
        }

        // Launch Sekiro via steam://rungameid/ — Steam handles DRM + Proton
        // Pre-downloads AppID 228980 (Steamworks Common Redistributables) natively
        // to avoid FEX filesystem overlay issues with Steam's download pipeline
        findViewById<Button>(R.id.btnLaunchSekiro).setOnClickListener {
            ensureX11()
            showLoginDialog("Sekiro — Steam Login") { loginArgs ->
                if (!isDisplayMode) toggleDisplayMode()

                val parts = loginArgs.trim().split("\\s+".toRegex(), limit = 2)
                val user = parts.getOrElse(0) { "" }
                val pass = parts.getOrElse(1) { "" }

                val downloader = app.contentDownloader

                // Check if 228980 already downloaded
                if (downloader.isAppInstalled(228980)) {
                    appendOutput("[228980 already installed, launching Sekiro]\n")
                    executeCommand(protonManager.getSteamRunGameCommand(
                        loginArgs = loginArgs,
                        appId = "814380"
                    ))
                    return@showLoginDialog
                }

                appendOutput("[Pre-downloading AppID 228980 via JavaSteam...]\n")
                val authenticator = DialogAuthenticator(this@TerminalActivity, handler)
                downloader.onProgress = { msg -> handler.post { appendOutput("[DL] $msg\n") } }
                downloader.onError = { msg -> handler.post { appendOutput("[DL ERROR] $msg\n") } }
                downloader.onComplete = {
                    handler.post {
                        appendOutput("[228980 download complete, launching Sekiro]\n")
                        executeCommand(protonManager.getSteamRunGameCommand(
                            loginArgs = loginArgs,
                            appId = "814380"
                        ))
                    }
                }

                scope.launch {
                    downloader.downloadApp(
                        appId = 228980,
                        installDir = "Steamworks Shared",
                        username = user,
                        password = pass,
                        authenticator = authenticator
                    )
                }
            }
        }
    }

    private fun showX11Display() {
        // LorieView causes ANR — just show terminal output for now
        // X11 runs headless, auto-clicker handles launcher interaction
        if (!isDisplayMode) toggleDisplayMode()
    }

    private fun toggleDisplayMode() {
        isDisplayMode = !isDisplayMode

        if (isDisplayMode) {
            scrollView.visibility = View.GONE
            vulkanSurface.visibility = View.VISIBLE
            btnDisplay.text = "Terminal"

            // Display path: render Darkside's X11 pixmap to the surface.
            // wine's winex11.drv paints GDI content (title bars, taskbar,
            // dialogs, wine-drawn window decorations) into Darkside's
            // internal Bitmap. The DXVK Vulkan-capture layer ONLY sees the
            // game's 3D swapchain — all non-DXVK UI is invisible via
            // that path. So display the X11 root composite directly.
            //
            // FrameSocketServer is NOT bound to the surface here to avoid
            // both renderers racing for the canvas (which flickers between
            // DXVK's black frames and the Darkside desktop). If we ever
            // need the DXVK content composited on top, we'd need a real
            // compositor drawing both onto the same canvas per frame.
            // (2026-04-22 — pivot after BGM-loop diagnostics showed
            // Darkside has real content the user never sees.)
            frameSocketServer?.setOutputSurface(null)
            startDarksideRenderLoop()
            Log.i(TAG, "Switched to X11 display mode")
        } else {
            vulkanSurface.visibility = View.GONE
            scrollView.visibility = View.VISIBLE
            btnDisplay.text = "Display"

            frameSocketServer?.setOutputSurface(null)
            stopDarksideRenderLoop()
            Log.i(TAG, "Switched to terminal mode")
        }
    }

    private var darksideRenderThread: Thread? = null
    @Volatile private var darksideRenderStop = false

    private fun startDarksideRenderLoop() {
        if (darksideRenderThread?.isAlive == true) return
        darksideRenderStop = false
        val t = Thread({
            val srcRect = android.graphics.Rect()
            val dstRect = android.graphics.Rect()
            while (!darksideRenderStop) {
                val bmp = try { darksideX11?.renderRoot() } catch (_: Throwable) { null }
                val sv = vulkanSurface
                val holder = sv.holder
                val surf = holder.surface
                if (bmp != null && surf != null && surf.isValid) {
                    try {
                        val canvas = holder.lockHardwareCanvas() ?: continue
                        try {
                            srcRect.set(0, 0, bmp.width, bmp.height)
                            dstRect.set(0, 0, canvas.width, canvas.height)
                            canvas.drawBitmap(bmp, srcRect, dstRect, null)
                        } finally {
                            holder.unlockCanvasAndPost(canvas)
                        }
                    } catch (_: Throwable) {
                        // surface gone / locked by DXVK frame socket; skip frame
                    }
                }
                try { Thread.sleep(100) } catch (_: InterruptedException) { break }
            }
        }, "DarksideRender")
        t.isDaemon = true
        t.start()
        darksideRenderThread = t
    }

    private fun stopDarksideRenderLoop() {
        darksideRenderStop = true
        darksideRenderThread?.interrupt()
        darksideRenderThread = null
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

    /**
     * Rewrite ColdClient's on-device per-game settings so steamclient_loader_x64.exe
     * launches the requested game. Called from every game-launch handler so
     * switching between games doesn't leave stale config.
     *
     * ColdClient reads:
     *   - `steam_settings/steam_appid.txt`: one line, the AppID (e.g. "814380")
     *   - `ColdClientLoader.ini`: [SteamClient] section with AppId= + Exe= + ExeRunDir=
     *
     * Both `ensureColdClient` treats as preserve-on-change, so our writes stick.
     */
    private fun reconfigureColdClient(appId: String, exePath: String, exeRunDir: String) {
        try {
            val steamDir = java.io.File(
                filesDir, "proton10/prefix/.wine/drive_c/Program Files (x86)/Steam"
            )
            java.io.File(steamDir, "steam_settings").mkdirs()
            java.io.File(steamDir, "steam_settings/steam_appid.txt").writeText("$appId\n")

            val iniContent = """
                [SteamClient]

                Exe=$exePath
                ExeRunDir=$exeRunDir
                ExeCommandLine=
                AppId=$appId

                SteamClientDll=steamclient.dll
                SteamClient64Dll=steamclient64.dll


                [Injection]
                IgnoreLoaderArchDifference=1
            """.trimIndent()
            java.io.File(steamDir, "ColdClientLoader.ini").writeText(iniContent)
            appendOutput("[ColdClient reconfigured: AppId=$appId target=$exePath]\n")
        } catch (t: Throwable) {
            appendOutput("[ColdClient reconfigure failed: ${t.message}]\n")
        }
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
                        // Log full output to logcat for diagnostic capture
                        // Use: adb logcat -s FexOutput:I
                        logFullOutput(text)

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

    /**
     * Log full process output to logcat for diagnostic capture.
     * Capture with: adb logcat -s FexOutput:I
     */
    private fun logFullOutput(text: String) {
        var remaining = text
        while (remaining.isNotEmpty()) {
            val chunk = remaining.take(3900)
            Log.i("FexOutput", chunk)
            remaining = remaining.drop(3900)
        }
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
            "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/fex_thunk_icd.json"
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

            // Create FramebufferBridge with real HardwareBuffers for Vortek rendering.
            // Vortek REQUIRES a valid AHardwareBuffer — returning null crashes
            // AsyncPipelineCreator in Mali driver (null VkDevice at offset 0xb8).
            val bridge = FramebufferBridge(
                vulkanSurface.holder.surface,  // may be null if surface not ready yet
                1280,  // default width (matches Wine virtual desktop)
                720    // default height
            )
            framebufferBridge = bridge
            VortekRenderer.setWindowInfoProvider(bridge)
            Log.i(TAG, "FramebufferBridge connected to VortekRenderer")

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

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Forward mapped keys to the X11 server while a game is running.
        // Skip when the command EditText has focus (so the diagnostic
        // prompt still works). Back/volume/menu keep their Android meaning.
        val srv = xConnectorX11
        if (srv != null && srv.isRunning() && !etCommand.isFocused) {
            val xkc = androidKeyToXKeycode(event.keyCode) ?: return super.dispatchKeyEvent(event)
            when (event.action) {
                KeyEvent.ACTION_DOWN -> {
                    Log.i(TAG, "key DOWN kc=${event.keyCode} -> X11 ${xkc.name}")
                    srv.xServer.injectKeyPress(xkc); return true
                }
                KeyEvent.ACTION_UP -> {
                    srv.xServer.injectKeyRelease(xkc); return true
                }
            }
        }
        return super.dispatchKeyEvent(event)
    }

    /** Wire touchscreen-as-mouse input on the XServerView (the GL surface
     *  that overlays vulkanSurface). Touches are translated to X11 pointer
     *  events on the wrapped XServer. */
    private fun wireXServerViewInput(xsv: com.winlator.widget.XServerView) {
        xsv.isFocusable = true
        xsv.isFocusableInTouchMode = true
        xsv.setOnTouchListener { v, event ->
            val srv = xConnectorX11 ?: return@setOnTouchListener false
            val w = v.width
            val h = v.height
            if (w <= 0 || h <= 0) return@setOnTouchListener false
            val sw = srv.xServer.screenInfo.width
            val sh = srv.xServer.screenInfo.height
            val x = (event.x * sw / w).toInt().coerceIn(0, sw - 1)
            val y = (event.y * sh / h).toInt().coerceIn(0, sh - 1)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    Log.i(TAG, "touch DOWN -> X11 ($x,$y)")
                    srv.xServer.injectPointerMove(x, y)
                    srv.xServer.injectPointerButtonPress(com.winlator.xserver.Pointer.Button.BUTTON_LEFT)
                }
                MotionEvent.ACTION_MOVE -> srv.xServer.injectPointerMove(x, y)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    Log.i(TAG, "touch UP   -> X11 ($x,$y)")
                    srv.xServer.injectPointerMove(x, y)
                    srv.xServer.injectPointerButtonRelease(com.winlator.xserver.Pointer.Button.BUTTON_LEFT)
                }
            }
            true
        }
    }

    /** Map common Android KeyEvent codes to X11 keycodes (XKeycode enum).
     *  Returns null for keys we deliberately don't forward (Back, Volume,
     *  Menu, Search, etc). Covers letters, digits, arrows, function keys,
     *  modifiers, and common nav so menu navigation in games works
     *  with USB/BT keyboards or remapped device keys. */
    private fun androidKeyToXKeycode(kc: Int): com.winlator.xserver.XKeycode? {
        return when (kc) {
            KeyEvent.KEYCODE_A -> K.KEY_A; KeyEvent.KEYCODE_B -> K.KEY_B
            KeyEvent.KEYCODE_C -> K.KEY_C; KeyEvent.KEYCODE_D -> K.KEY_D
            KeyEvent.KEYCODE_E -> K.KEY_E; KeyEvent.KEYCODE_F -> K.KEY_F
            KeyEvent.KEYCODE_G -> K.KEY_G; KeyEvent.KEYCODE_H -> K.KEY_H
            KeyEvent.KEYCODE_I -> K.KEY_I; KeyEvent.KEYCODE_J -> K.KEY_J
            KeyEvent.KEYCODE_K -> K.KEY_K; KeyEvent.KEYCODE_L -> K.KEY_L
            KeyEvent.KEYCODE_M -> K.KEY_M; KeyEvent.KEYCODE_N -> K.KEY_N
            KeyEvent.KEYCODE_O -> K.KEY_O; KeyEvent.KEYCODE_P -> K.KEY_P
            KeyEvent.KEYCODE_Q -> K.KEY_Q; KeyEvent.KEYCODE_R -> K.KEY_R
            KeyEvent.KEYCODE_S -> K.KEY_S; KeyEvent.KEYCODE_T -> K.KEY_T
            KeyEvent.KEYCODE_U -> K.KEY_U; KeyEvent.KEYCODE_V -> K.KEY_V
            KeyEvent.KEYCODE_W -> K.KEY_W; KeyEvent.KEYCODE_X -> K.KEY_X
            KeyEvent.KEYCODE_Y -> K.KEY_Y; KeyEvent.KEYCODE_Z -> K.KEY_Z
            KeyEvent.KEYCODE_0 -> K.KEY_0; KeyEvent.KEYCODE_1 -> K.KEY_1
            KeyEvent.KEYCODE_2 -> K.KEY_2; KeyEvent.KEYCODE_3 -> K.KEY_3
            KeyEvent.KEYCODE_4 -> K.KEY_4; KeyEvent.KEYCODE_5 -> K.KEY_5
            KeyEvent.KEYCODE_6 -> K.KEY_6; KeyEvent.KEYCODE_7 -> K.KEY_7
            KeyEvent.KEYCODE_8 -> K.KEY_8; KeyEvent.KEYCODE_9 -> K.KEY_9
            KeyEvent.KEYCODE_DPAD_UP -> K.KEY_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> K.KEY_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> K.KEY_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> K.KEY_RIGHT
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_NUMPAD_ENTER -> K.KEY_ENTER
            KeyEvent.KEYCODE_ESCAPE -> K.KEY_ESC
            KeyEvent.KEYCODE_TAB -> K.KEY_TAB
            KeyEvent.KEYCODE_SPACE -> K.KEY_SPACE
            KeyEvent.KEYCODE_DEL -> K.KEY_BKSP
            KeyEvent.KEYCODE_FORWARD_DEL -> K.KEY_DEL
            KeyEvent.KEYCODE_INSERT -> K.KEY_INSERT
            KeyEvent.KEYCODE_MOVE_HOME -> K.KEY_HOME
            KeyEvent.KEYCODE_MOVE_END -> K.KEY_END
            KeyEvent.KEYCODE_PAGE_UP -> K.KEY_PRIOR
            KeyEvent.KEYCODE_PAGE_DOWN -> K.KEY_NEXT
            KeyEvent.KEYCODE_SHIFT_LEFT -> K.KEY_SHIFT_L
            KeyEvent.KEYCODE_SHIFT_RIGHT -> K.KEY_SHIFT_R
            KeyEvent.KEYCODE_CTRL_LEFT -> K.KEY_CTRL_L
            KeyEvent.KEYCODE_CTRL_RIGHT -> K.KEY_CTRL_R
            KeyEvent.KEYCODE_ALT_LEFT -> K.KEY_ALT_L
            KeyEvent.KEYCODE_ALT_RIGHT -> K.KEY_ALT_R
            KeyEvent.KEYCODE_F1 -> K.KEY_F1; KeyEvent.KEYCODE_F2 -> K.KEY_F2
            KeyEvent.KEYCODE_F3 -> K.KEY_F3; KeyEvent.KEYCODE_F4 -> K.KEY_F4
            KeyEvent.KEYCODE_F5 -> K.KEY_F5; KeyEvent.KEYCODE_F6 -> K.KEY_F6
            KeyEvent.KEYCODE_F7 -> K.KEY_F7; KeyEvent.KEYCODE_F8 -> K.KEY_F8
            KeyEvent.KEYCODE_F9 -> K.KEY_F9; KeyEvent.KEYCODE_F10 -> K.KEY_F10
            KeyEvent.KEYCODE_F11 -> K.KEY_F11; KeyEvent.KEYCODE_F12 -> K.KEY_F12
            KeyEvent.KEYCODE_COMMA -> K.KEY_COMMA
            KeyEvent.KEYCODE_PERIOD -> K.KEY_PERIOD
            KeyEvent.KEYCODE_SLASH -> K.KEY_SLASH
            KeyEvent.KEYCODE_SEMICOLON -> K.KEY_SEMICOLON
            KeyEvent.KEYCODE_APOSTROPHE -> K.KEY_APOSTROPHE
            KeyEvent.KEYCODE_GRAVE -> K.KEY_GRAVE
            KeyEvent.KEYCODE_MINUS -> K.KEY_MINUS
            KeyEvent.KEYCODE_EQUALS -> K.KEY_EQUAL
            KeyEvent.KEYCODE_LEFT_BRACKET -> K.KEY_BRACKET_LEFT
            KeyEvent.KEYCODE_RIGHT_BRACKET -> K.KEY_BRACKET_RIGHT
            KeyEvent.KEYCODE_BACKSLASH -> K.KEY_BACKSLASH
            else -> null
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
        x11Server?.stop()
        x11Server = null
        VortekRenderer.stop()
        framebufferBridge?.release()
        framebufferBridge = null
        super.onDestroy()
    }
}
