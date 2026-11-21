#!/bin/sh
# Cross-build the a5xx Turnip compute smoke test.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BASE=$(cd "$HERE/.." && pwd)
CROSS=${CROSS:-$BASE/cross/bin/aarch64-alarm-gcc}
SYSROOT=${SYSROOT:-$BASE/sysroot}
ROOTFS=${ROOTFS:-$HOME/rootfs}

if [ ! -f "$HERE/ssbo_spv.h" ]; then
   glslc -fshader-stage=comp --target-env=vulkan1.0 -mfmt=c \
      "$HERE/ssbo.comp" -o "$HERE/ssbo_spv.inc"
   printf '#pragma once\n#include <stdint.h>\nstatic const uint32_t ssbo_spv[] =\n#include "ssbo_spv.inc"\n;\n' \
      > "$HERE/ssbo_spv.h"
fi

# Link against the real loader object instead of -L$ROOTFS/usr/lib: the
# rootfs libc.so is a linker script with absolute phone paths, which makes
# the host linker pick up x86_64 libraries.
"$CROSS" -O2 -Wall -o "$HERE/tu5_test" "$HERE/tu5_test.c" -I"$HERE" \
   "$ROOTFS/usr/lib/libvulkan.so.1.4.357"

echo "built: $HERE/tu5_test"
file "$HERE/tu5_test"
