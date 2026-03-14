#!/bin/bash
# Test "steam steam://rungameid/814380" on PC to see if Steam launches Sekiro
# This mirrors what we do on the Android device via getSteamCommand()

STEAMDIR="$HOME/.steam/steam"

export DISPLAY=:0
export DBUS_SESSION_BUS_ADDRESS=disabled

# Same env vars as getSteamCommand on device
export DXVK_ASYNC=1
export DXVK_STATE_CACHE=1
export DXVK_LOG_LEVEL=info
export PROTON_NO_FSYNC=1
export PROTON_USE_WINED3D=0
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAMDIR"
export STEAM_COMPAT_DATA_PATH="$STEAMDIR/steamapps/compatdata/814380"

export LD_LIBRARY_PATH="$STEAMDIR/linux64:$STEAMDIR/ubuntu12_64:$STEAMDIR/ubuntu12_32:$STEAMDIR/ubuntu12_32/panorama:${LD_LIBRARY_PATH:-}"
export STEAMSCRIPT="$STEAMDIR/steam.sh"

echo "=== Test: steam steam://rungameid/814380 ==="
echo "STEAMDIR=$STEAMDIR"
echo "STEAM_COMPAT_DATA_PATH=$STEAM_COMPAT_DATA_PATH"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo ""

# Check appmanifest
if [ -f "$STEAMDIR/steamapps/appmanifest_814380.acf" ]; then
    echo "appmanifest_814380.acf EXISTS:"
    cat "$STEAMDIR/steamapps/appmanifest_814380.acf"
else
    echo "WARNING: appmanifest_814380.acf NOT FOUND"
fi
echo ""

# Check game dir
GAMEDIR="$STEAMDIR/steamapps/common/Sekiro"
if [ -d "$GAMEDIR" ]; then
    echo "Game dir EXISTS: $GAMEDIR"
    ls -la "$GAMEDIR/" | head -10
else
    echo "WARNING: Game dir NOT FOUND at $GAMEDIR"
    # Also check alternative name
    ALT="$STEAMDIR/steamapps/common/Sekiro Shadows Die Twice"
    if [ -d "$ALT" ]; then
        echo "Found at alternative path: $ALT"
        ls -la "$ALT/" | head -10
    fi
fi
echo ""

# Check libraryfolders.vdf
echo "libraryfolders.vdf:"
cat "$STEAMDIR/steamapps/libraryfolders.vdf" 2>/dev/null | head -30
echo ""

echo "=== Running: bash steam.sh steam://rungameid/814380 ==="
cd "$STEAMDIR"
bash steam.sh steam://rungameid/814380 2>&1 | tee /tmp/steam_rungameid_test.log
