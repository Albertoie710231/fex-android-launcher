#!/bin/bash
# Bootstrap full Steam client in Docker and create tar with symlinks preserved
# Output: /tmp/steam_symlinks.tar

set -e

echo "=== Bootstrapping Steam client in Docker ==="

docker run --rm -v /tmp:/output ubuntu:22.04 bash -c '
set -e
dpkg --add-architecture i386
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq steam-installer xvfb 2>/dev/null | tail -3

export HOME=/root DISPLAY=:99
Xvfb :99 -screen 0 1024x768x24 &
sleep 2

echo "Starting Steam bootstrap..."
/usr/games/steam -no-browser -no-cef-sandbox &
STEAM_PID=$!

for i in $(seq 1 180); do
    sleep 10
    if [ -f /root/.steam/debian-installation/ubuntu12_32/steamui.so ]; then
        echo "Bootstrap complete at ${i}0s"
        break
    fi
    du -sh /root/.steam/ 2>/dev/null || echo "Waiting for download..."
    ls /root/.steam/debian-installation/ubuntu12_32/steamui.so 2>/dev/null && break
done

kill $STEAM_PID 2>/dev/null; sleep 3

echo "=== Structure ==="
ls -la /root/.steam/

echo "=== Creating tar (with symlinks) ==="
cd /root && tar cf /output/steam_symlinks.tar .steam/
ls -lh /output/steam_symlinks.tar
echo "FINISHED"
'

echo ""
echo "Done! Output: /tmp/steam_symlinks.tar"
echo "Next: run ./scripts/push_steam.sh to deploy to device"
