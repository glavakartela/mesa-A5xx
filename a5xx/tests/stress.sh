#!/bin/sh
# Stress loop for the a5xx Turnip driver.
# Run on the device from /tmp/tu5-test:
#   MODE=safe DURATION=1200 ./stress.sh
#   MODE=full VKCUBE=1 ./stress.sh
# Logs every step to disk and syncs, so after a hardlock the last line tells
# which test triggered it.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

MODE=${MODE:-safe}
DURATION=${DURATION:-1200}
VKCUBE=${VKCUBE:-0}
LOG=${LOG:-$HOME/stress-$(date +%H%M%S).log}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1001}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
unset VK_ICD_FILENAMES VK_DRIVER_FILES TU5_GFX_NODEPTH

mark() {
   echo "$(date +%H:%M:%S) $*" >> "$LOG"
   sync
}

: > "$LOG"
mark "start mode=$MODE duration=$DURATION vkcube=$VKCUBE"
mark "kernel=$(uname -r)"
mark "gpuclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null)"

SAFE="tu5_test tu5_tri_test tu5_bigclear_test tu5_ubo_gfx_test tu5_wsi_test"
FULL="$SAFE tu5_texcopy_test"

case "$MODE" in
   safe) TESTS=$SAFE ;;
   full) TESTS=$FULL ;;
   *) echo "MODE must be safe or full" >&2; exit 2 ;;
esac

i=0
end=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -lt "$end" ]; do
   i=$((i + 1))
   mark "iter $i start"
   for t in $TESTS; do
      mark "BEGIN $t"
      timeout 30 "./$t" >/tmp/o 2>&1
      r=$?
      mark "END $t rc=$r $(tail -1 /tmp/o)"
      if [ "$r" -ne 0 ]; then
         mark "STOP $t rc=$r"
         exit 1
      fi
   done

   if [ "$VKCUBE" = 1 ]; then
      mark "BEGIN vkcube60"
      timeout 70 vkcube --wsi wayland --c 60 --width 128 --height 128 >/tmp/o 2>&1
      r=$?
      mark "END vkcube60 rc=$r"
      if [ "$r" -ne 0 ]; then
         mark "STOP vkcube rc=$r"
         exit 1
      fi
   fi

   mark "iter $i done gpuclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
done
mark "DONE iterations=$i"
