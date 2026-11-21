#!/bin/sh
# Simple a5xx performance smoke benchmark.
# Run on the device from /tmp/tu5-test.
set -u

cd "$(dirname "$0")"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1001}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
unset VK_ICD_FILENAMES VK_DRIVER_FILES

LOG=${LOG:-$HOME/bench-$(date +%H%M%S).log}
: > "$LOG"

mark() { echo "$*" | tee -a "$LOG"; sync; }

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

run_timed() {
   name=$1
   shift
   t0=$(now_ms)
   "$@" >/tmp/o 2>&1
   rc=$?
   t1=$(now_ms)
   mark "$name rc=$rc ms=$((t1 - t0)) $(tail -1 /tmp/o)"
}

mark "kernel=$(uname -r)"
mark "gpuclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null)"
mark "temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"

for i in 1 2 3; do
   mark "iter $i"
   run_timed tu5_bigclear ./tu5_bigclear_test
   run_timed tu5_texcopy ./tu5_texcopy_test
   run_timed tu5_ubo_gfx ./tu5_ubo_gfx_test
   run_timed tu5_compute ./tu5_test
   run_timed vkcube300 vkcube --wsi wayland --c 300 --width 256 --height 256
   mark "gpuclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
done
mark "DONE"
