#!/usr/bin/env bash
# Dump /proc/<sekiro-pid>/smaps via adb exec-out + run-as (same UID as
# sekiro, can read its smaps). Drives the loop host-side so stdout
# redirection works.
#
# Usage: scripts/trace_smaps.sh [interval_sec] [max_samples]
# Output: /tmp/smaps/smaps_NN_<pid>.txt on HOST (not tablet)

INTERVAL=${1:-30}
MAX=${2:-6}
OUTDIR=${OUTDIR:-/tmp/smaps}
mkdir -p "$OUTDIR"
rm -f "$OUTDIR"/smaps_*.txt

echo "smaps tracer: every ${INTERVAL}s up to $MAX samples -> $OUTDIR"

n=0
while [ "$n" -lt "$MAX" ]; do
  pid=$(adb shell "
    for p in /proc/[0-9]*/comm; do
      c=\$(cat \$p 2>/dev/null)
      if [ \"\$c\" = sekiro.exe ]; then
        d=\${p%/comm}
        echo \${d##*/}
        break
      fi
    done
  " 2>/dev/null | tr -d '\r')
  if [ -z "$pid" ] || [ "$pid" = "0" ]; then
    echo "$(date +%H:%M:%S) no sekiro — waiting ${INTERVAL}s"
    sleep "$INTERVAL"
    continue
  fi
  t=$(date +%s)
  out="$OUTDIR/smaps_$(printf '%02d' $n)_${pid}.txt"
  adb exec-out "run-as com.mediatek.steamlauncher cat /proc/$pid/smaps" > "$out" 2>/dev/null
  lines=$(wc -l < "$out")
  bytes=$(wc -c < "$out")
  echo "$(date +%H:%M:%S) pid=$pid t=$t -> $(basename "$out") ($lines lines, $bytes bytes)"
  n=$((n+1))
  sleep "$INTERVAL"
done

echo "Done. Samples in $OUTDIR"
