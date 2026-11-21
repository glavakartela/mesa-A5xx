English | [Русский](STABILITY.md)

# A5xx stability: run log

## Run 1 (17:35, safe + texcopy + vkcube60, 20 min)

- Device booted, system ICD `git-a73cf2af75`, Plasma alive.
- Pre-stress smoke: 27/27 PASS except `tu5_texvb_test` (app-mapped WIP);
  `vkcube --c 60` rc=0.
- Started a 20-minute `safe + tu5_texcopy_test + vkcube60` loop with a
  disk log at `$HOME/stress-173537.log` (`sync` after every
  line).
- After a few minutes the device disappeared from the network (`.73` dead;
  the LAN scan found only the old `.39`). Either a GPU hardlock or ath10k.
- **The log is on disk.** After a power cycle, the last `BEGIN/END` line
  identifies the trigger.

## Bisection plan after power-on

1. Read `stress-173537.log`:
   - last `BEGIN tu5_texcopy_test`/`vkcube60` → texture/WSI suspicion;
   - log reaches `DONE` or last `iter done` → likely Wi-Fi/ath10k, not GPU;
   - `STOP`/`timeout` → the test itself returned an error.
2. Run `MODE=safe DURATION=1800` (no textures, no vkcube) as a baseline.
3. If safe is stable, add `tu5_texcopy_test` (`MODE=full`).
4. If full is stable, add `VKCUBE=1`.
5. For every run keep a root watcher:
   `sudo sh -c 'dmesg -w > /var/log/a5xx-kmsg.log' &`
   and collect `journalctl -k -b -1` + `/sys/fs/pstore` after reboot.
6. Record `gpuclk`, temperature, iteration count and rc.

## Tools

- `tests/stress.sh` — disk-logged loop (`MODE=safe|full`, `VKCUBE=1`).
- `tests/collect-logs.sh` — dmesg, current/previous-boot journal, pstore.

## Hypotheses for the first hang

- **Texture path** (`tu5_texcopy_test`, vkcube): TPL1/UCHE WIP; repeated
  processes may accumulate state.
- **WSI/KWin**: repeated swapchain/present and dmabuf import/export.
- **ath10k**: the device often dropped off the network; if the log reached
  the end, it was Wi-Fi, not GPU.
- **Power/thermal**: check temp/battery in the log.


## Run 2 (18:45, controlled)

- Fresh boot, `bigclear -> texcopy`: `bigclear` PASS, `texcopy` returned
  **rc=1 (zeros)**, not a hang. A repeat of `texcopy`/`texvb` hung the device
  with no output. The texture path (staging -> image -> sampler) is flaky:
  sometimes zeros, sometimes a hang; vkcube still renders its cube.
- Previous-boot kernel log: `a5xx_irq gpu fault ring 0 status 800001C1`,
  `hangcheck recover`, `msm_dpu CP opcode error`; separately
  `ath10k_snoc ... failed to create WMI vdev 0: -108`.
- Wi-Fi was on 5 GHz `KB_36_5G`, -87..-92 dBm; power save disabled.
  2.4 GHz `KB_36_EXT` (79) and `KB_36` (67) are available.
- Prepared a `texfix` driver: whole image BO flush after CPU copy,
  `TU5_GFX_TEXWAIT` and TPL1 descriptor debug output.
- Next: run `texcopy-stress.sh` with `texfix` + `TU5_GFX_DEBUG=1`, compare
  descriptors/addresses; on a hang the last disk log identifies the step.

## KWin picked Turnip Vulkan (19:30)

- The KWin env files kept only `KWIN_USE_BUFFER_AGE=0` (plus a line of
  spaces); `KWIN_DISABLE_VULKAN=1` was lost.
- KWin 6.8 greeter selected Vulkan: `Physical device Turnip Adreno 509
  doesn't support host query resets`; texture/rotation led to GPU fault
  `iova=0`, `vblank wait timed out`, kernel trace, reboot.
- Fixed: both env files rewritten (`KWIN_USE_BUFFER_AGE=0`,
  `KWIN_DISABLE_VULKAN=1`), `user@962/1001` restarted; greeter KWin is now
  OpenGL and does not use Turnip.
- A single `tu5_texcopy_test` with texflush/ptrlog (KWin stopped) passed;
  all pointers non-zero. Next: headless stress without KWin and OpenGL
  artifact checks.

## Plasma OpenGL artifacts (19:45+)

- The KWin/Plasma artifacts were on **OpenGL** (system Mesa `msm_dri`,
  fd5), not Turnip: white stripes/"bricks", text/rotation, shade lag.
- The system had the Mesa issue 8442 workaround
  `FD_MESA_DEBUG=noblit` (`/etc/profile.d/sdm660-adreno-quirks.sh`),
  which breaks blits/UI.
- Replaced with `FD_MESA_DEBUG=sysmem` (blits stay, GMEM/tiling off);
  KWin greeter/user no longer logs `a5xx_irq`/vblank faults.
- Also `KWIN_DRM_NO_DIRECT_SCANOUT=1`, `KWIN_DRM_USE_MODIFIERS=0`,
  blur disabled in `kwinrc`.
- Open question: remaining white stripes/"bricks" - verify with `sysmem`
  + no-scanout; if they remain, try `noblit,inorder,gmem` or rebuild a
  newer fd5 GL.
