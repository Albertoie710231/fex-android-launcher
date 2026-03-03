#!/bin/bash
# Trace Steam IPC on local x86_64 machine.
#
# Usage:
#   ./trace_steam_local.sh setup    # Compile shims, patch sniper_wrap, clean logs
#   LD_PRELOAD="/tmp/trace_steam_ipc32.so:/tmp/trace_steam_ipc64.so" steam
#   ./trace_steam_local.sh analyze  # Analyze collected trace logs
#   ./trace_steam_local.sh restore  # Restore original sniper_wrap.sh

DIR="$(cd "$(dirname "$0")" && pwd)"
STEAM_DIR="$HOME/.steam/steam"
TRACE_SRC="$DIR/trace_steam_ipc.c"
SNIPER_WRAP="$STEAM_DIR/ubuntu12_64/steamwebhelper_sniper_wrap.sh"
SNIPER_BACKUP="$STEAM_DIR/ubuntu12_64/steamwebhelper_sniper_wrap.sh.bak_trace"

case "${1:-help}" in

setup)
    echo "=== Compiling trace shims ==="
    gcc -shared -fPIC -O2 -w -o /tmp/trace_steam_ipc64.so "$TRACE_SRC" -ldl -lrt -lpthread
    echo "  64-bit: OK"
    gcc -m32 -shared -fPIC -O2 -w -o /tmp/trace_steam_ipc32.so "$TRACE_SRC" -ldl -lrt -lpthread
    echo "  32-bit: OK"

    rm -f /tmp/steam_ipc_trace_*.log
    echo "  Cleaned old trace logs"

    if [ -f "$SNIPER_WRAP" ] && [ ! -f "$SNIPER_BACKUP" ]; then
        cp "$SNIPER_WRAP" "$SNIPER_BACKUP"
        echo "  Backed up sniper_wrap.sh"
    fi

    cat > "$SNIPER_WRAP" << 'EOF'
#!/bin/bash
export LD_LIBRARY_PATH=.${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
echo "<6>exec ./steamwebhelper $*"
echo "<remaining-lines-assume-level=7>"
export LD_PRELOAD="/tmp/trace_steam_ipc64.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec ./steamwebhelper "$@"
EOF
    chmod +x "$SNIPER_WRAP"
    echo "  Patched sniper_wrap.sh"

    echo ""
    echo "Now run:"
    echo "  LD_PRELOAD=\"/tmp/trace_steam_ipc32.so:/tmp/trace_steam_ipc64.so\" steam"
    echo ""
    echo "When Steam is up, run:"
    echo "  $0 analyze"
    ;;

restore)
    if [ -f "$SNIPER_BACKUP" ]; then
        mv "$SNIPER_BACKUP" "$SNIPER_WRAP"
        echo "Restored original sniper_wrap.sh"
    else
        echo "No backup found"
    fi
    ;;

analyze)
    echo "=== Trace logs ==="
    for f in /tmp/steam_ipc_trace_*.log; do
        [ -f "$f" ] || continue
        lines=$(wc -l < "$f")
        bits="??"
        [[ "$f" == *_32_* ]] && bits="32"
        [[ "$f" == *_64_* ]] && bits="64"
        pid=$(echo "$f" | grep -oP '\d+(?=\.log)')
        echo "  ${bits}-bit PID=$pid  ${lines} lines  $(du -h "$f" | cut -f1)"
    done

    for f in /tmp/steam_ipc_trace_*.log; do
        [ -f "$f" ] || continue
        lines=$(wc -l < "$f")
        [ "$lines" -lt 5 ] && continue
        echo ""
        echo "--- $(basename "$f") ($lines lines) ---"
        echo "  shm_open:"
        grep "shm_open" "$f" 2>/dev/null | head -10 || true
        echo "  ftruncate:"
        grep "FTRUNCATE" "$f" 2>/dev/null | head -10 || true
        echo "  mmap (shm):"
        grep " MMAP " "$f" 2>/dev/null | head -10 || true
        echo "  MMAP headers:"
        grep "MMAP_HDR\|MMAP64_HDR" "$f" 2>/dev/null | head -20 || true
        echo "  PERIODIC headers:"
        grep "PERIODIC_HDR" "$f" 2>/dev/null | head -30 || true
        echo "  MUNMAP headers:"
        grep "MUNMAP_HDR" "$f" 2>/dev/null | head -10 || true
        echo "  MSYNC:"
        grep "MSYNC" "$f" 2>/dev/null | head -10 || true
        echo "  PREAD/PWRITE:"
        grep "PREAD\|PWRITE" "$f" 2>/dev/null | head -20 || true
        echo "  SCM_RIGHTS:"
        grep "SCM_RIGHTS" "$f" 2>/dev/null | head -10 || true
        echo "  sdPC:"
        grep "sdPC" "$f" 2>/dev/null | head -5 || true
        echo "  1-byte writes:"
        grep "WRITE 1-byte" "$f" 2>/dev/null | head -10 || true
        echo "  1-byte reads:"
        grep "READ 1-byte" "$f" 2>/dev/null | head -10 || true
        echo "  shmem socket:"
        grep "steam_chrome_shmem\|SteamChrome" "$f" 2>/dev/null | head -5 || true
        echo "  socketpairs:"
        grep "socketpair" "$f" 2>/dev/null | head -5 || true
    done

    echo ""
    echo "=== Non-zero Shm_ headers ==="
    grep "PERIODIC_HDR\[/u1000-Shm_" /tmp/steam_ipc_trace_*.log 2>/dev/null \
        | grep -v "hdr\[0\]=0(cubHeader) hdr\[1\]=0(eState) hdr\[2\]=0(cubRingBuf) hdr\[3\]=0(cubData)" \
        | head -40 || echo "  (none)"

    echo ""
    echo "=== Timeline (first 150 events) ==="
    cat /tmp/steam_ipc_trace_*.log 2>/dev/null \
        | grep -v "socket fd=\|  raw:\|  00 00 " \
        | sort -t']' -k1 \
        | head -150
    ;;

*)
    echo "Usage:"
    echo "  $0 setup    # Compile, patch, clean logs"
    echo "  $0 analyze  # Analyze trace logs"
    echo "  $0 restore  # Restore original sniper_wrap.sh"
    ;;
esac
