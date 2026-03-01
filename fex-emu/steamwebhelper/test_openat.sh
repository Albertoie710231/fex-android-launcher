#!/bin/bash
# Test openat in 3 scenarios to isolate where FEX breaks

DIR="$(dirname "$0")"
cd "${DIR}"
DIR="$(pwd)"

echo "=== TEST 1: Direct from bash (same CWD as steamwebhelper) ==="
"${DIR}/test_openat" 2>&1

echo ""
echo "=== TEST 2: With same env vars as steamwebhelper ==="
SNIPER_LIBS="/home/user/.steam/steam/steamrt64/pv-runtime/steam-runtime-steamrt/steamrt3c_platform_3c.0.20251202.187499/files/lib/x86_64-linux-gnu"
export LD_LIBRARY_PATH=".:${DIR}:${SNIPER_LIBS}:${SNIPER_LIBS}/nss${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DISPLAY=":0"
export TMPDIR=/tmp
export DBUS_SESSION_BUS_ADDRESS=disabled:
"${DIR}/test_openat" 2>&1

echo ""
echo "=== TEST 3: With patchelf'd steamwebhelper's DT_NEEDED shim loaded ==="
# This uses the shim .so via LD_PRELOAD (may fail) but tests the env
LD_PRELOAD="${DIR}/libfix_steamwebhelper.so" "${DIR}/test_openat" 2>&1

echo ""
echo "=== ALL TESTS DONE ==="
