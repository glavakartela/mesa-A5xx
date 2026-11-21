# Mesa A5xx / Adreno 509 bring-up

Development fork of Mesa (`27.0.0-devel`) with a Turnip Vulkan bring-up for
the Adreno 509 GPU in the Qualcomm SDM636 (Xiaomi Redmi Note 5 / Note 6 Pro,
`whyred`).

**Status: work in progress, not upstream Mesa.** `vkcube`, basic Vulkan
graphics/compute, textures and Wayland WSI work on the target device; the
driver is not a drop-in daily driver yet. See
[`a5xx/README.md`](a5xx/README.md) and [`a5xx/STABILITY.md`](a5xx/STABILITY.md)
for the tested feature matrix and known limitations.

## What is here

- `src/freedreno/` — Turnip changes (a5xx sysmem path, bindful TPL1 textures,
  depth/blending/MRT, pipeline-cache serialization, WSI).
- `a5xx/patches/` — the bring-up split into reviewable patches.
- `a5xx/tests/` — headless and on-screen Vulkan tests.
- `a5xx/install-turnip-a5xx.sh` — install a locally built Turnip ICD on a
  device over SSH.
- `a5xx/diag-vulkan-wayland.sh` — on-device WSI diagnostics.

## Build and install

```sh
meson setup build-a5xx -Dvulkan-drivers=freedreno -Dgallium-drivers= \
  -Dplatforms=wayland -Dbuildtype=release
ninja -C build-a5xx

DRIVER=build-a5xx/src/freedreno/vulkan/libvulkan_freedreno.so \
  ./a5xx/install-turnip-a5xx.sh user@host
```

The install script never stores passwords: it expects SSH key access and
passwordless `sudo` on the target. Roll back with:

```sh
./a5xx/install-turnip-a5xx.sh user@host --rollback
```

## Upstream status

This fork is **not** an upstream Mesa submission. The a5xx bindful path adds
state (`tex_map`/`samp_map`) that upstream `tu_shader` does not have, so the
pipeline-cache fix cannot be applied to `main` on its own. The intended path
is to stabilise the bring-up here first, then submit a cleaned series for
review.

Upstream Mesa is licensed under the MIT license; this fork keeps the same
licensing. The base snapshot is imported from Mesa and the bring-up commits
are maintained on top of it.
