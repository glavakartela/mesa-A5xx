# Turnip A5xx (Adreno 509): bring-up status

Mesa fork: this repository (`mesa-A5xx`).
Working tag: **`a5xx-vkcube-working`** (`a6e57fe`); master adds
`51828ed`, `04fdfbf` and `43823af`.
Target: Xiaomi Redmi Note 5 / whyred, Adreno 509 (a5xx, sysmem path).

## Commits

- `83f34df` — compute bring-up for Adreno 509;
- `31237b7` — sysmem graphics bring-up;
- `3490401` — bindful TPL1 textures;
- `064541c` — WSI, CPU copies and indexed draws;
- `ca8636a` — tests, docs, installer and KWin workarounds;
- `51828ed` — keep texture samples f32;
- `04fdfbf` — keep bindful tex/sampler tables in the pipeline cache blob;
- `a6e57fe` — public installer and docs (tag `a5xx-vkcube-working`);
- `43823af` — expose `samplerAnisotropy` for vkmark;
- `d464adf`, `03f70b8`, `8a500e6`, `7674e83`, `dc8c5d2` — Gamescope:
  feature set, dynamic rendering, bindful storage images and compute
  textures (master head `71f3bca`).

## Gamescope (2026-11-26)

Gamescope 3.17 now starts and composites on a5xx.  Chip 5 advertises
`dynamicRendering`, `timelineSemaphore`, `scalarBlockLayout`,
`samplerYcbcrConversion`, `nullDescriptor`,
`shaderStorageImageWriteWithoutFormat`, `presentId`/`presentWait`,
`swapchainMaintenance1` and `presentId2`/`presentWait2`.  The compute
path gained bindful storage images (`imageStore` via STIB, `imageLoad`
via ISAM/LDIB) and sampled textures; `vkCmdBeginRendering`/
`vkCmdEndRendering` bypass GMEM/LRZ.

Verified: `gamescope --backend=wayland --expose-wayland` nested in KWin
with `vkcube --wsi wayland` inside it shows the LUNAR cube.  The
headless backend also creates the client swapchain.  `glxgears` through
Xwayland inside Gamescope runs at 60 FPS (`GL_RENDERER = FD509`), so the
X11/GL path used by Wine is functional too.

Limits: sampler arrays use element 0 only; `imageLoad` into a read-write
storage image does not write yet; `gamescopectl screenshot` in the
headless backend crashes Gamescope (normal rendering is unaffected).

The latest vkmark run passes shading/texture/effect2d/desktop/cube scenes,
survives a two-minute stress run with no reset or fault at ~49–53 °C.

## What works

- Vulkan compute: SSBO/UBO, descriptor arrays, push constants, indirect dispatch.
- Graphics: triangles, points, lines, clears, depth/stencil (D16/D24S8/D32),
  blending, MRT, textures (bindful TPL1), varyings via vertex buffers.
- Buffer/image operations: `vkCmdCopyBufferToImage`, `vkCmdCopyImageToBuffer`,
  `vkCmdCopyImage`, `vkCmdClearColorImage`, `vkCmdFillBuffer`,
  `vkCmdUpdateBuffer`, `vkCmdCopyBuffer` (CPU paths on a5xx).
- Indexed draws are expanded on the CPU into non-indexed draws (HW index
  fetch does not work in the sysmem/bypass path).
- WSI: headless swapchain and real Wayland `xdg-shell` swapchain work.
- **vkcube renders the textured LUNAR G cube with depth enabled**, 60+ frames
  without hangs.
- System-wide ICD is installed; `vulkaninfo` without env shows
  `Turnip Adreno (TM) 509`.

## Key findings / fixes

- **Barriers**: generic a6xx cache-flush packets from `vkCmdPipelineBarrier`
  hang a5xx. Barriers are a no-op on chip 5; resources are CPU-managed.
- **CPU copy/clear paths**: the common r2d engine is not usable for a5xx.
  Buffer/image copies and clears are done with memcpy/memset plus explicit
  cache maintenance.
- **Map/unmap cache sync**: a5xx advertises HOST_COHERENT, but the CPU cache
  still needs clean/invalidate on map/unmap.
- **DRM modifiers**: a5xx must only advertise/export `DRM_FORMAT_MOD_LINEAR`;
  a compressed modifier with a linear layout breaks the compositor import.
- **Graphics UBO**: a5xx lowers UBO loads to global loads through pointers in
  const state. The compute path uploaded them; graphics did not, which made
  vkcube read a zero pointer and hang. Fixed for VS and FS, including dynamic
  UBO offsets.
- **Swapchain format 58**: KWin offers `VK_FORMAT_A2R10G10B10_UNORM_PACK32`
  first. It must be supported in MRT, swap and clear; otherwise the cube is
  drawn into an invalid format and the window stays black.
- **D16/D32 depth clear**: the render-pass clear mask kept the generic COLOR
  bit for plain depth formats, so the depth buffer was never cleared and
  `LESS_OR_EQUAL` rejected all fragments. Clear mask is converted to DEPTH;
  tiled depth is cleared over the full `layer_size`.
- **Indexed draws**: `vkCmdBindIndexBuffer` was a weak stub; implemented. HW
  index fetch still fails in sysmem/bypass, so indexed draws are expanded on
  the CPU into a temporary non-indexed VBO.
- **Textures**: f16 `sam`, `HALFREGFOOTPRINT`, `FOUR_QUADS` for FS with half
  registers, texture state preload. Compute must be loaded from memory, not
  preloaded, after graphics.

## Plasma / KWin

- `KWIN_COMPOSE=OpenGL` is fatal for the KWin 6.8.2 greeter when OpenGL
  compositing cannot be fulfilled: KWin logs "Could not fulfill..." and then
  SEGVs in `KWin::OutputLayer::setOutput`. Use only `KWIN_DISABLE_VULKAN=1`.
- Stale Plasma user units without a seat cannot open `/dev/dri/card1` and
  loop; stop them. `environment.d` is read at `user@UID.service` start, so
  restart the user manager or reboot after changing it.

## Install / rollback

Scripts: `a5xx/install-turnip-a5xx.sh`. The script never stores passwords:
use an SSH key and passwordless sudo on the device (`sudo -n true` must
succeed).

```sh
# build the driver, then:
DRIVER=build-a5xx/src/freedreno/vulkan/libvulkan_freedreno.so \
  ./a5xx/install-turnip-a5xx.sh user@host
./a5xx/install-turnip-a5xx.sh user@host --rollback
```

Backups are stored as `$HOME/turnip-a5xx-backup-*`.

## Known limitations / WIP

- The app-mapped image path for textures (`tu5_texvb_test`) is flaky: it can
  sample zeros. The realistic path, staging buffer -> `vkCmdCopyBufferToImage`
  (`tu5_texcopy_test`), works and is used by vkcube.
- LINEAR sampler on an app-mapped image is WIP.
- The CPU index expansion covers one-instance draws and common bindings.
- KWin ScreenShot2 + a Vulkan window can hang the GPU; do not use it in
  automated loops.
- GPU hang hardlocks the AP; only a full power-off recovers.
- No GMEM/binning, MSAA, LRZ; WSI is linear/sysmem.

## Next phases

Stability (24-72h loops, kernel logs), performance (GPU 430 MHz fused cap,
fillrate/texture/compute, thermals), rendering artifacts, kernel panics/CPU
hardlockups, and non-breaking deep sleep (ADSP/rproc, cpuidle workaround).
See `TESTING.md` and `TESTING.en.md`.

- **Critical**: after installation verify that both
  `~/.config/environment.d/99-a5xx-kwin.conf` and
  `/var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf` really
  contain `KWIN_DISABLE_VULKAN=1`. If the line is lost, KWin 6.8 picks our
  Turnip Vulkan: password/rotation artifacts, `Physical device Turnip ...
  doesn't support host query resets`, GPU fault `iova=0` and vblank timeout.
  `install-turnip-a5xx.sh` now rewrites the file with `printf`.
