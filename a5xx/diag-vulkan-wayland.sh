#!/bin/sh
# diag-vulkan-wayland.sh - step-by-step on-screen Vulkan WSI diagnostics.
#
# Writes markers to $HOME/vkdiag.log on the device and syncs after every
# step, so after a GPU hang the last marker shows which test triggered it.
#
# Requirements: SSH key access, a built Turnip, test binaries from a5xx/tests.
# Usage:
#   DRIVER=/path/to/libvulkan_freedreno.so ./diag-vulkan-wayland.sh user@host [--with-vkcube]
set -eu

HOST="${1:-${WHYRED_HOST:-}}"
[ -n "$HOST" ] || {
   echo "usage: DRIVER=... $0 user@host [--with-vkcube]" >&2
   exit 2
}
WITH_VKCUBE=0
[ "${2:-}" = "--with-vkcube" ] && WITH_VKCUBE=1

HERE=$(cd "$(dirname "$0")" && pwd)
DRIVER="${DRIVER:-$HERE/../build-a5xx/src/freedreno/vulkan/libvulkan_freedreno.so}"
TESTS="${TESTS:-$HERE/tests}"
[ -f "$DRIVER" ] || { echo "missing driver: $DRIVER" >&2; exit 1; }
[ -d "$TESTS" ] || { echo "missing tests: $TESTS" >&2; exit 1; }

SSH_OPTS="-o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new"
rssh() { ssh $SSH_OPTS "$@"; }
rscp() { scp $SSH_OPTS "$@"; }

rssh "$HOST" 'mkdir -p /tmp/tu5-test'
rscp "$DRIVER" "$HOST:/tmp/tu5-test/libvulkan_freedreno_mod.so"
rscp "$TESTS"/tu5_tri_test "$TESTS"/tu5_texcopy_test "$TESTS"/tu5_wsi_test \
     "$TESTS"/tu5_wl_wsi_test "$TESTS"/tu5_idx_test "$TESTS"/tu5_texlinear_test \
     "$HOST:/tmp/tu5-test/" 2>/dev/null || true

rssh "$HOST" 'cd /tmp/tu5-test
printf "%s\n" "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"/tmp/tu5-test/libvulkan_freedreno_mod.so\",\"api_version\":\"1.3.368\"}}" > mod.json
export VK_ICD_FILENAMES=$PWD/mod.json
unset VK_DRIVER_FILES TU5_GFX_NO_PRELOAD TU5_GFX_FOUR_QUADS TU5_NO_SUBMIT
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1001} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
LOG="$HOME/vkdiag.log"
: > "$LOG"
mark() { echo "$(date +%H:%M:%S) $*" >> "$LOG"; sync; echo "$*"; }

mark "START tri";    timeout 30 ./tu5_tri_test >/tmp/o 2>&1; mark "END tri rc=$? $(tail -1 /tmp/o)"
mark "START texcopy"; timeout 30 ./tu5_texcopy_test >/tmp/o 2>&1; mark "END texcopy rc=$? $(tail -1 /tmp/o)"
mark "START wsi-headless"; timeout 40 ./tu5_wsi_test >/tmp/o 2>&1; mark "END wsi-headless rc=$? $(tail -1 /tmp/o)"
mark "START idx";    timeout 30 ./tu5_idx_test >/tmp/o 2>&1; mark "END idx rc=$? $(tail -1 /tmp/o)"
mark "START texlinear"; timeout 30 ./tu5_texlinear_test >/tmp/o 2>&1; mark "END texlinear rc=$? $(tail -1 /tmp/o)"
mark "START wl-wsi";  timeout 40 ./tu5_wl_wsi_test >/tmp/o 2>&1; mark "END wl-wsi rc=$? $(tail -2 /tmp/o | tr "\n" " ")"
mark "DONE"
'
if [ "$WITH_VKCUBE" = 1 ]; then
   rssh "$HOST" 'cd /tmp/tu5-test
export VK_ICD_FILENAMES=$PWD/mod.json
unset VK_DRIVER_FILES TU5_GFX_NO_PRELOAD TU5_GFX_FOUR_QUADS TU5_NO_SUBMIT
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1001} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
LOG="$HOME/vkdiag.log"
echo "$(date +%H:%M:%S) START vkcube" >> "$LOG"; sync
timeout 40 stdbuf -oL -eL vkcube --c 1 --width 128 --height 128 >/tmp/vkcube.log 2>&1
rc=$?
echo "$(date +%H:%M:%S) END vkcube rc=$rc $(tail -3 /tmp/vkcube.log | tr "\n" " ")" >> "$LOG"; sync
cat /tmp/vkcube.log | tail -3'
fi
