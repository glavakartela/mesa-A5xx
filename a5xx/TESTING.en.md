# Turnip A5xx: test suite and validation

Driver: `src/freedreno/vulkan/tu5_*` plus fixes in common Turnip paths.
Working tag: `a5xx-vkcube-working` (`af1f612`).

## Build the tests

Requires a cross compiler and a rootfs with libvulkan/wayland:

```sh
cd a5xx/tests
CROSS=/path/to/cross/bin/aarch64-alarm-gcc \
ROOTFS=/path/to/rootfs \
./build-all.sh
```

`build-all.sh` builds every `tu5_*` test; `tu5_wl_wsi_test` additionally
uses `xdg-shell-protocol.c` and `libwayland-client` from the rootfs.

## Run on the device (system ICD)

```sh
unset VK_ICD_FILENAMES VK_DRIVER_FILES
cd /tmp/tu5-test
for t in tu5_test tu5_ubo_test tu5_array_test \
         tu5_tri_test tu5_small_test tu5_fsprobe_test tu5_vb_test \
         tu5_vspos_test tu5_vscount_test tu5_vsprobe_test \
         tu5_point_test tu5_pointprobe_test tu5_line_test \
         tu5_clear_test tu5_cleardraw_test tu5_bigclear_test \
         tu5_stencil_test tu5_mrt_test tu5_uvvb_test \
         tu5_texcopy_test tu5_ubo_gfx_test tu5_idx_test \
         tu5_wsi_test tu5_wl_wsi_test tu5_bufops_test \
         tu5_clearcopy_test tu5_storage_image_test \
         tu5_dynrender_test; do
  timeout 30 ./$t >/tmp/o 2>&1
  echo "$t rc=$? $(tail -1 /tmp/o)"
done
for a in "" d24s8; do timeout 30 ./tu5_depth_test $a; done
timeout 30 ./tu5_blend_test
timeout 30 ./tu5_blend_alpha_test
```

Expected: all PASS. At the working tag the smoke set is 27/27 (texvb excluded
as WIP). `tu5_texvb_test` uses direct app-mapped image memory and is known to
be flaky; `tu5_texcopy_test` covers the real staging path.

## Compute: storage images and Gamescope features

`tu5_storage_image_test` exercises the Gamescope-style compute path:
binding 0 is a storage image (imageStore) and binding 1 a combined image
sampler (texelFetch). `tu5_storage_image_test grid` additionally records
`gl_GlobalInvocationID`/`gl_WorkGroupID` in an SSBO to validate the 2D grid.

```sh
timeout 30 ./tu5_storage_image_test
# expected: PASS: compute sampled texture -> storage image
timeout 30 ./tu5_storage_image_test grid
# diagnostics: "grid touched=4096 wrong=0/4096"
```

For chip 5 Turnip now advertises `dynamicRendering`, `timelineSemaphore`,
`scalarBlockLayout`, `samplerYcbcrConversion`, `nullDescriptor` and
`presentId`/`presentWait`, the minimum Gamescope 3.17 requests at
`vkCreateDevice`. `tu5_dynrender_test` exercises
`vkCmdBeginRendering`/`vkCmdEndRendering` itself (triangle + loadOp=CLEAR).

## WSI / vkcube

```sh
# headless WSI
./tu5_wsi_test

# real Wayland (active Plasma session)
WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1001 \
  vkcube --wsi wayland --c 60
```

Expected: LUNAR G cube with texture and depth, rc=0.
`TU5_GFX_NODEPTH=1` is a debug knob that disables the depth test/write.

## Install / rollback

```sh
DRIVER=build-a5xx/src/freedreno/vulkan/libvulkan_freedreno.so \
  ./a5xx/install-turnip-a5xx.sh user@host
./a5xx/install-turnip-a5xx.sh user@host --rollback
```

Backups: `$HOME/turnip-a5xx-backup-*`.

## Known issues

- LINEAR sampler on an app-mapped image can return zeros (WIP).
- HW index fetch is broken in sysmem/bypass; indexed draws are CPU-expanded
  (one instance, common bindings).
- KWin ScreenShot2 + a Vulkan window can hang the GPU; avoid in automation.
- GPU hang hardlocks the AP; full power-off is the only recovery.
- `dmesg`/journal need root; collect with `sudo`.

## Next phase: stability / performance / sleep

1. 24-72h loop of the safe suite + vkcube, logs on disk with `sync`; watch
   `dmesg`/`journalctl -k` for GPU faults, hardlockups, panics, ath10k/ADSP.
2. Performance: fillrate/texture/compute, vkcube frame time, GPU frequency
   (fused 430 MHz), thermals/battery.
3. Artifacts: readback matrix for formats/depth/blend/MRT/textures; careful
   vkcube screenshots.
4. Deep sleep: ADSP `remoteproc` crash / `rproc_crash_handler_work`, cpuidle
   workaround; `echo mem` + RTC wake without breaking the session.

- `tests/stress.sh` — reproducible stress loop with a disk log
  (`MODE=safe|full`, `VKCUBE=1`).
- `tests/collect-logs.sh` — dmesg/journal/pstore collector.
- `tests/bench.sh` — simple timing benchmark (bigclear, texcopy, UBO, compute,
  vkcube 300).
