#!/usr/bin/env bash
# v2 memory tracer. Scans /proc/<pid>/comm for sekiro.exe + wineserver
# exactly, reads smaps_rollup for PSS/Anon/Swap, and sums PSS of every
# wine-related process (PE .exe binaries) each poll. Tracks system meminfo.
#
# Usage: scripts/trace_mem_v2.sh [interval_sec]  # default 5s
# Output: /sdcard/mem_v2.csv on tablet

INTERVAL=${1:-5}
CSV=/sdcard/mem_v2.csv

echo "Tracer interval=${INTERVAL}s -> $CSV"

adb shell "rm -f $CSV; echo 't,sekiro_pss,sekiro_anon,sekiro_swap,sekiro_vmRSS,sekiro_rssFile,sekiro_rssShm,wineserver_pss,launcher_pss,all_wine_pss,sys_MemFree,sys_MemAvail,sys_SwapFree,sys_Active,sys_Inactive,sys_Slab,sys_PageTables,sys_Buffers,sys_Cached' > $CSV"

adb shell "
pss_of() {
  awk '/^Pss:/{p+=\$2} END{print p+0}' /proc/\$1/smaps_rollup 2>/dev/null
}
anon_of() {
  awk '/^Anonymous:/{a+=\$2} END{print a+0}' /proc/\$1/smaps_rollup 2>/dev/null
}
swap_of() {
  awk '/^Swap:/{s+=\$2} END{print s+0}' /proc/\$1/smaps_rollup 2>/dev/null
}

while true; do
  t=\$(date +%s)

  spid=0; wspid=0
  for p in /proc/[0-9]*/comm; do
    c=\$(cat \$p 2>/dev/null)
    pid_dir=\${p%/comm}
    pid=\${pid_dir##*/}
    case \"\$c\" in
      sekiro.exe)   spid=\$pid ;;
      wineserver)   wspid=\$pid ;;
    esac
  done

  lpid=\$(pidof com.mediatek.steamlauncher 2>/dev/null || echo 0)

  if [ \"\$spid\" != \"0\" ]; then
    s_pss=\$(pss_of \$spid)
    s_anon=\$(anon_of \$spid)
    s_swap=\$(swap_of \$spid)
    s_rss=\$(awk '/^VmRSS:/{print \$2}' /proc/\$spid/status 2>/dev/null)
    s_rssfile=\$(awk '/^RssFile:/{print \$2}' /proc/\$spid/status 2>/dev/null)
    s_rssshm=\$(awk '/^RssShmem:/{print \$2}' /proc/\$spid/status 2>/dev/null)
  else
    s_pss=0; s_anon=0; s_swap=0; s_rss=0; s_rssfile=0; s_rssshm=0
  fi

  ws_pss=0
  [ \"\$wspid\" != \"0\" ] && ws_pss=\$(pss_of \$wspid)

  l_pss=0
  [ \"\$lpid\" != \"0\" ] && l_pss=\$(pss_of \$lpid)

  # Sum PSS of all .exe wine PE processes
  all_wine_pss=0
  for p in /proc/[0-9]*/comm; do
    c=\$(cat \$p 2>/dev/null)
    case \"\$c\" in
      *.exe|wineserver|winedevice.exe|services.exe|explorer.exe|start.exe)
        pid_dir=\${p%/comm}
        pid=\${pid_dir##*/}
        pss=\$(pss_of \$pid)
        all_wine_pss=\$((all_wine_pss + pss))
        ;;
    esac
  done

  eval \$(awk '/^MemFree|^MemAvailable|^SwapFree|^Active:|^Inactive:|^Slab:|^PageTables|^Buffers:|^Cached:/{
    gsub(\":\",\"\",\$1); printf \"sys_%s=%s \", \$1, \$2
  }' /proc/meminfo)

  echo \"\$t,\$s_pss,\$s_anon,\$s_swap,\$s_rss,\$s_rssfile,\$s_rssshm,\$ws_pss,\$l_pss,\$all_wine_pss,\$sys_MemFree,\$sys_MemAvailable,\$sys_SwapFree,\$sys_Active,\$sys_Inactive,\$sys_Slab,\$sys_PageTables,\$sys_Buffers,\$sys_Cached\" >> $CSV

  sleep $INTERVAL
done
"
