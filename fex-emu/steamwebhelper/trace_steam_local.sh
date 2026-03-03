#!/bin/bash
# Trace Steam IPC on local x86_64 machine.
# Captures the real handshake protocol between steam (32-bit) and steamwebhelper (64-bit).
#
# Usage: ./trace_steam_local.sh
#
# Output: /tmp/steam_ipc_trace_*.log files
# After Steam starts, wait ~10s then check logs with: ./trace_steam_local.sh --analyze

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
STEAM_DIR="$HOME/.steam/steam"
TRACE_SRC="$DIR/trace_steam_ipc.c"

# Compile both 32-bit and 64-bit trace shims
echo "=== Compiling trace shims ==="
gcc -shared -fPIC -O2 -o /tmp/trace_steam_ipc64.so "$TRACE_SRC" -ldl 2>&1
echo "  64-bit: OK"
gcc -m32 -shared -fPIC -O2 -o /tmp/trace_steam_ipc32.so "$TRACE_SRC" -ldl 2>&1
echo "  32-bit: OK"

if [ "$1" = "--analyze" ]; then
    echo ""
    echo "=== Analyzing trace logs ==="
    echo ""

    for f in /tmp/steam_ipc_trace_*.log; do
        [ -f "$f" ] || continue
        echo "--- $f ---"
        echo "  Lines: $(wc -l < "$f")"

        # Key events
        echo "  sdPC writes:"
        grep "WRITE sdPC" "$f" 2>/dev/null | head -5
        echo "  sdPC reads:"
        grep "READ sdPC" "$f" 2>/dev/null | head -5
        echo "  1-byte writes:"
        grep "WRITE 1-byte" "$f" 2>/dev/null | head -5
        echo "  1-byte reads:"
        grep "READ 1-byte" "$f" 2>/dev/null | head -5
        echo "  socketpairs:"
        grep "socketpair" "$f" 2>/dev/null | head -5
        echo "  binds (shmem):"
        grep "bind.*shmem\|bind.*steam\|bind.*chrome" "$f" 2>/dev/null | head -5
        echo "  connects (shmem):"
        grep "connect.*shmem\|connect.*steam\|connect.*chrome" "$f" 2>/dev/null | head -5
        echo "  accepts:"
        grep "accept" "$f" 2>/dev/null | head -5
        echo "  shm_open:"
        grep "shm_open" "$f" 2>/dev/null | head -10
        echo "  listens:"
        grep "listen" "$f" 2>/dev/null | head -5
        echo ""
    done

    echo "=== Timeline (all logs merged, sorted by time) ==="
    cat /tmp/steam_ipc_trace_*.log 2>/dev/null | sort -t']' -k1 | grep -v "socket fd=" | head -80
    exit 0
fi

# Clean old traces
rm -f /tmp/steam_ipc_trace_*.log

echo ""
echo "=== Patching steamwebhelper.sh to inject trace shim ==="

# Create a wrapper for steamwebhelper that adds our LD_PRELOAD
WRAPPER="$STEAM_DIR/ubuntu12_64/steamwebhelper_trace_wrapper.sh"
REAL_WH="$STEAM_DIR/ubuntu12_64/steamwebhelper"

cat > "$WRAPPER" << 'WRAPPER_EOF'
#!/bin/bash
# Trace wrapper — adds LD_PRELOAD for IPC tracing
export LD_PRELOAD="/tmp/trace_steam_ipc64.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$(dirname "$0")/steamwebhelper" "$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

echo "  Created: $WRAPPER"

echo ""
echo "=== Starting Steam with trace shims ==="
echo "  32-bit shim: /tmp/trace_steam_ipc32.so"
echo "  64-bit shim: /tmp/trace_steam_ipc64.so (via wrapper)"
echo ""
echo "  Trace logs will be at: /tmp/steam_ipc_trace_*.log"
echo "  After Steam starts and shows login, run:"
echo "    $0 --analyze"
echo ""

# Launch Steam with 32-bit trace shim and point browser-subprocess to our wrapper
# The 32-bit LD_PRELOAD traces the steam client's side
# The wrapper script adds the 64-bit LD_PRELOAD for steamwebhelper
export LD_PRELOAD="/tmp/trace_steam_ipc32.so"

# Steam will launch steamwebhelper — we need it to use our wrapper
# We can do this by temporarily replacing the steamwebhelper.sh or by using
# STEAM_BROWSER_SUBPROCESS_PATH

# Method: temporarily rename and swap
ORIG_SH="$STEAM_DIR/ubuntu12_64/steamwebhelper.sh"
BACKUP_SH="$STEAM_DIR/ubuntu12_64/steamwebhelper.sh.bak_trace"

if [ -f "$ORIG_SH" ] && [ ! -f "$BACKUP_SH" ]; then
    cp "$ORIG_SH" "$BACKUP_SH"
fi

# Create a minimal tracing steamwebhelper.sh that loads our shim
cat > "$ORIG_SH" << 'SH_EOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_PRELOAD="/tmp/trace_steam_ipc64.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$DIR/steamwebhelper" "$@"
SH_EOF
chmod +x "$ORIG_SH"

echo "  Replaced steamwebhelper.sh with tracing version"
echo "  Original backed up to: $BACKUP_SH"
echo ""

# Cleanup trap — restore original steamwebhelper.sh on exit
cleanup() {
    echo ""
    echo "=== Restoring original steamwebhelper.sh ==="
    if [ -f "$BACKUP_SH" ]; then
        mv "$BACKUP_SH" "$ORIG_SH"
        echo "  Restored."
    fi
    rm -f "$WRAPPER"
    echo "  Cleaned up wrapper."
    echo ""
    echo "  Run '$0 --analyze' to see the trace results."
}
trap cleanup EXIT

# Run Steam
echo "Starting Steam..."
steam "$@" 2>&1 | tee /tmp/steam_trace_stdout.log
