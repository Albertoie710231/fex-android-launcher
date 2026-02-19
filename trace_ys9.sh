#!/bin/bash
# Capture DXVK trace from Ys IX via Proton for comparison with Android
# Usage: ./trace_ys9.sh
# Close the game window (or Ctrl+C) when done. Output goes to /tmp/ys9_proton_trace.log

LOG="/tmp/ys9_proton_trace.log"

echo "Starting Ys IX with DXVK trace logging..."
echo "Output: $LOG"
echo "Close the game or press Ctrl+C when done."
echo ""

STEAM_COMPAT_DATA_PATH="$HOME/.local/share/Steam/steamapps/compatdata/1351630" \
STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
DXVK_LOG_LEVEL=trace \
WINEDEBUG=err+all \
"$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton" run \
"$HOME/.local/share/Steam/steamapps/common/Ys IX Monstrum Nox/ys9.exe" \
> "$LOG" 2>&1

echo ""
echo "Done. Trace saved to $LOG ($(wc -l < "$LOG") lines)"
