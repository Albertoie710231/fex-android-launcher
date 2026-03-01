#!/bin/bash
# PATCHED v50: exec (preserve PID for IPC) + minimal flags

set -u

DIR="$(dirname "$0")"
cd "${DIR}"
DIR="$(pwd)"

BASENAME="$(basename "$0")"
log () { ( echo "${BASENAME}[$$]: $*" >&2 ) || :; }

SNIPER_LIBS="/home/user/.steam/steam/steamrt64/pv-runtime/steam-runtime-steamrt/steamrt3c_platform_3c.0.20251202.187499/files/lib/x86_64-linux-gnu"
export LD_LIBRARY_PATH=".:${DIR}:${SNIPER_LIBS}:${SNIPER_LIBS}/nss${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Real Android paths — short path for Unix sockets (108 byte limit), long path for data
REAL_TMPDIR="/data/data/com.mediatek.steamlauncher/cache/s"
REAL_DATADIR="/data/user/0/com.mediatek.steamlauncher/cache/tmp/htmlcache"

export DISPLAY="${DISPLAY:-:0}"
export FONTCONFIG_PATH=/etc/fonts
export TMPDIR="${REAL_TMPDIR}"
export XDG_RUNTIME_DIR="${REAL_TMPDIR}"
export DBUS_SESSION_BUS_ADDRESS=disabled:
export DBUS_SYSTEM_BUS_ADDRESS=disabled:

ulimit -v unlimited 2>/dev/null
ulimit -n 65536 2>/dev/null

mkdir -p "${REAL_DATADIR}" 2>/dev/null
mkdir -p "${REAL_TMPDIR}" 2>/dev/null
rm -f "${REAL_TMPDIR}/SingletonLock" "${REAL_TMPDIR}/SingletonSocket" "${REAL_TMPDIR}/SingletonCookie" 2>/dev/null
rm -rf "${REAL_TMPDIR}/.com.valvesoftware.Steam."* 2>/dev/null
rm -rf /tmp/.com.valvesoftware.Steam.* 2>/dev/null
mkdir -p /tmp 2>/dev/null
chmod 1777 /tmp 2>/dev/null

# Copy ICU data + resource paks to real Android path so Chromium can mmap them
# FEX overlay has a bug where mmap on files accessed through symlinks fails
log "Copying ICU data and paks to ${REAL_DATADIR}..."
cp -f "${DIR}/icudtl.dat" "${REAL_DATADIR}/icudtl.dat" 2>/dev/null
cp -f "${DIR}/chrome_100_percent.pak" "${REAL_DATADIR}/" 2>/dev/null
cp -f "${DIR}/chrome_200_percent.pak" "${REAL_DATADIR}/" 2>/dev/null
cp -f "${DIR}/resources.pak" "${REAL_DATADIR}/" 2>/dev/null
cp -f "${DIR}/headless_command_resources.pak" "${REAL_DATADIR}/" 2>/dev/null
cp -rf "${DIR}/locales" "${REAL_DATADIR}/" 2>/dev/null
log "Copy done. icudtl.dat size: $(stat -c%s "${REAL_DATADIR}/icudtl.dat" 2>/dev/null || echo MISSING)"

export ICU_DATA="${REAL_DATADIR}"

# Process args: fix cachedir path, disable NotReachedIsFatal
FOUND_DISABLE_FEATURES=0
ARGS=()
for arg in "$@"; do
    case "$arg" in
        -cachedir=*) ARGS+=("-cachedir=${REAL_DATADIR}") ;;
        --disable-features=*) ARGS+=("${arg},NotReachedIsFatal"); FOUND_DISABLE_FEATURES=1 ;;
        *) ARGS+=("$arg") ;;
    esac
done
# Add NotReachedIsFatal even if Steam didn't pass --disable-features
if [ "$FOUND_DISABLE_FEATURES" -eq 0 ]; then
    ARGS+=("--disable-features=NotReachedIsFatal")
fi

log "PATCHED v50: exec for IPC, minimal flags"

# Use exec to REPLACE this script process with steamwebhelper.
# This preserves the PID so Steam's IPC pipes/sockets reach the binary directly.
# Original Steam script also uses exec chain: .sh → exec entry_point → exec steamwebhelper
exec "${DIR}/steamwebhelper" \
    --user-data-dir="${REAL_DATADIR}" \
    --browser-subprocess-path="${DIR}/steamwebhelper" \
    --no-sandbox \
    --disable-gpu \
    --disable-gpu-sandbox \
    --disable-gpu-compositing \
    --disable-setuid-sandbox \
    --disable-seccomp-filter-sandbox \
    --disable-dev-shm-usage \
    --disable-breakpad \
    --disable-crash-reporter \
    --disable-crashpad-for-testing \
    --no-first-run \
    --disable-field-trial-config \
    --icu-data-dir="${REAL_DATADIR}" \
    "${ARGS[@]}"
