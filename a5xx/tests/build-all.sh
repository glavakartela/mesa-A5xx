#!/bin/sh
# Build the a5xx Turnip test programs.
# Usage:
#   CROSS=/path/to/aarch64-alarm-gcc ROOTFS=/path/to/rootfs ./build-all.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=${CROSS:-aarch64-alarm-gcc}
ROOTFS=${ROOTFS:-/home/user/rootfs}
VK=${VK:-$ROOTFS/usr/lib/libvulkan.so.1.4.357}
WAYLAND=${WAYLAND:-$ROOTFS/usr/lib/libwayland-client.so.0.26.0}

[ -x "$CROSS" ] || command -v "$CROSS" >/dev/null || {
   echo "CROSS not found: $CROSS" >&2
   exit 1
}
[ -f "$VK" ] || { echo "missing $VK" >&2; exit 1; }

cd "$HERE"

# Shaders: regenerate .inc if glslc is available, otherwise use the
# checked-in generated includes.
if command -v glslc >/dev/null 2>&1; then
   for f in *.vert; do
      [ -f "$f" ] || continue
      base=${f%.vert}
      glslc -fshader-stage=vert --target-env=vulkan1.0 -mfmt=c \
         "$f" -o "${base}_vert.inc"
   done
   for f in *.frag; do
      [ -f "$f" ] || continue
      base=${f%.frag}
      glslc -fshader-stage=frag --target-env=vulkan1.0 -mfmt=c \
         "$f" -o "${base}_frag.inc"
   done
fi

for src in tu5_*.c; do
   [ "$src" = "tu5_wl_wsi_test.c" ] && continue
   out=${src%.c}
   echo "CC $out"
   "$CROSS" -O2 -Wall -I. -o "$out" "$src" "$VK"
done

echo "CC tu5_wl_wsi_test"
"$CROSS" -O2 -Wall -I. -I"$ROOTFS/usr/include" \
   -o tu5_wl_wsi_test tu5_wl_wsi_test.c xdg-shell-protocol.c \
   "$WAYLAND" "$VK"

echo "done"
