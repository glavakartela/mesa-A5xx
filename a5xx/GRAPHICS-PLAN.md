# A5xx Turnip: план графики

Текущее: sysmem-графика с треугольниками, точками, линиями,
depth/stencil, blending, MRT и текстурами. Последние коммиты: `98ea3ad`,
`7c3da2b`, `ce84bda`.

Сделано: CP_SET_RENDER_MODE(BYPASS), SP/HLSQ program state, VFD, VPC,
RB MRT, sample mask, UCHE invalidate между процессами, swap как в fd5,
loadOp=CLEAR через CP_MEM_WRITE (включая D/S), depth test и stencil,
blending (факторы/op/константы), несколько MRT, Y-ориентация viewport.

Сделано для реальных приложений (CPU-пути, `ce84bda`):
- `vkCmdCopyBufferToImage2` / `vkCmdCopyImageToBuffer2` — staging <-> linear image;
- `vkCmdCopyImage2`, `vkCmdClearColorImage` — linear color;
- `vkCmdFillBuffer`, `vkCmdUpdateBuffer`, `vkCmdCopyBuffer2`;
- `vkCmdPipelineBarrier` на chip 5 — no-op: a6xx flush-пакеты вешают GPU;
- тесты: `tu5_texcopy_test` PASS, `tu5_bufops_test` PASS,
  `tu5_imgops_test` PASS, `tu5_clearcopy_test` PASS;
- итог: **29/29 PASS через системный ICD**, без `VK_ICD_FILENAMES`.

WSI: `VK_KHR_swapchain` есть; `tu5_wsi_test` (headless) и
`tu5_wl_wsi_test` (xdg-shell, B8G8R8A8_SRGB) проходят.
`vkcube --wsi wayland --c 60` через системный ICD — rc=0, без hang.

Indexed draw: HW index fetch в sysmem/bypass не работает; сделан CPU-разворот
в non-indexed (временный VBO). `tu5_idx_test` PASS.

vkcube: работает и рисует куб. Починены: graphics UBO-указатели в const
state, dynamic UBO offset, swapchain-формат 58
(A2R10G10B10_UNORM_PACK32), D16/D32 depth clear (clear_mask COLOR→DEPTH),
полный clear tiled depth layer.

Осталось: LINEAR-сэмплер на app-mapped образе даёт нули (WIP).

Дальше по графике:
1. WSI/swapchain (DMA-BUF export/import, VK_KHR_swapchain) —
   следующий крупный шаг для on-screen Vulkan;
2. CPU-clear depth/stencil (tiled) и `vkCmdClearAttachments`;
3. GMEM/binning + tile store/load (сейчас только sysmem);
4. preload шейдеров в HLSQ (сейчас external memory);
5. MSAA, LRZ, dual-source blend, независимый blend по RT;
6. CPU-clear depth/stencil и MSAA.

1. Device/entrypoints
   - разрешить graphics queue/features только после реальной проверки;
   - отдельные tu5-эмиттеры не должны попадать в A6xx пути.

2. Шейдеры VS/FS
   - A5xx program state: SP_VS/SP_FS, VPC/VFD, константы, shader object;
   - IR3 варианты VS/FS компилируются общим компилятором, нужен только эмиттер.

3. Render pass / tiling
   - GMEM/VSC, binning, tile store/load;
   - для первого теста можно sysmem + один render target без binning.

4. Draw state
   - rasterizer, viewport/scissor, RB MRT, depth/stencil off;
   - CP_DRAW_INDX/CP_DRAW_INDX_OFFSET и fence/flush.

5. Минимальный тест
   - очистка/треугольник в 256x256 R8G8B8A8, readback через host-visible буфер;
   - затем dEQP/CTS.

Оценка: это недели работы, не одна сессия; каждый шаг требует
fastboot boot нового kernel/DTB и проверки на A509.
