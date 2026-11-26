# A5xx Turnip: план графики

Текущее: sysmem-графика с треугольниками, точками, линиями,
depth/stencil, blending, MRT и текстурами; compute с SSBO/UBO/push
constants, storage images и sampled textures. Последние коммиты:
`98ea3ad`, `7c3da2b`, `ce84bda` (графика), `d464adf`, `03f70b8`, `8a500e6`
(Gamescope-фичи и compute-изображения).

## Gamescope

Gamescope 3.17 падал на `vkCreateDevice` с
`VK_ERROR_FEATURE_NOT_PRESENT`: chip 5 рекламировал только три фичи.
Теперь для chip 5 включены ровно те, что нужны Gamescope (и реализуемы):
`dynamicRendering`, `timelineSemaphore`, `scalarBlockLayout`,
`samplerYcbcrConversion`, `nullDescriptor`,
`shaderStorageImageWriteWithoutFormat`, `presentId`/`presentWait`,
`swapchainMaintenance1` и `presentId2`/`presentWait2`.
`VK_EXT_swapchain_maintenance1` обязателен: Gamescope WSI форсирует его
и патчит `VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT` в true.
`vkCmdBeginRendering`/`vkCmdEndRendering` для chip 5 обходят GMEM/LRZ и
работают как sysmem render pass.

Проверено на A509: `gamescope --backend=wayland --expose-wayland` внутри
KWin, внутри него `vkcube --wsi wayland` — куб LUNAR рисуется и
композитится на экран. Headless-бэкенд также стартует и создаёт
swapchain клиента.

Compute-путь получил bindful storage images: descriptor set 0, слот =
Vulkan binding; `imageLoad` идёт через TEX-блок (ISAM/LDIB), `imageStore`
через SSBO-блок (STIB). Для LDIB/STIB в const state загружаются
bytes-per-pixel, pitch и layer size. Тест: `tu5_storage_image_test`
(включая `grid`-режим для проверки 2D-сетки) — PASS на A509.

Ограничения: сэмплерные массивы эмитятся только для элемента 0
(Gamescope для одного слоя использует `s_samplers[0]`), динамические
image-массивы не поддержаны. `tu5_storage_image_test load` (явный
`imageLoad` в read-write образ) пока не проходит; Gamescope пишет
storage images без чтения, поэтому на композитор это не влияет.
Debug-команда `gamescopectl screenshot` в headless-режиме роняет
Gamescope (backtrace в `tu_AllocateMemory`); обычный рендер и
презентация работают.

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
