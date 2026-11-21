#!/bin/sh
# Collect kernel/driver logs after a run or a suspected hang.
# Needs root (sudo) for dmesg/journal.
set -u

OUT=${OUT:-/home/user/a5xx-logs-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

{
   uname -a
   uptime
   echo "--- vulkan"
   env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES vulkaninfo --summary 2>&1
   echo "--- sessions"
   loginctl list-sessions --no-legend 2>&1
   echo "--- gpu"
   cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null
   for z in /sys/class/thermal/thermal_zone*/temp; do
      printf '%s=%s\n' "$z" "$(cat "$z" 2>/dev/null)"
   done
} > "$OUT/status.txt" 2>&1

if command -v sudo >/dev/null 2>&1; then
   sudo dmesg > "$OUT/dmesg-current.txt" 2>&1 || true
   sudo journalctl -k -b 0 --no-pager > "$OUT/journal-current.txt" 2>&1 || true
   sudo journalctl -k -b -1 --no-pager > "$OUT/journal-prev.txt" 2>&1 || true
   sudo ls -l /sys/fs/pstore > "$OUT/pstore-list.txt" 2>&1 || true
   mkdir -p "$OUT/pstore"
   sudo cp -a /sys/fs/pstore/. "$OUT/pstore/" 2>/dev/null || true
fi

echo "collected to $OUT"
