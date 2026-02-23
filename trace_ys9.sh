#!/bin/bash
# Capture DXVK memory trace from Ys IX via Proton
# Compare with Android/FEX-Emu Mali behavior
# Usage: ./trace_ys9.sh
# Close the game window (or Ctrl+C) when done.

LOG="/tmp/ys9_proton_trace.log"
SUMMARY="/tmp/ys9_memory_summary.txt"

echo "Starting Ys IX with DXVK trace logging..."
echo "Trace: $LOG"
echo "Summary: $SUMMARY"
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
echo ""
echo "=== Extracting memory summary ==="

{
echo "============================================"
echo "  Ys IX DXVK Memory Summary (Desktop)"
echo "  $(date)"
echo "============================================"
echo ""

echo "--- Memory Types & Heaps (DXVK view) ---"
grep -i "memory type\|heap\|Memory Type Mask\|memoryType\|Heap " "$LOG" | head -60
echo ""

echo "--- Memory Allocator / Chunk Info ---"
grep -i "chunk\|DxvkMemoryAllocator\|memory pool\|max chunk" "$LOG" | head -40
echo ""

echo "--- All AllocateMemory (type + size) ---"
grep -i "allocatememory\|DxvkMemory\|Allocated\|alloc.*memory" "$LOG" | head -80
echo ""

echo "--- MapMemory calls ---"
grep -i "mapmemory\|mapped\|mapping" "$LOG" | head -40
echo ""

echo "--- HOST_VISIBLE vs DEVICE_LOCAL decisions ---"
grep -i "host.visible\|device.local\|memory type.*flag\|unified\|isUMA" "$LOG" | head -30
echo ""

echo "--- DXVK Config / maxChunkSize ---"
grep -i "maxchunk\|config\|dxvk\.conf\|chunk.size" "$LOG" | head -20
echo ""

echo "--- Errors / Failures ---"
grep -i "error\|fail\|DEVICE_LOST\|out.of.*memory\|crash" "$LOG" | head -30
echo ""
} > "$SUMMARY"

cat "$SUMMARY"
echo ""
echo "Full trace: $LOG"
echo "Summary: $SUMMARY"
