package com.mediatek.steamlauncher

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Manages Proton-GE (Wine + DXVK) for running Windows games via FEX-Emu.
 *
 * Rendering chain:
 *   Game (Win x86-64) -> Wine/DXVK (DX11->Vulkan) -> FEX thunks -> Vortek -> Mali GPU
 *   -> FrameSocketServer -> SurfaceView
 *
 * Proton-GE bundles Wine, DXVK, VKD3D-Proton, and various game fixes.
 * GE-Proton10-30 uses files/ subdirectory for Wine binaries and libraries.
 */
class ProtonManager(private val context: Context) {

    companion object {
        private const val TAG = "ProtonManager"

        const val PROTON_VERSION = "GE-Proton10-30"
        private const val PROTON_TARBALL = "$PROTON_VERSION.tar.gz"
        private const val PROTON_URL =
            "https://github.com/GloriousEggroll/proton-ge-custom/releases/download/$PROTON_VERSION/$PROTON_TARBALL"

        /** Where Proton-GE gets installed inside the rootfs */
        const val PROTON_INSTALL_DIR = "/opt/proton-ge"

        /** Default game directory inside the rootfs */
        const val GAMES_DIR = "/home/user/games"

        private const val MARKER_PROTON = ".proton_installed"
    }

    private val app: SteamLauncherApp
        get() = context.applicationContext as SteamLauncherApp

    private val fexRootfsDir: String
        get() = app.getFexRootfsDir()

    private val fexHomeDir: String
        get() = app.getFexHomeDir()

    /** Host-side path to Proton inside the rootfs */
    private val protonHostPath: File
        get() = File(fexRootfsDir, "opt/proton-ge")

    /** Check if Proton-GE is installed */
    fun isProtonInstalled(): Boolean {
        return File(protonHostPath, "files/bin/wine64").exists()
    }

    /**
     * Get the shell script that downloads and extracts Proton-GE inside the FEX guest.
     * This runs as a FEX command via the terminal.
     */
    fun getSetupCommand(): String {
        return """
            set -e
            PROTON_DIR="$PROTON_INSTALL_DIR"

            # Check if already installed
            if [ -x "${'$'}PROTON_DIR/files/bin/wine64" ]; then
                echo "Proton-GE already installed at ${'$'}PROTON_DIR"
                "${'$'}PROTON_DIR/files/bin/wine64" --version
                exit 0
            fi

            echo "=== Installing $PROTON_VERSION ==="

            # Check for pre-downloaded tarball (pushed via adb)
            TARBALL="/tmp/$PROTON_TARBALL"
            if [ ! -f "${'$'}TARBALL" ]; then
                echo "Downloading $PROTON_VERSION (~486MB)..."
                echo "URL: $PROTON_URL"

                # Try wget first, then curl
                if command -v wget >/dev/null 2>&1; then
                    wget -q --show-progress -O "${'$'}TARBALL" "$PROTON_URL" || {
                        echo "wget failed, trying curl..."
                        curl -L -o "${'$'}TARBALL" "$PROTON_URL"
                    }
                elif command -v curl >/dev/null 2>&1; then
                    curl -L -o "${'$'}TARBALL" "$PROTON_URL"
                else
                    echo "ERROR: Neither wget nor curl found!"
                    echo "Alternative: download on your PC and push via adb:"
                    echo "  adb push $PROTON_TARBALL /data/.../fex-rootfs/Ubuntu_22_04/tmp/"
                    exit 1
                fi
            else
                echo "Using pre-downloaded tarball: ${'$'}TARBALL"
            fi

            # Verify download
            SIZE=${'$'}(stat -c%s "${'$'}TARBALL" 2>/dev/null || echo 0)
            echo "Tarball size: ${'$'}SIZE bytes"
            if [ "${'$'}SIZE" -lt 100000000 ]; then
                echo "ERROR: Tarball too small (${'$'}SIZE bytes), download may have failed"
                rm -f "${'$'}TARBALL"
                exit 1
            fi

            # Extract
            echo "Extracting to /opt/ ..."
            mkdir -p /opt
            tar xzf "${'$'}TARBALL" -C /opt/ 2>&1 | tail -5

            # Rename to standard path
            if [ -d "/opt/$PROTON_VERSION" ]; then
                rm -rf "${'$'}PROTON_DIR"
                mv "/opt/$PROTON_VERSION" "${'$'}PROTON_DIR"
            fi

            # Verify
            if [ -x "${'$'}PROTON_DIR/files/bin/wine64" ]; then
                echo ""
                echo "=== Proton-GE installed successfully ==="
                "${'$'}PROTON_DIR/files/bin/wine64" --version
                echo "Location: ${'$'}PROTON_DIR"
                ls -la "${'$'}PROTON_DIR/files/bin/" | head -20
            else
                echo "ERROR: wine64 not found after extraction!"
                ls -la "${'$'}PROTON_DIR/" 2>/dev/null || echo "${'$'}PROTON_DIR does not exist"
                exit 1
            fi

            # Clean up tarball to save space
            rm -f "${'$'}TARBALL"
            echo "Cleaned up tarball"
            echo "Done!"
        """.trimIndent()
    }

    /**
     * Get environment variables for running Wine/Proton inside FEX.
     * These are GUEST-side env vars (paths inside the rootfs).
     */
    fun getWineEnvironment(
        winePrefix: String = "/home/user/.wine",
        enableDxvkHud: Boolean = true,
        dxvkLogLevel: String = "info"
    ): Map<String, String> {
        return mapOf(
            // Wine/Proton paths
            "WINEPREFIX" to winePrefix,
            "PATH" to "$PROTON_INSTALL_DIR/files/bin:/usr/local/bin:/usr/bin:/bin",
            "WINEDLLPATH" to "$PROTON_INSTALL_DIR/files/lib64/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix",
            "WINELOADER" to "$PROTON_INSTALL_DIR/files/bin/wine64",
            "WINESERVER" to "$PROTON_INSTALL_DIR/files/bin/wineserver",

            // Display (Xvfb in TCP mode)
            "DISPLAY" to "localhost:99",

            // DXVK (DirectX 11 -> Vulkan translation)
            "DXVK_ASYNC" to "1",
            "DXVK_STATE_CACHE" to "1",
            "DXVK_LOG_LEVEL" to dxvkLogLevel,
            "DXVK_HUD" to if (enableDxvkHud) "fps,devinfo" else "",

            // VKD3D (DirectX 12 -> Vulkan translation)
            "VKD3D_FEATURE_LEVEL" to "12_1",

            // Proton compatibility flags
            "PROTON_NO_ESYNC" to "1",     // eventfd may not work on Android
            "PROTON_NO_FSYNC" to "1",     // futex_waitv not available on Android
            "PROTON_USE_WINED3D" to "0",  // Use DXVK (Vulkan), not OpenGL
            "PROTON_ENABLE_NVAPI" to "0",
            "PROTON_HIDE_NVIDIA_GPU" to "0",

            // Vulkan ICD (guest-side, through FEX thunks to Vortek)
            "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/vortek_icd.json",

            // Headless frame capture (intercepted by vulkan_headless.so)
            "LD_PRELOAD" to "/usr/lib/libvulkan_headless.so",

            // Mali GPU workarounds
            "MALI_NO_ASYNC_COMPUTE" to "1",

            // Misc
            "TERM" to "xterm-256color",
            "XDG_RUNTIME_DIR" to "/tmp",
            "TMPDIR" to "/tmp"
        )
    }

    /**
     * Command to start Xvfb in TCP-only mode (Android SELinux blocks Unix sockets).
     * Must be run before any Wine/X11 commands.
     */
    fun getXvfbStartCommand(): String {
        return """
            # Kill any existing Xvfb
            pkill -f 'Xvfb :99' 2>/dev/null || true
            sleep 0.5

            # Create X11 socket dir (Xvfb needs it even in TCP mode)
            mkdir -p /tmp/.X11-unix

            # Start Xvfb on display :99, TCP only (no Unix sockets — Android SELinux blocks them)
            Xvfb :99 -screen 0 1280x720x24 -ac -nolisten local -nolisten unix -listen tcp &
            XVFB_PID=${'$'}!
            sleep 1

            # Verify
            if kill -0 ${'$'}XVFB_PID 2>/dev/null; then
                echo "Xvfb started (PID ${'$'}XVFB_PID) on display :99 (TCP mode)"
            else
                echo "ERROR: Xvfb failed to start"
                exit 1
            fi
        """.trimIndent()
    }

    /**
     * Command to initialize a Wine prefix using Proton-GE's Wine.
     * Creates the drive_c directory structure, registry, and installs DXVK DLLs.
     */
    fun getWineBootCommand(winePrefix: String = "/home/user/.wine"): String {
        return """
            export WINEPREFIX="$winePrefix"
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            export DISPLAY=localhost:99
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1

            echo "=== Initializing Wine Prefix ==="
            echo "WINEPREFIX=$winePrefix"

            # Create prefix directory
            mkdir -p "$winePrefix"

            # Initialize the Wine prefix (creates drive_c, registry, etc.)
            echo "Running wineboot -u (this may take a minute)..."
            wineboot -u 2>&1

            # Verify
            if [ -d "$winePrefix/drive_c" ]; then
                echo ""
                echo "=== Wine prefix initialized successfully ==="
                echo "drive_c contents:"
                ls "$winePrefix/drive_c/"
                echo ""
                echo "DXVK DLLs (bundled in Proton):"
                ls "$winePrefix/drive_c/windows/system32/d3d11.dll" 2>/dev/null && echo "  d3d11.dll OK" || echo "  d3d11.dll not found (will be installed on first run)"
            else
                echo "ERROR: Wine prefix creation failed"
                exit 1
            fi
        """.trimIndent()
    }

    /**
     * Build the command to launch a Windows executable via Wine.
     *
     * @param exePath Path to the .exe inside the rootfs (e.g., /home/user/games/ysix/YsIX.exe)
     * @param winePrefix Wine prefix path
     * @param extraArgs Additional arguments for the exe
     */
    fun getLaunchCommand(
        exePath: String,
        winePrefix: String = "/home/user/.wine",
        extraArgs: String = ""
    ): String {
        val exeDir = File(exePath).parent ?: "/home/user/games"

        return """
            export WINEPREFIX="$winePrefix"
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            export WINEDLLPATH="$PROTON_INSTALL_DIR/files/lib64/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix"
            export WINELOADER="$PROTON_INSTALL_DIR/files/bin/wine64"
            export WINESERVER="$PROTON_INSTALL_DIR/files/bin/wineserver"
            export DISPLAY=localhost:99

            # DXVK settings
            export DXVK_ASYNC=1
            export DXVK_STATE_CACHE=1
            export DXVK_LOG_LEVEL=info
            export DXVK_HUD=fps,devinfo

            # Proton compatibility
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1
            export PROTON_USE_WINED3D=0

            # Vulkan via Vortek (thunks handle x86-64 -> ARM64 translation)
            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/vortek_icd.json
            export MALI_NO_ASYNC_COMPUTE=1

            # Headless frame capture → TCP 19850 → Android SurfaceView
            export LD_PRELOAD=/usr/lib/libvulkan_headless.so

            # Misc
            export XDG_RUNTIME_DIR=/tmp
            export TMPDIR=/tmp

            echo "=== Launching: $exePath ==="
            echo "Working dir: $exeDir"
            echo "Wine prefix: $winePrefix"
            echo ""

            cd "$exeDir"
            wine64 "$exePath" $extraArgs 2>&1
        """.trimIndent()
    }

    /**
     * Command for incremental testing — Step 1: Verify wine64 binary works under FEX.
     */
    fun getWineVersionTestCommand(): String {
        return """
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            echo "=== Wine Version Test ==="
            echo "Testing wine64 binary under FEX-Emu..."
            wine64 --version 2>&1
            echo ""
            echo "Wine server:"
            wineserver --version 2>&1 || echo "(wineserver --version not supported)"
            echo ""
            echo "Wine libraries:"
            ls -la "$PROTON_INSTALL_DIR/files/lib64/wine/x86_64-unix/" 2>/dev/null | head -20
        """.trimIndent()
    }

    /**
     * Command for incremental testing — Step 2: Run Notepad (basic Windows app).
     * Requires Xvfb to be running.
     */
    fun getNotepadTestCommand(): String {
        return """
            export WINEPREFIX="/home/user/.wine"
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            export DISPLAY=localhost:99
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1

            echo "=== Notepad Test ==="
            echo "Starting Wine notepad (should open and close within timeout)..."
            timeout 15 wine64 notepad 2>&1 || true
            echo ""
            echo "Test complete"
        """.trimIndent()
    }

    /**
     * Get command to check if the rootfs has the required dependencies for Wine.
     */
    fun getDependencyCheckCommand(): String {
        return """
            echo "=== Proton-GE Dependency Check ==="
            echo ""

            echo "--- Python3 (Proton launcher needs it) ---"
            python3 --version 2>&1 || echo "MISSING: python3"
            echo ""

            echo "--- Core Libraries ---"
            for lib in libfreetype.so.6 libfontconfig.so.1 libSDL2-2.0.so.0 libxkbcommon.so.0; do
                if find /usr/lib/x86_64-linux-gnu -name "${'$'}lib" 2>/dev/null | head -1 | grep -q .; then
                    echo "OK: ${'$'}lib"
                else
                    echo "MISSING: ${'$'}lib"
                fi
            done
            echo ""

            echo "--- X11 Libraries ---"
            for lib in libX11.so.6 libXext.so.6 libXrandr.so.2 libXcursor.so.1 libXi.so.6; do
                if find /usr/lib/x86_64-linux-gnu -name "${'$'}lib" 2>/dev/null | head -1 | grep -q .; then
                    echo "OK: ${'$'}lib"
                else
                    echo "MISSING: ${'$'}lib"
                fi
            done
            echo ""

            echo "--- Xvfb ---"
            which Xvfb 2>/dev/null && echo "OK: Xvfb found" || echo "MISSING: Xvfb (apt install xvfb)"
            echo ""

            echo "--- Proton-GE ---"
            if [ -x "$PROTON_INSTALL_DIR/files/bin/wine64" ]; then
                echo "OK: wine64 found"
                "$PROTON_INSTALL_DIR/files/bin/wine64" --version 2>&1
            else
                echo "NOT INSTALLED: Run 'Setup Proton' first"
            fi
            echo ""

            echo "--- Disk Space ---"
            df -h / 2>/dev/null | tail -1
            echo ""

            echo "--- Memory ---"
            free -m 2>/dev/null | head -2
        """.trimIndent()
    }

    /**
     * Get command to move game files from a source directory.
     * The user should push game files via adb to /tmp/ first.
     */
    fun getMoveGameCommand(gameName: String, sourceDir: String): String {
        val targetDir = "$GAMES_DIR/$gameName"
        return """
            echo "=== Moving game files ==="
            mkdir -p "$targetDir"

            if [ ! -d "$sourceDir" ]; then
                echo "ERROR: Source directory not found: $sourceDir"
                echo ""
                echo "Push game files first:"
                echo "  adb push /path/to/game/ /data/.../fex-rootfs/Ubuntu_22_04/tmp/game/"
                echo "  (or place them directly in the rootfs)"
                exit 1
            fi

            echo "Source: $sourceDir"
            echo "Target: $targetDir"
            echo ""

            cp -av "$sourceDir"/* "$targetDir"/ 2>&1

            echo ""
            echo "=== Game files installed ==="
            echo "Contents:"
            ls -la "$targetDir/" | head -20

            # Find .exe files
            echo ""
            echo "Executables found:"
            find "$targetDir" -name "*.exe" -o -name "*.EXE" 2>/dev/null | head -10
        """.trimIndent()
    }
}
