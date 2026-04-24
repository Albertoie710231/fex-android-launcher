#!/usr/bin/env bash
# Strip SteamStub DRM from a Steam game .exe using bundled Steamless, on the PC.
#
# Steam games wrapped with SteamStub won't boot under the launcher's
# ColdClient setup — the wrapper's auth check refuses to decrypt and
# exits with "Application load error 3:0000065432". GameNative's
# workaround is to unpack the wrapper once at game-import time, producing
# a DRM-free <name>.unpacked.exe.
#
# Running Steamless.CLI.exe (a .NET 4.x Windows binary) directly on the
# tablet has been flaky (wine-mono under ARM64EC exits 53 with no
# output). But running it on the PC under Proton works in ~30s — and
# NativeWinePipeline.unpackWithSteamless short-circuits to the pre-staged
# .unpacked.exe if it's fresher than the source, so dev-side unpacking
# is all we need.
#
# Usage:
#   scripts/unpack_game_exe.sh <path-to-game.exe>
#
# Output:
#   <path-to-game.exe>.unpacked.exe  (placed next to input)

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <path-to-game.exe>" >&2
    exit 2
fi

GAME_EXE_ABS="$(readlink -f "$1")"
if [[ ! -f "$GAME_EXE_ABS" ]]; then
    echo "error: input not a file: $GAME_EXE_ABS" >&2
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STEAMLESS_ASSETS="$REPO_ROOT/app/src/main/assets/productize/Steamless"
if [[ ! -f "$STEAMLESS_ASSETS/Steamless.CLI.exe" ]]; then
    echo "error: bundled Steamless missing at $STEAMLESS_ASSETS" >&2
    exit 2
fi

# Pick a Proton install. 8.0 is the verified-working one (2026-04-23).
STEAMROOT="${STEAMROOT:-$HOME/.steam/steam}"
PROTON_DIR=""
for candidate in "Proton 8.0" "Proton 9.0 (Beta)" "Proton Hotfix" "Proton - Experimental"; do
    if [[ -x "$STEAMROOT/steamapps/common/$candidate/proton" ]]; then
        PROTON_DIR="$STEAMROOT/steamapps/common/$candidate"
        break
    fi
done
if [[ -z "$PROTON_DIR" ]]; then
    echo "error: no Proton install found under $STEAMROOT/steamapps/common/" >&2
    exit 2
fi

# Use a dedicated persistent prefix. Scanning existing compatdata
# prefixes is tempting (they're pre-initialized) but dangerous: if the
# prefix was initialized by a different Proton version (e.g.
# GE-Proton10-32), running our Proton 8.0 against it triggers a
# destructive downgrade and prints "Prefix has an invalid version?!".
# A dedicated cache dir is safe; the first run pays a 1-3 min init cost
# (wine-mono install + wineboot), subsequent runs are immediate.
CACHE_PREFIX="${STEAMLESS_PREFIX_DIR:-$HOME/.cache/fex-android-launcher/steamless-prefix}"
COMPAT_PREFIX="$CACHE_PREFIX"
if [[ ! -d "$CACHE_PREFIX/pfx" ]]; then
    mkdir -p "$CACHE_PREFIX"
    echo "[info] first run: initializing Proton prefix at $CACHE_PREFIX (1-3 min)..." >&2
fi

# Stage input + Steamless in a tmpdir. Input is copied (not symlinked)
# so Steamless can write .unpacked.exe next to it without touching the
# real install.
WORKDIR="$(mktemp -d -t steamless-work.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

cp "$GAME_EXE_ABS" "$WORKDIR/"
cp -r "$STEAMLESS_ASSETS"/* "$WORKDIR/"
GAME_EXE_BASENAME="$(basename "$GAME_EXE_ABS")"

echo "[info] Proton:     $PROTON_DIR"
echo "[info] prefix:     $COMPAT_PREFIX"
echo "[info] input:      $GAME_EXE_ABS"
echo "[info] staged:     $WORKDIR/$GAME_EXE_BASENAME"

WINE_PATH="Z:$(echo "$WORKDIR/$GAME_EXE_BASENAME" | tr / \\\\)"
echo "[info] wine path:  $WINE_PATH"

STEAM_COMPAT_DATA_PATH="$COMPAT_PREFIX" \
STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAMROOT" \
    "$PROTON_DIR/proton" run "$WORKDIR/Steamless.CLI.exe" "$WINE_PATH" \
    2>&1 | grep -vE '^wine: using kernel write watches|^fsync: up and running' || true

UNPACKED_IN_WORK="$WORKDIR/${GAME_EXE_BASENAME}.unpacked"
# Steamless v3.x emits <name>.unpacked (no .exe extension), older
# variants use .unpacked.exe. Handle both.
if [[ -f "$WORKDIR/${GAME_EXE_BASENAME}.unpacked.exe" ]]; then
    UNPACKED_IN_WORK="$WORKDIR/${GAME_EXE_BASENAME}.unpacked.exe"
fi
if [[ ! -f "$UNPACKED_IN_WORK" ]]; then
    echo "error: Steamless produced no .unpacked output in $WORKDIR" >&2
    ls -la "$WORKDIR" >&2
    exit 1
fi

# Launcher (NativeWinePipeline.unpackWithSteamless) looks for the file
# at <original>.unpacked.exe on-tablet, so normalize the name here too.
FINAL_OUT="$GAME_EXE_ABS.unpacked.exe"
cp "$UNPACKED_IN_WORK" "$FINAL_OUT"

IN_BYTES=$(stat -c %s "$GAME_EXE_ABS")
OUT_BYTES=$(stat -c %s "$FINAL_OUT")
echo ""
echo "[ok] wrote: $FINAL_OUT"
echo "[ok] size:  $IN_BYTES -> $OUT_BYTES bytes ($(( IN_BYTES - OUT_BYTES )) saved)"
echo ""
echo "Next: push to tablet + swap in as the guest sekiro.exe."
echo "  adb push \"$FINAL_OUT\" /data/local/tmp/"
echo "  adb shell 'run-as com.mediatek.steamlauncher sh -c \""
echo "    GAME_DIR=\\\"files/proton10/prefix/.wine/drive_c/Program Files (x86)/Steam/steamapps/common/<Game>\\\";"
echo "    cd \\\"\$GAME_DIR\\\";"
echo "    [ -f $(basename "$GAME_EXE_ABS").steamstub ] || cp $(basename "$GAME_EXE_ABS") $(basename "$GAME_EXE_ABS").steamstub;"
echo "    cp /data/local/tmp/$(basename "$FINAL_OUT") $(basename "$FINAL_OUT");"
echo "    cp $(basename "$FINAL_OUT") $(basename "$GAME_EXE_ABS")"
echo "  \"'"
