#!/bin/bash
# Test RE4 Remake (AppID 2050650) via steam://rungameid/ on PC
# Compare DRM/steam_api behavior between PC and Android device
#
# Usage: bash test_re4.sh
# Make sure Steam is NOT already running, or the URL will be sent via pipe to existing instance.

STEAMDIR="$HOME/.steam/steam"
APPID=2050650
GAMENAME="RESIDENT EVIL 4  BIOHAZARD RE4"

export DISPLAY=${DISPLAY:-:1}

# Don't override DBUS on real desktop — needed for Steam
# export DBUS_SESSION_BUS_ADDRESS=disabled

# Match device env vars from getSteamRunGameCommand()
export DXVK_ASYNC=1
export DXVK_STATE_CACHE=1
export DXVK_LOG_LEVEL=info
export DXVK_LOG_PATH=/tmp/dxvk_re4

export VKD3D_FEATURE_LEVEL=12_1
export VKD3D_DEBUG=warn
export VKD3D_SHADER_DEBUG=warn

export PROTON_NO_FSYNC=1
export PROTON_USE_WINED3D=0
export PROTON_ENABLE_NVAPI=0
export PROTON_HIDE_NVIDIA_GPU=0
export PROTON_LOG=1
export PROTON_LOG_DIR=/tmp/proton_re4

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAMDIR"
export STEAM_COMPAT_DATA_PATH="$STEAMDIR/steamapps/compatdata/$APPID"

# Enable Wine debug for DRM-related calls
export WINEDEBUG=+loaddll,+seh,+module

export LD_LIBRARY_PATH="$STEAMDIR/linux64:$STEAMDIR/ubuntu12_64:$STEAMDIR/ubuntu12_32:$STEAMDIR/ubuntu12_32/panorama:${LD_LIBRARY_PATH:-}"
export STEAMSCRIPT="$STEAMDIR/steam.sh"

LOGFILE=/tmp/re4_drm_test.log
COMPAT_LOG="$STEAMDIR/logs/compat_log.txt"
CONTENT_LOG="$STEAMDIR/logs/content_log.txt"
GAMEPROCESS_LOG="$STEAMDIR/logs/gameprocess_log.txt"

mkdir -p /tmp/dxvk_re4 /tmp/proton_re4

echo "=== RE4 Remake DRM Test (AppID $APPID) ===" | tee "$LOGFILE"
echo "Date: $(date)" | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"

# Check appmanifest
echo "--- appmanifest_${APPID}.acf ---" | tee -a "$LOGFILE"
MANIFEST="$STEAMDIR/steamapps/appmanifest_${APPID}.acf"
if [ -f "$MANIFEST" ]; then
    cat "$MANIFEST" | tee -a "$LOGFILE"
else
    # Check alternate Steam library paths
    for LIB in $(grep -oP '"path"\s+"\K[^"]+' "$STEAMDIR/steamapps/libraryfolders.vdf" 2>/dev/null); do
        ALT="$LIB/steamapps/appmanifest_${APPID}.acf"
        if [ -f "$ALT" ]; then
            echo "Found at: $ALT" | tee -a "$LOGFILE"
            cat "$ALT" | tee -a "$LOGFILE"
            MANIFEST="$ALT"
            break
        fi
    done
    if [ ! -f "$MANIFEST" ]; then
        echo "WARNING: appmanifest NOT FOUND" | tee -a "$LOGFILE"
    fi
fi
echo "" | tee -a "$LOGFILE"

# Check game dir
echo "--- Game directory ---" | tee -a "$LOGFILE"
GAMEDIR="$STEAMDIR/steamapps/common/$GAMENAME"
if [ ! -d "$GAMEDIR" ]; then
    # Search all library folders
    for LIB in $(grep -oP '"path"\s+"\K[^"]+' "$STEAMDIR/steamapps/libraryfolders.vdf" 2>/dev/null); do
        ALT="$LIB/steamapps/common/$GAMENAME"
        if [ -d "$ALT" ]; then
            GAMEDIR="$ALT"
            break
        fi
    done
fi
if [ -d "$GAMEDIR" ]; then
    echo "Game dir: $GAMEDIR" | tee -a "$LOGFILE"
    ls -la "$GAMEDIR/" | head -20 | tee -a "$LOGFILE"
    echo "" | tee -a "$LOGFILE"
    echo "re4.exe info:" | tee -a "$LOGFILE"
    file "$GAMEDIR/re4.exe" 2>/dev/null | tee -a "$LOGFILE"
    ls -la "$GAMEDIR/re4.exe" 2>/dev/null | tee -a "$LOGFILE"
    echo "" | tee -a "$LOGFILE"
    echo "steam_api64.dll:" | tee -a "$LOGFILE"
    ls -la "$GAMEDIR/steam_api64.dll" 2>/dev/null | tee -a "$LOGFILE"
    # Check if it's the real Steam API or a stub
    strings "$GAMEDIR/steam_api64.dll" 2>/dev/null | grep -iE "SteamAPI_Init|Valve|steamclient" | head -5 | tee -a "$LOGFILE"
else
    echo "WARNING: Game dir NOT FOUND" | tee -a "$LOGFILE"
fi
echo "" | tee -a "$LOGFILE"

# Check compatdata/prefix
echo "--- Wine prefix ---" | tee -a "$LOGFILE"
if [ -d "$STEAM_COMPAT_DATA_PATH/pfx" ]; then
    echo "Prefix exists: $STEAM_COMPAT_DATA_PATH/pfx" | tee -a "$LOGFILE"
    ls -la "$STEAM_COMPAT_DATA_PATH/pfx/drive_c/windows/system32/steam_api64.dll" 2>/dev/null | tee -a "$LOGFILE"
    echo "DLL overrides in registry:" | tee -a "$LOGFILE"
    grep -iE "steam_api|d3d12|dxgi" "$STEAM_COMPAT_DATA_PATH/pfx/user.reg" 2>/dev/null | head -10 | tee -a "$LOGFILE"
else
    echo "No prefix yet (will be created on first launch)" | tee -a "$LOGFILE"
fi
echo "" | tee -a "$LOGFILE"

# Check 228980 (Steamworks Common Redistributables) — RE4 depends on it
echo "--- AppID 228980 (dependency) ---" | tee -a "$LOGFILE"
MANIFEST_228980="$STEAMDIR/steamapps/appmanifest_228980.acf"
if [ -f "$MANIFEST_228980" ]; then
    grep -E "StateFlags|buildid|installdir" "$MANIFEST_228980" | tee -a "$LOGFILE"
else
    for LIB in $(grep -oP '"path"\s+"\K[^"]+' "$STEAMDIR/steamapps/libraryfolders.vdf" 2>/dev/null); do
        ALT="$LIB/steamapps/appmanifest_228980.acf"
        if [ -f "$ALT" ]; then
            echo "Found at: $ALT" | tee -a "$LOGFILE"
            grep -E "StateFlags|buildid|installdir" "$ALT" | tee -a "$LOGFILE"
            break
        fi
    done
fi
echo "" | tee -a "$LOGFILE"

# Clear logs before launch
echo "Clearing Steam logs..." | tee -a "$LOGFILE"
: > "$COMPAT_LOG" 2>/dev/null
: > "$CONTENT_LOG" 2>/dev/null
: > "$GAMEPROCESS_LOG" 2>/dev/null

echo "" | tee -a "$LOGFILE"
echo "=== Launching: steam steam://rungameid/$APPID ===" | tee -a "$LOGFILE"
echo "Logs will be at:" | tee -a "$LOGFILE"
echo "  Main: $LOGFILE" | tee -a "$LOGFILE"
echo "  DXVK: /tmp/dxvk_re4/" | tee -a "$LOGFILE"
echo "  Proton: /tmp/proton_re4/" | tee -a "$LOGFILE"
echo "  Wine: WINEDEBUG=+loaddll,+seh,+module" | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"

cd "$STEAMDIR"
bash steam.sh steam://rungameid/$APPID 2>&1 | tee -a "$LOGFILE" &
STEAM_PID=$!

# Monitor in background for game process and DRM activity
echo "Monitoring for game launch and DRM..." | tee -a "$LOGFILE"
WAITED=0
MAX_WAIT=120
while [ $WAITED -lt $MAX_WAIT ]; do
    sleep 5
    WAITED=$((WAITED + 5))

    # Check compat_log for session
    SESSION=$(grep "$APPID" "$COMPAT_LOG" 2>/dev/null | tail -1)
    if [ -n "$SESSION" ]; then
        echo "[$WAITED s] compat: $SESSION" | tee -a "$LOGFILE"
    fi

    # Check gameprocess_log
    GAME=$(grep "$APPID" "$GAMEPROCESS_LOG" 2>/dev/null | tail -1)
    if [ -n "$GAME" ]; then
        echo "[$WAITED s] gameprocess: $GAME" | tee -a "$LOGFILE"
    fi

    # Check content_log for 228980 and RE4
    CONTENT=$(grep -E "228980|$APPID" "$CONTENT_LOG" 2>/dev/null | tail -3)
    if [ -n "$CONTENT" ]; then
        echo "[$WAITED s] content:" | tee -a "$LOGFILE"
        echo "$CONTENT" | tee -a "$LOGFILE"
    fi

    # Check if re4.exe is actually running
    RE4_PID=$(pgrep -f "re4.exe" 2>/dev/null)
    if [ -n "$RE4_PID" ]; then
        echo "[$WAITED s] re4.exe RUNNING! PID=$RE4_PID" | tee -a "$LOGFILE"
        echo "  Process info:" | tee -a "$LOGFILE"
        ps -p $RE4_PID -o pid,ppid,comm,args 2>/dev/null | tee -a "$LOGFILE"

        # Check DXVK/vkd3d logs
        ls -la /tmp/dxvk_re4/ 2>/dev/null | tee -a "$LOGFILE"
        ls -la /tmp/proton_re4/ 2>/dev/null | tee -a "$LOGFILE"
    fi

    # Check if Proton wine process exists
    WINE_PID=$(pgrep -f "proton.*waitforexitandrun" 2>/dev/null)
    if [ -n "$WINE_PID" ]; then
        echo "[$WAITED s] Proton running: PID=$WINE_PID" | tee -a "$LOGFILE"
    fi
done

echo "" | tee -a "$LOGFILE"
echo "=== Post-launch analysis ===" | tee -a "$LOGFILE"
echo "--- compat_log ---" | tee -a "$LOGFILE"
grep "$APPID" "$COMPAT_LOG" 2>/dev/null | tee -a "$LOGFILE"
echo "--- content_log ---" | tee -a "$LOGFILE"
grep -E "228980|$APPID" "$CONTENT_LOG" 2>/dev/null | tee -a "$LOGFILE"
echo "--- gameprocess_log ---" | tee -a "$LOGFILE"
cat "$GAMEPROCESS_LOG" 2>/dev/null | tee -a "$LOGFILE"
echo "--- DXVK logs ---" | tee -a "$LOGFILE"
ls -la /tmp/dxvk_re4/ 2>/dev/null | tee -a "$LOGFILE"
echo "--- Proton logs ---" | tee -a "$LOGFILE"
ls -la /tmp/proton_re4/ 2>/dev/null | tee -a "$LOGFILE"
echo "" | tee -a "$LOGFILE"
echo "Full log saved to: $LOGFILE"
