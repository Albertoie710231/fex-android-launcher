#!/bin/bash
# PATCHED v19: SIGTRAP skip + RLIMIT_AS + crash recovery

set -u

DIR="$(dirname "$0")"
cd "${DIR}"
DIR="$(pwd)"

BASENAME="$(basename "$0")"
log () {
    ( echo "${BASENAME}[$$]: $*" >&2 ) || :
}

SNIPER_LIBS="/home/user/.steam/steam/steamrt64/pv-runtime/steam-runtime-steamrt/steamrt3c_platform_3c.0.20251202.187499/files/lib/x86_64-linux-gnu"
export LD_LIBRARY_PATH=".:${DIR}:${SNIPER_LIBS}:${SNIPER_LIBS}/nss${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

export DISPLAY="${DISPLAY:-:0}"
export FONTCONFIG_PATH=/etc/fonts

# Raise limits for PartitionAlloc (needs 16-32 GiB VA space)
ulimit -v unlimited 2>/dev/null
ulimit -n 65536 2>/dev/null

log "FEX-patched v19: SIGTRAP skip + RLIMIT_AS unlimited"

"${DIR}/steamwebhelper" \
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
    --no-zygote \
    "$@" 2>/home/user/steamwebhelper_stderr.log

EXIT=$?
log "Exit code $EXIT"
