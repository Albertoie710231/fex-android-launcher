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

            # Replace system Wine with RELATIVE symlinks to Proton-GE
            # CRITICAL: Must be relative — absolute symlinks escape FEX rootfs overlay!
            echo ""
            echo "=== Redirecting system Wine paths to Proton-GE ==="
            if [ -d "/usr/lib/x86_64-linux-gnu/wine" ] && [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
                mv /usr/lib/x86_64-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine.system.bak
            fi
            [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ] && ln -sf ../../../opt/proton-ge/files/lib/wine /usr/lib/x86_64-linux-gnu/wine
            if [ -d "/usr/lib/wine" ] && [ ! -L "/usr/lib/wine" ]; then
                mv /usr/lib/wine /usr/lib/wine.system.bak
            fi
            rm -f /usr/lib/wine 2>/dev/null; ln -sf ../../opt/proton-ge/files/lib/wine /usr/lib/wine
            for bin in wine wine64 wineserver wineboot winecfg; do
                [ -f "/usr/bin/${'$'}bin" ] && rm -f "/usr/bin/${'$'}bin"
            done
            echo "System Wine paths now point to Proton-GE"
            echo ""
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
        dxvkLogLevel: String = "trace"
    ): Map<String, String> {
        return mapOf(
            // Wine/Proton paths
            "WINEPREFIX" to winePrefix,
            "PATH" to "$PROTON_INSTALL_DIR/files/bin:/usr/local/bin:/usr/bin:/bin",
            "WINEDLLPATH" to "$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-windows",
            "WINELOADER" to "$PROTON_INSTALL_DIR/files/bin/wine",
            "WINESERVER" to "$PROTON_INSTALL_DIR/files/bin/wineserver",
            "LD_LIBRARY_PATH" to "$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib",

            // X11 via libXlorie (native ARM64 X server, abstract socket @/tmp/.X11-unix/X0)
            // DISPLAY=:0 uses abstract sockets (NOT TCP) — FEX passes connect() to host kernel
            "DISPLAY" to ":0",

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

            // Vulkan ICD (x86-64 shim → FEX thunks → host Vortek)
            "VK_ICD_FILENAMES" to "/usr/share/vulkan/icd.d/fex_thunk_icd.json",

            // Headless frame capture via Vulkan implicit layer (LD_PRELOAD blocked by AT_SECURE)
            "HEADLESS_LAYER" to "1",

            // Mali GPU workarounds
            "MALI_NO_ASYNC_COMPUTE" to "1",

            // Disable VR in Proton
            "PROTON_ENABLE_NVAPI" to "0",

            // DLL overrides: use DXVK (native) for D3D, disable wined3d (SIGILL),
            // use our stub DLLs for d3dcompiler_47, and disable mscoree/mshtml (.NET/IE)
            "WINEDLLOVERRIDES" to "d3d11=n;d3d10core=n;d3d9=n;dxgi=n;d3d8=n;d3dcompiler_47=n;d3dcompiler_43=n;wined3d=d;mscoree=d;mshtml=d;steam_api64=n;steam_api=n;openvr_api_dxvk=d;d3d12=d;d3d12core=d",
            "WINEDEBUG" to "err+all",

            // Misc
            "TERM" to "xterm-256color",
            "XDG_RUNTIME_DIR" to "/tmp",
            "TMPDIR" to "/tmp"
        )
    }

    /**
     * Command to check X11 server connectivity from FEX guest side.
     * libXlorie runs natively on Android (ARM64), started by TerminalActivity.
     * Wine connects via DISPLAY=:0 (abstract socket @/tmp/.X11-unix/X0).
     */
    fun getXServerStartCommand(): String {
        return """
            echo "=== X11 Server Status ==="
            echo "libXlorie (native ARM64) should be started from Android side."
            echo "Press 'Start X' button in the app first."
            echo ""
            echo "libXlorie uses abstract socket @/tmp/.X11-unix/X0"
            echo "FEX guest connects via DISPLAY=:0 (abstract socket, NOT TCP)"
            echo "Checking if abstract socket exists..."
            if [ -e /tmp/.X11-unix/X0 ] || python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
try: s.connect('\x00/tmp/.X11-unix/X0'); print('OK: abstract socket reachable'); s.close()
except: print('NOT REACHABLE: abstract socket @/tmp/.X11-unix/X0'); sys.exit(1)
" 2>/dev/null; then
                echo "Use: export DISPLAY=:0"
            else
                echo "NOT RUNNING: X11 server not detected"
                echo "Press the 'Start X' button in the app toolbar."
            fi
        """.trimIndent()
    }

    /**
     * Command to remove system Wine 6.0.3 from rootfs.
     * System Wine's ntdll.so causes dladdr() to return wrong paths,
     * making Wine derive data_dir with ".." components that fail under
     * FEX's \\?\ NT path prefix. Removing it forces Wine to use Proton-GE's modules.
     */
    fun getCleanSystemWineCommand(): String {
        return """
            echo "=== Replacing System Wine with Proton-GE (relative symlinks) ==="

            # CRITICAL: Use RELATIVE symlinks — absolute ones escape FEX rootfs overlay!
            # /usr/lib/x86_64-linux-gnu/wine -> ../../../opt/proton-ge/files/lib/wine
            if [ -d "/usr/lib/x86_64-linux-gnu/wine" ] && [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
                echo "Backing up system Wine modules..."
                mv /usr/lib/x86_64-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine.system.bak
            fi
            if [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
                ln -sf ../../../opt/proton-ge/files/lib/wine /usr/lib/x86_64-linux-gnu/wine
                echo "Created: /usr/lib/x86_64-linux-gnu/wine -> ../../../opt/proton-ge/files/lib/wine"
            else
                echo "Symlink already exists: /usr/lib/x86_64-linux-gnu/wine"
                ls -la /usr/lib/x86_64-linux-gnu/wine
            fi

            # /usr/lib/wine -> ../../opt/proton-ge/files/lib/wine
            if [ -d "/usr/lib/wine" ] && [ ! -L "/usr/lib/wine" ]; then
                mv /usr/lib/wine /usr/lib/wine.system.bak
            fi
            if [ ! -L "/usr/lib/wine" ]; then
                rm -f /usr/lib/wine
                ln -sf ../../opt/proton-ge/files/lib/wine /usr/lib/wine
                echo "Created: /usr/lib/wine -> ../../opt/proton-ge/files/lib/wine"
            else
                echo "Symlink already exists: /usr/lib/wine"
                ls -la /usr/lib/wine
            fi

            # Verify symlinks work
            echo ""
            echo "Verification:"
            ls /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/kernel32.dll 2>/dev/null && echo "  kernel32.dll: OK" || echo "  kernel32.dll: NOT FOUND"

            # Remove system Wine binaries
            for bin in wine wine64 wineserver wineboot winecfg msiexec regedit regsvr32; do
                [ -f "/usr/bin/${'$'}bin" ] && rm -f "/usr/bin/${'$'}bin"
            done
            echo "System Wine replaced with Proton-GE"
        """.trimIndent()
    }

    /**
     * Command to initialize a Wine prefix using Proton-GE's Wine.
     * Creates the drive_c directory structure, registry, and installs DXVK DLLs.
     * Includes system Wine cleanup to prevent dladdr() path interference.
     */
    fun getWineBootCommand(winePrefix: String = "/home/user/.wine"): String {
        return """
            export WINEPREFIX="$winePrefix"
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            export WINEDLLPATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-windows"
            export WINELOADER="$PROTON_INSTALL_DIR/files/bin/wine"
            export WINESERVER="$PROTON_INSTALL_DIR/files/bin/wineserver"
            export DISPLAY=:0
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1
            export LD_LIBRARY_PATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib:${'$'}{LD_LIBRARY_PATH:-}"

            # ---- Replace system Wine with RELATIVE symlinks to Proton-GE ----
            # CRITICAL: Symlinks MUST be relative — absolute symlinks escape FEX rootfs overlay!
            if [ -d "/usr/lib/x86_64-linux-gnu/wine" ] && [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
                echo "Replacing system Wine modules with relative symlink to Proton-GE..."
                mv /usr/lib/x86_64-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine.system.bak
            fi
            if [ ! -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
                ln -sf ../../../opt/proton-ge/files/lib/wine /usr/lib/x86_64-linux-gnu/wine
            fi
            if [ -d "/usr/lib/wine" ] && [ ! -L "/usr/lib/wine" ]; then
                mv /usr/lib/wine /usr/lib/wine.system.bak
            fi
            if [ ! -L "/usr/lib/wine" ]; then
                rm -f /usr/lib/wine
                ln -sf ../../opt/proton-ge/files/lib/wine /usr/lib/wine
            fi
            for bin in wine wine64 wineserver wineboot winecfg; do
                [ -f "/usr/bin/${'$'}bin" ] && rm -f "/usr/bin/${'$'}bin"
            done

            echo "=== Initializing Wine Prefix ==="
            echo "WINEPREFIX=$winePrefix"

            # Fix any existing absolute symlinks (must be relative for FEX overlay)
            if [ -L "/usr/lib/x86_64-linux-gnu/wine" ]; then
                TARGET=${'$'}(readlink /usr/lib/x86_64-linux-gnu/wine)
                case "${'$'}TARGET" in /*)
                    rm -f /usr/lib/x86_64-linux-gnu/wine
                    ln -sf ../../../opt/proton-ge/files/lib/wine /usr/lib/x86_64-linux-gnu/wine
                    echo "Fixed absolute symlink -> relative"
                ;; esac
            fi
            if [ -L "/usr/lib/wine" ]; then
                TARGET=${'$'}(readlink /usr/lib/wine)
                case "${'$'}TARGET" in /*)
                    rm -f /usr/lib/wine
                    ln -sf ../../opt/proton-ge/files/lib/wine /usr/lib/wine
                    echo "Fixed absolute symlink -> relative"
                ;; esac
            fi

            # Remove stale prefix (may have been created with wrong Wine)
            if [ -d "$winePrefix" ]; then
                echo "Removing stale Wine prefix..."
                rm -rf "$winePrefix"
            fi

            # Create prefix directory structure
            mkdir -p "$winePrefix/drive_c/windows/system32"
            mkdir -p "$winePrefix/drive_c/windows/syswow64"

            # CRITICAL: Fix Z: drive to point to HOST rootfs path.
            # Wine resolves dosdevices/z: symlink via kernel, which follows to the
            # REAL filesystem root. Default z:->/ goes to Android root (wrong!).
            # Must point to the actual FEX rootfs directory on the host.
            mkdir -p "$winePrefix/dosdevices"
            ln -sf ../drive_c "$winePrefix/dosdevices/c:"
            ln -sf "$fexRootfsDir" "$winePrefix/dosdevices/z:"
            echo "Z: drive -> $fexRootfsDir (host rootfs)"

            # Pre-seed system32 with Proton-GE PE DLLs AND EXEs (Wine's builtin
            # search may fail under FEX because dladdr() returns host paths).
            # EXEs are critical: explorer.exe (desktop), services.exe, winedevice.exe, etc.
            echo "Pre-seeding system32 with Proton-GE DLLs + EXEs..."
            PROTON_WIN64="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows"
            PROTON_WIN32="$PROTON_INSTALL_DIR/files/lib/wine/i386-windows"
            cp "${'$'}PROTON_WIN64"/*.dll "$winePrefix/drive_c/windows/system32/" 2>/dev/null || true
            cp "${'$'}PROTON_WIN64"/*.exe "$winePrefix/drive_c/windows/system32/" 2>/dev/null || true
            cp "${'$'}PROTON_WIN64"/*.drv "$winePrefix/drive_c/windows/system32/" 2>/dev/null || true
            cp "${'$'}PROTON_WIN32"/*.dll "$winePrefix/drive_c/windows/syswow64/" 2>/dev/null || true
            cp "${'$'}PROTON_WIN32"/*.exe "$winePrefix/drive_c/windows/syswow64/" 2>/dev/null || true
            cp "${'$'}PROTON_WIN32"/*.drv "$winePrefix/drive_c/windows/syswow64/" 2>/dev/null || true
            SYS32_COUNT=${'$'}(ls "$winePrefix/drive_c/windows/system32/"*.dll 2>/dev/null | wc -l)
            SYS32_EXE=${'$'}(ls "$winePrefix/drive_c/windows/system32/"*.exe 2>/dev/null | wc -l)
            echo "  system32: ${'$'}SYS32_COUNT DLLs, ${'$'}SYS32_EXE EXEs"

            # Copy key EXEs to windows root (Wine looks here for explorer, etc.)
            for exe in explorer.exe notepad.exe regedit.exe hh.exe; do
                if [ -f "${'$'}PROTON_WIN64/${'$'}exe" ]; then
                    cp "${'$'}PROTON_WIN64/${'$'}exe" "$winePrefix/drive_c/windows/${'$'}exe"
                fi
            done
            echo "  windows root: explorer.exe + key EXEs"

            # Initialize the Wine prefix (creates drive_c, registry, etc.)
            echo "Running wineboot -u (this may take a minute)..."
            wine64 wineboot -u 2>&1

            # Verify
            if [ -d "$winePrefix/drive_c" ]; then
                echo ""
                echo "=== Wine prefix initialized successfully ==="
                echo "drive_c contents:"
                ls "$winePrefix/drive_c/"
                echo ""
                echo "system32 DLLs:"
                ls "$winePrefix/drive_c/windows/system32/" 2>/dev/null | wc -l
                echo "files in system32"
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
            export WINEDLLPATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-windows"
            export WINELOADER="$PROTON_INSTALL_DIR/files/bin/wine"
            export WINESERVER="$PROTON_INSTALL_DIR/files/bin/wineserver"
            export DISPLAY=:0
            export LD_LIBRARY_PATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib:${'$'}{LD_LIBRARY_PATH:-}"

            # DXVK settings
            export DXVK_ASYNC=1
            export DXVK_STATE_CACHE=1
            export DXVK_LOG_LEVEL=trace
            export DXVK_HUD=fps,devinfo

            # Proton compatibility
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1
            export PROTON_USE_WINED3D=0

            # Vulkan ICD: x86-64 shim → FEX thunks → host Vortek → Mali GPU
            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/fex_thunk_icd.json
            export MALI_NO_ASYNC_COMPUTE=1

            # Headless frame capture → TCP 19850 → Android SurfaceView
            # Use Vulkan implicit layer instead of LD_PRELOAD (LD_PRELOAD blocked by AT_SECURE on Android)
            export HEADLESS_LAYER=1

            # DLL overrides: DXVK for D3D, disable wined3d (SIGILL), use stub DLLs
            export WINEDLLOVERRIDES="d3d11=n;d3d10core=n;d3d9=n;dxgi=n;d3d8=n;d3dcompiler_47=n;d3dcompiler_43=n;wined3d=d;mscoree=d;mshtml=d;steam_api64=n;steam_api=n;openvr_api_dxvk=d;d3d12=d;d3d12core=d;xaudio2_0=d;xaudio2_1=d;xaudio2_2=d;xaudio2_3=d;xaudio2_4=d;xaudio2_5=d;xaudio2_6=d;xaudio2_7=d;xaudio2_8=d;xaudio2_9=d;x3daudio1_7=d;x3daudio1_0=d;mfplat=d;mfreadwrite=d;mf=d;mfplay=d;quartz=d;wmvcore=d"
            export WINEDEBUG=err+all

            # Misc
            export XDG_RUNTIME_DIR=/tmp
            export TMPDIR=/tmp

            # Fix Z: drive to point to host rootfs (kernel resolves symlinks via real FS)
            if [ -d "${'$'}WINEPREFIX/dosdevices" ]; then
                rm -f "${'$'}WINEPREFIX/dosdevices/z:"
                ln -sf "$fexRootfsDir" "${'$'}WINEPREFIX/dosdevices/z:"
            fi

            # Ensure critical EXEs are in the prefix (pre-seed may have missed them)
            PROTON_WIN64="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows"
            SYS32="${'$'}WINEPREFIX/drive_c/windows/system32"
            WINDIR="${'$'}WINEPREFIX/drive_c/windows"
            if [ ! -f "${'$'}SYS32/explorer.exe" ] || [ ! -f "${'$'}SYS32/winex11.drv" ]; then
                echo "Fixing missing EXEs/DRVs in prefix..."
                cp "${'$'}PROTON_WIN64"/*.exe "${'$'}SYS32/" 2>/dev/null || true
                cp "${'$'}PROTON_WIN64"/*.drv "${'$'}SYS32/" 2>/dev/null || true
                for exe in explorer.exe notepad.exe regedit.exe hh.exe; do
                    [ -f "${'$'}PROTON_WIN64/${'$'}exe" ] && cp "${'$'}PROTON_WIN64/${'$'}exe" "${'$'}WINDIR/${'$'}exe"
                done
                echo "  Deployed EXEs + DRVs to system32 + windows root"
            fi

            # Install standalone DXVK DLLs to system32 (from Proton's dxvk/ directory)
            # These use Vulkan directly — unlike Wine's builtins which need wined3d/VKD3D
            DXVK_DIR="$PROTON_INSTALL_DIR/files/lib/wine/dxvk/x86_64-windows"
            mkdir -p "${'$'}SYS32"
            for dll in d3d11.dll dxgi.dll d3d10core.dll d3d9.dll d3d8.dll; do
                if [ -f "${'$'}DXVK_DIR/${'$'}dll" ]; then
                    cp "${'$'}DXVK_DIR/${'$'}dll" "${'$'}SYS32/${'$'}dll"
                fi
            done
            # d3dcompiler_47 stub
            [ -f "/opt/stubs/d3dcompiler_47.dll" ] && cp "/opt/stubs/d3dcompiler_47.dll" "${'$'}SYS32/d3dcompiler_47.dll"
            echo "DXVK standalone DLLs installed to system32"

            # Create DXVK config to disable OpenVR/OpenXR (no VR hardware, avoids extension query crash)
            cat > "$exeDir/dxvk.conf" << 'DXVKEOF'
# DXVK config for Android/FEX-Emu
dxgi.enableOpenVR = False
dxgi.enableOpenXR = False
dxgi.maxFrameLatency = 1
dxvk.logLevel = trace
DXVKEOF

            # Deploy game-specific stub DLLs (backup originals if present)
            for stub in Galaxy64.dll GFSDK_SSAO_D3D11.win64.dll steam_api64.dll; do
                if [ -f "/opt/stubs/${'$'}stub" ]; then
                    if [ -f "$exeDir/${'$'}stub" ] && [ ! -f "$exeDir/${'$'}{stub}.orig" ]; then
                        cp "$exeDir/${'$'}stub" "$exeDir/${'$'}{stub}.orig"
                        echo "Backed up ${'$'}stub -> ${'$'}{stub}.orig"
                    fi
                    cp "/opt/stubs/${'$'}stub" "$exeDir/${'$'}stub"
                    echo "Deployed stub: ${'$'}stub"
                fi
            done

            # X11: always use abstract socket (libXlorie), never TCP
            echo "=== X11 Display Check ==="
            export DISPLAY=:0
            echo "DISPLAY=:0 (abstract socket)"

            # Disable XRandR — libXlorie doesn't support mode switching,
            # causing NtUserChangeDisplaySettings to return -2 and Wine to poll forever.
            wine64 reg add 'HKCU\Software\Wine\X11 Driver' /v UseXRandr /t REG_SZ /d N /f 2>/dev/null
            wine64 reg add 'HKCU\Software\Wine\X11 Driver' /v UseXVidMode /t REG_SZ /d N /f 2>/dev/null
            echo "Disabled XRandR/XVidMode in Wine registry"

            echo "=== Launching: $exePath ==="
            echo "Working dir: $exeDir"
            echo "Wine prefix: $winePrefix"
            echo ""

            # Clear old layer trace log for this run
            rm -f /tmp/layer_trace.log

            cd "$exeDir"

            # Create steam_appid.txt BEFORE wine launch (prevents Steam client check)
            echo "1351630" > "$exeDir/steam_appid.txt" 2>/dev/null

            # Dump game's PE imports to identify which DLLs are loaded
            echo "=== PE IMPORTS ==="
            objdump -p "$exePath" 2>/dev/null | grep "DLL Name" | head -30 || echo "(objdump not available)"
            echo "=== END PE IMPORTS ==="

            # Launch wine in background so we can inspect its threads
            wine64 "$exePath" $extraArgs 2>&1 &
            WINE_PID=${'$'}!
            echo "Wine PID: ${'$'}WINE_PID"

            # Thread diagnostic — sample at t+15s and t+45s to see if CPU increases
            dump_threads() {
                local LABEL=${'$'}1
                echo ""
                echo "=== THREAD DIAGNOSTIC (${'$'}LABEL, Wine PID=${'$'}WINE_PID) ==="
                if [ -d /proc/${'$'}WINE_PID ]; then
                    for tp in /proc/${'$'}WINE_PID/task/*; do
                        tid=${'$'}{tp##*/}
                        STATE=${'$'}(grep '^State:' ${'$'}tp/status 2>/dev/null | awk '{print ${'$'}2, ${'$'}3}')
                        WCHAN=${'$'}(cat ${'$'}tp/wchan 2>/dev/null)
                        SYSCALL=${'$'}(cat ${'$'}tp/syscall 2>/dev/null | cut -d' ' -f1)
                        UT=${'$'}(awk '{print ${'$'}14}' ${'$'}tp/stat 2>/dev/null)
                        ST=${'$'}(awk '{print ${'$'}15}' ${'$'}tp/stat 2>/dev/null)
                        MINFLT=${'$'}(awk '{print ${'$'}10}' ${'$'}tp/stat 2>/dev/null)
                        MAJFLT=${'$'}(awk '{print ${'$'}12}' ${'$'}tp/stat 2>/dev/null)
                        echo "  TID ${'$'}tid: ${'$'}STATE wchan=${'$'}WCHAN u=${'$'}UT s=${'$'}ST minflt=${'$'}MINFLT majflt=${'$'}MAJFLT sys=${'$'}SYSCALL"
                    done
                else
                    echo "  Wine PID ${'$'}WINE_PID no longer exists!"
                fi
                echo "=== END DIAGNOSTIC ==="
            }

            sleep 15
            dump_threads "t+15s"

            # Dump memory maps (raw, first 40 lines + last 20)
            echo ""
            echo "=== PROC MAPS (head) ==="
            head -40 /proc/${'$'}WINE_PID/maps 2>/dev/null || echo "(maps not readable)"
            echo "..."
            echo "=== PROC MAPS (named regions) ==="
            grep -v '^\S* \S* \S* \S* 0 ' /proc/${'$'}WINE_PID/maps 2>/dev/null | tail -30
            echo "=== END MAPS ==="

            sleep 15
            dump_threads "t+30s"

            # Wait for wine to finish
            wait ${'$'}WINE_PID 2>/dev/null
            WINE_EXIT=${'$'}?
            echo ""
            echo "[wine64 exit code: ${'$'}WINE_EXIT]"

            # Dump headless layer trace log (critical for BC spoofing diagnostics)
            echo "=== /tmp/layer_trace.log ==="
            cat /tmp/layer_trace.log 2>/dev/null || echo "(no layer_trace.log)"
            echo "=== end layer_trace.log ==="

            if [ ${'$'}WINE_EXIT -eq 132 ]; then
                echo "=== SIGILL CRASH — FEX debug log (last 100 lines) ==="
                tail -100 /tmp/fex-debug.log 2>/dev/null || echo "(no FEX log at /tmp)"
            fi
        """.trimIndent()
    }

    /**
     * Test vkBeginCommandBuffer through Wine's winevulkan path.
     * This isolates whether the hang is in Wine's Vulkan PE/unix bridge.
     */
    fun getVkCmdBufTestCommand(): String {
        return """
            export WINEPREFIX="/home/user/.wine"
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            export WINEDLLPATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-windows"
            export WINELOADER="$PROTON_INSTALL_DIR/files/bin/wine"
            export WINESERVER="$PROTON_INSTALL_DIR/files/bin/wineserver"
            export LD_LIBRARY_PATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib:${'$'}{LD_LIBRARY_PATH:-}"
            export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/fex_thunk_icd.json
            export MALI_NO_ASYNC_COMPUTE=1
            export HEADLESS_LAYER=1
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1
            export WINEDLLOVERRIDES="d3d11=n;d3d10core=n;d3d9=n;dxgi=n;d3d8=n;d3dcompiler_47=n;d3dcompiler_43=n;wined3d=d;mscoree=d;mshtml=d"
            export WINEDEBUG=err+all

            echo "=== Vulkan CmdBuf Test (FULL game env: HeadlessLayer + Proton winevulkan) ==="
            echo "HEADLESS_LAYER=${'$'}HEADLESS_LAYER"
            echo "VK_ICD_FILENAMES=${'$'}VK_ICD_FILENAMES"
            echo ""

            # Run with 30s timeout — if it hangs on vkBeginCommandBuffer, timeout fires
            timeout 30 wine64 /opt/stubs/test_vk_cmdbuf.exe 2>&1
            EXIT=${'$'}?
            echo ""
            if [ ${'$'}EXIT -eq 0 ]; then
                echo "=== TEST PASSED: vkBeginCommandBuffer works through Wine ==="
            elif [ ${'$'}EXIT -eq 124 ]; then
                echo "=== TEST FAILED: TIMEOUT (30s) — vkBeginCommandBuffer HANGS through Wine ==="
            else
                echo "=== TEST FAILED: exit code ${'$'}EXIT ==="
            fi
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
            ls -la "$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix/" 2>/dev/null | head -20
        """.trimIndent()
    }

    /**
     * Command for incremental testing — Step 2: Run Notepad (basic Windows app).
     * Requires libXlorie X11 server (press 'Start X' button first).
     */
    fun getNotepadTestCommand(): String {
        return """
            export WINEPREFIX="/home/user/.wine"
            export PATH="$PROTON_INSTALL_DIR/files/bin:${'$'}PATH"
            export WINEDLLPATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib/wine/x86_64-windows:$PROTON_INSTALL_DIR/files/lib/wine/i386-unix:$PROTON_INSTALL_DIR/files/lib/wine/i386-windows"
            export WINELOADER="$PROTON_INSTALL_DIR/files/bin/wine"
            export WINESERVER="$PROTON_INSTALL_DIR/files/bin/wineserver"
            export PROTON_NO_ESYNC=1
            export PROTON_NO_FSYNC=1
            export LD_LIBRARY_PATH="$PROTON_INSTALL_DIR/files/lib/wine/x86_64-unix:$PROTON_INSTALL_DIR/files/lib:${'$'}{LD_LIBRARY_PATH:-}"

            # Critical: disable wined3d (SIGILL from Mesa GLX) and unnecessary DLLs
            export WINEDLLOVERRIDES="wined3d=d;mscoree=d;mshtml=d"
            export WINEDEBUG=err+all

            # X11: always use abstract socket (libXlorie), never TCP
            export DISPLAY=:0
            echo "DISPLAY=:0 (abstract socket)"

            # Disable XRandR/XVidMode (libXlorie doesn't support mode switching)
            wine64 reg add 'HKCU\Software\Wine\X11 Driver' /v UseXRandr /t REG_SZ /d N /f 2>/dev/null
            wine64 reg add 'HKCU\Software\Wine\X11 Driver' /v UseXVidMode /t REG_SZ /d N /f 2>/dev/null

            # Fix Z: drive to point to host rootfs (kernel resolves symlinks via real FS)
            if [ -d "${'$'}WINEPREFIX/dosdevices" ]; then
                rm -f "${'$'}WINEPREFIX/dosdevices/z:"
                ln -sf "$fexRootfsDir" "${'$'}WINEPREFIX/dosdevices/z:"
            fi

            echo "=== Notepad Test ==="
            echo "Testing basic Wine windowing (30s timeout)..."
            timeout 30 wine64 notepad 2>&1 || echo "[notepad timed out or exited]"
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

            echo "--- X11 Server (libXlorie via abstract socket) ---"
            python3 -c "
import socket
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
try:
    s.connect('\x00/tmp/.X11-unix/X0')
    print('OK: X11 abstract socket @/tmp/.X11-unix/X0 reachable')
    s.close()
except Exception as e:
    print(f'NOT REACHABLE: {e}')
    print('Press Start X button in the app')
" 2>&1 || echo "(python3 not available — cannot test abstract socket)"
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
            df -h /home 2>/dev/null | tail -1
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
