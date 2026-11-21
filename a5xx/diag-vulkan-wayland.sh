#!/bin/sh
# diag-vulkan-wayland.sh — пошаговая диагностика on-screen Vulkan WSI.
# Пишет маркеры на диск (/home/user/vkdiag.log) и sync после каждого
# шага: если устройство словит GPU hang, после power-cycle видно, где.
#
# Usage: ./diag-vulkan-wayland.sh [user@host] [--with-vkcube]
set -eu

HOST="${1:-user@192.0.2.10}"
WITH_VKCUBE=0
[ "${2:-}" = "--with-vkcube" ] && WITH_VKCUBE=1

# SSH key/agent expected
DRIVER=/tmp/libvulkan_freedreno_mod.so
BASE=/home/user/7.0-mainline-whyred/gpu-fix/mesa-a5xx/turnip-test
: # SSH agent expected

rssh() { setsid -w ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 -p 22 "$@"; }
rscp() { setsid -w scp -P 22 "$@"; }

rssh "$HOST" 'mkdir -p /tmp/tu5-test'
rscp "$DRIVER" "$HOST:/tmp/tu5-test/libvulkan_freedreno_mod.so"
rscp "$BASE/tu5_tri_test" "$BASE/tu5_texcopy_test" "$BASE/tu5_wsi_test" \
     "$BASE/tu5_wl_wsi_test" "$BASE/tu5_idx_test" "$BASE/tu5_texlinear_test" \
     "$HOST:/tmp/tu5-test/"

rssh "$HOST" 'cd /tmp/tu5-test
printf "%s\n" "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"/tmp/tu5-test/libvulkan_freedreno_mod.so\",\"api_version\":\"1.3.368\"}}" > mod.json
export VK_ICD_FILENAMES=$PWD/mod.json
unset VK_DRIVER_FILES TU5_GFX_NO_PRELOAD TU5_GFX_FOUR_QUADS TU5_NO_SUBMIT
export XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-1
LOG=/home/user/vkdiag.log
: > $LOG
mark() { echo "$(date +%H:%M:%S) $*" >> $LOG; sync; echo "$*"; }

mark "START tri";    timeout 30 ./tu5_tri_test >/tmp/o 2>&1; mark "END tri rc=$? $(tail -1 /tmp/o)"
mark "START texcopy"; timeout 30 ./tu5_texcopy_test >/tmp/o 2>&1; mark "END texcopy rc=$? $(tail -1 /tmp/o)"
mark "START wsi-headless"; timeout 40 ./tu5_wsi_test >/tmp/o 2>&1; mark "END wsi-headless rc=$? $(tail -1 /tmp/o)"
mark "START idx";    timeout 30 ./tu5_idx_test >/tmp/o 2>&1; mark "END idx rc=$? $(tail -1 /tmp/o)"
mark "START texlinear"; timeout 30 ./tu5_texlinear_test >/tmp/o 2>&1; mark "END texlinear rc=$? $(tail -1 /tmp/o)"
mark "START wl-wsi";  timeout 40 ./tu5_wl_wsi_test >/tmp/o 2>&1; mark "END wl-wsi rc=$? $(tail -2 /tmp/o | tr "\n" " ")"
if [ "$WITH_VKCUBE" = 1 ]; then
  mark "START vkcube"
  timeout 40 stdbuf -oL -eL vkcube --c 1 --width 128 --height 128 >/tmp/vkcube.log 2>&1
  mark "END vkcube rc=$? $(tail -3 /tmp/vkcube.log | tr "\n" " ")"
fi
mark "DONE"
'
