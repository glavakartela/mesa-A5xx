English: [README.en.md](README.en.md)

# Turnip a5xx: состояние MVP

Mesa-дерево: `~/mesa-a5xx-work`, ветка master.

Коммиты:
- `09bd044` — изолированный a5xx compute-путь;
- `16f2474` — доведение до работы на Adreno 509;
- `a8f7fd5` — несколько SSBO и indirect dispatch;
- `a5d864c` — UBO через global loads;
- `80108be` — массивы дескрипторов (bindful SSBO);
- `d729aaf` — WIP графический путь;
- `1468d99` — **первый рабочий Vulkan-треугольник (sysmem)**;
- `66125ee` — UCHE invalidate в compute;
- `36dfff4` — Y-ориентация viewport, points, polygon mode;
- `791e889` — loadOp=CLEAR через `CP_MEM_WRITE`, linear-only для chip 5;
- `85f4c53` — depth/stencil (D16/D24S8/D32/D32S8), depth test;
- `06e204b` — blending (факторы, op, blend constants);
- `08e28cf` — несколько MRT;
- `92caef4` — bindful-текстуры (TPL1), WIP;
- `91b0318` — текстуры + varying через VB, фикс HLSQ sysval regid;
- `98ea3ad` — **WIP**: f16-текстуры, HALFREGFOOTPRINT, FOUR_QUADS, фикс compute preload;
- `7c3da2b` — cache sync на map/unmap (a5xx рекламирует HOST_COHERENT, но CPU-кэш нужно чистить);
- `ce84bda` — CPU-пути для buffer/image ops, barrier'ы на chip 5 — no-op;
- `1709362` — a5xx не рекламирует/не экспортирует DRM modifiers (только LINEAR);
- `fbe11fb` — graphics UBO: указатели в const state (иначе VS/FS читает 0 и GPU висит);
- `vkCmdBindIndexBuffer` — реализован (core 1.0 entrypoint был weak-заглушкой).

Проверено на телефоне (Adreno 509):

| Тест | Результат |
|------|-----------|
| direct dispatch, SSBO binding 0 | PASS 512/512 |
| indirect dispatch | PASS 512/512 |
| SSBO на binding 1 | PASS 512/512 |
| `.length()` / get_ssbo_size | PASS 512/512 |
| push constants | PASS 512/512 |
| UBO (global-load path) | PASS 512/512 |
| массивы дескрипторов (2 SSBO) | PASS 512/512 |
| графика: full-screen треугольник | PASS (4096/4096 red) |
| графика: маленький треугольник | PASS (512/512 red) |
| VB/VS-probe/vspos/vscount | PASS |
| point list | PASS (3 точки) |
| line list (диагональ) | PASS (32 px) |
| loadOp=CLEAR, без draw | PASS (4096/4096) |
| CLEAR + треугольник | PASS (3584 green + 512 red) |
| CLEAR 1920x1080 (multi-chunk CP_MEM_WRITE) | PASS (2073600 px) |
| depth D32, LESS + запись | PASS (углы red, центр blue, третий draw отбракован) |
| depth D24S8 | PASS |
| stencil D24S8 (REPLACE/ref=1, EQUAL/ref=0, EQUAL/ref=1) | PASS |
| blending ONE+ONE/ADD | PASS (magenta) |
| blending SRC_ALPHA/ONE_MINUS_SRC_ALPHA | PASS (127,0,128,191) |
| MRT: 2 color attachment | PASS (отдельные red/green) |
| staging → `vkCmdCopyBufferToImage` → sampler | PASS (2048 red + 2048 green) |
| `vkCmdClearColorImage` + `vkCmdCopyImage` → sampler | PASS (4096 red) |
| `vkCmdFillBuffer` / `vkCmdUpdateBuffer` / `vkCmdCopyBuffer2` | PASS |
| CPU clear/copy image без submit (проверка памяти) | PASS |
| `VK_EXT_headless_surface` + `VK_KHR_swapchain` | PASS (3 кадра presented) |
| **системный ICD, полный набор** | **29/29 PASS без env** |

Ключевые находки по графике:
- `RB_BLEND_CNTL` должен содержать `SAMPLE_MASK(0xffff)`: при нулевой
  маске FS не запускается вообще (VS при этом работает);
- UCHE физически индексирован и не сбрасывается между процессами:
  перед каждым draw/dispatch нужен `UCHE_CACHE_INVALIDATE` (0xe91..0xe95,
  значение 0x12) + `CP_WAIT_FOR_IDLE`, иначе новый процесс читает чужие
  вершины/шейдеры (симптом: рисуется геометрия предыдущего процесса);
- swap для MRT как в fd5: `R8G8B8A8 -> WZYX`, `B8G8R8A8 -> WXYZ`;
- `PC_RASTER_CNTL` POLYMODE задаётся polygonMode (FILL/LINE/POINT), а не
  топологией примитивов;
- Vulkan NDC +y вниз, поэтому `GRAS_CL_VPORT_YSCALE` положительный
  (у fd5/GL знак противоположный). Точки сразу показали ошибку: NDC
  (-0.5,-0.5) попадала в левый-нижний угол вместо левого-верхнего;
- для points/lines нужен `GRAS_CL_CNTL_VP_CLIP_CODE_IGNORE` (как в fd5);
- **fast clear (`RB_CLEAR_CNTL`) в sysmem нельзя**: последовательность
  `RB_CLEAR_CNTL=FAST_CLEAR` + `CCU_RESOLVE` без GMEM вешает GPU.
  Clear сделан сырым `CP_MEM_WRITE` по памяти образа (по dword на
  пиксель); для 1920x1080 это ~8 МБ командного потока, работает, но
  это только MVP — нужен clear через draw/blit;
- **depth/stencil нельзя делать linear**: HW не поддерживает linear D/S,
  а в `RB_DEPTH_BUFFER_INFO` нет поля tile mode. Поэтому для chip 5
  linear форсится только для color, D/S остаётся TILE6_3 (UBWC off);
- упаковка 24-битной глубины: считать через `double`. `float` не
  представляет 16777215.5 и округляет вверх до 2^24, т.е. в бит
  стенсила (симптом: depth test «отключён»);
- D24S8 хранит depth и stencil в одном dword, поэтому очистка только
  одной грани делается лишь когда вторая — DONT_CARE;
- blending как в fd5: `RB_MRT_CONTROL_BLEND|BLEND2`, маска в
  `RB_BLEND_CNTL`/`SP_BLEND_CNTL`, `SP_BLEND_CNTL_UNK8` при blend,
  факторы/op в `RB_MRT_BLEND_CONTROL`, константы в блоке
  `RB_BLEND_RED` (8 dword);
- MRT: regid'ы `FRAG_RESULT_DATA0..7` уже писались в
  `SP_FS_OUTPUT_REG`; для нескольких RT нужны per-RT
  `RB_MRT_*`, `SP_FS_MRT_REG`, `RB_RENDER_COMPONENTS` и счётчик в
  `RB/SP_FS_OUTPUT_CNTL_MRT`.

Текстуры (`91b0318` + `98ea3ad`, проверено):
- bindful TPL1-путь, дескриптор/sampler совпадают с fd5;
- FD5-совместимые находки: f16 `sam` + `cov.f16f32`, `HALFREGFOOTPRINT>=1`,
  `FOUR_QUADS` для FS с half-регистрами, `MAX_LOD=0.125`, texture-шейдеры
  должны быть preloaded;
- compute нельзя preload'ить после graphics-preload: внутренний
  instruction store остаётся от VS/FS, dispatch читает нули. Compute
  теперь всегда грузится из памяти;
- **важно**: тест пишет текстуру через CPU-mapped image memory. Без
  `vkFlushMappedMemoryRanges` (пока память замаплена) GPU иногда читает
  нули и вешает контекст. С flush — стабильно; драйверный sync на
  map/unmap добавлен (`7c3da2b`), но старый app-map путь всё равно
  иногда даёт нули — реальные приложения грузят текстуру через copy;
- **copy path**: `vkCmdCopyBufferToImage2`/`vkCmdCopyImageToBuffer2` на
  chip 5 делают CPU memcpy (staging -> linear image), тест
  `tu5_texcopy_test` PASS — sampler читает red/green после
  `vkCmdCopyBufferToImage`;
- **barrier'ы на chip 5 — no-op** (`ce84bda`): generic a6xx cache-flush
  пакеты из `vkCmdPipelineBarrier` вешают GPU (hardlockup);
- **loader**: тестовый ICD JSON должен объявлять
  `api_version = 1.3.368`, а тест — создавать instance с
  `VK_API_VERSION_1_3`, иначе `vkCmdCopyBuffer2`/`vkCmdCopyImage2`
  приходят как NULL и тест падает в SIGSEGV;
- итог: **29/29 PASS через системный ICD**, без `VK_ICD_FILENAMES`;
- WSI: `VK_KHR_swapchain` рекламируется, **headless** swapchain работает
  (`tu5_wsi_test`: acquire -> render pass clear -> present, 3 кадра);
- **Wayland WSI работает**: минимальный тест `tu5_wl_wsi_test`
  (xdg-shell + swapchain B8G8R8A8_SRGB, 3 кадра) — PASS;
- **vkcube работает и рисует куб** (`vkcube --wsi wayland`), проверено
  скриншотом KWin: текстурированный LUNAR G, depth включён;
- причины чёрного окна: (1) graphics UBO-указатели не загружались;
  (2) KWin отдаёт swapchain-формат 58
  `VK_FORMAT_A2R10G10B10_UNORM_PACK32`, а `tu5_format`/clear его не знали;
  (3) для D16/D32 `clear_mask` оставался COLOR, depth не очищался и
  `LESS_OR_EQUAL` отбрасывал куб; (4) WSI-mодификаторы/UBO dynamic offset;
- исправлено: формат 58 в MRT/swap/clear, D16/D32 depth clear,
  dynamic UBO offset, полный clear tiled depth layer, CPU-разворот
  indexed draw;
- индексный draw: HW index fetch в sysmem/bypass-режиме не работает,
  поэтому indexed draw **разворачивается на CPU** во временный VBO и
  рисуется как non-indexed. `tu5_idx_test` — PASS; (vkcube использует
  non-indexed 36-вершинный путь, но CPU-разворот нужен для других приложений);
- LINEAR-сэмплер в offscreen-тесте на app-mapped образе всё ещё даёт
  нули (WIP, отдельно от vkcube).
- `vulkaninfo --summary` без env:
  `deviceName = Turnip Adreno (TM) 509`,
  `driverName = turnip Mesa driver`,
  `driverInfo = Mesa 27.0.0-devel (git-afc436e62d)`.

Plasma/KWin (разбор падения greeter'а):
- **нельзя ставить `KWIN_COMPOSE=OpenGL`**: в KWin 6.8.2 при невозможности
  создать запрошенный compositing он пишет
  `Could not fulfill the requested compositing mode in KWIN_COMPOSE: 1.
  Exiting.` и затем падает с SEGV в `KWin::OutputLayer::setOutput`
  (DRM output-layer teardown). Это и был краш greeter'а;
- правильная настройка: только `KWIN_DISABLE_VULKAN=1` (и при желании
  `KWIN_USE_BUFFER_AGE=0`); renderer по умолчанию — OpenGL;
- старые Plasma-юниты пользователя, поднятые без seat, не могут открыть
  `/dev/dri/card1` (`Permission denied`) и уходят в цикл рестарта KWin.
  Их нужно остановить (`systemctl --user stop plasma-kwin_wayland.service
  plasma-plasmashell.service ...`) — активный greeter держит DRM;
- `environment.d` читается при старте `user@UID.service`: после правки
  env-файла нужен `systemctl restart user@UID.service` или reboot,
  иначе KWin продолжит получать старое окружение;
- после фикса greeter-KWin работает, login screen живой.

Системная установка (боевой режим):
- ICD: `/usr/share/vulkan/icd.d/freedreno_icd.json` →
  `/usr/lib/turnip-a5xx/libvulkan_freedreno.so`;
- ставится `bash install.sh user@192.0.2.10 --ops`
  (stable — без аргумента, textures — `--texture`);
- откат: `bash install.sh user@192.0.2.10 --rollback`;
- бэкапы: `/home/user/turnip-a5xx-backup-*`;
- KWin: только `KWIN_DISABLE_VULKAN=1` (без `KWIN_COMPOSE=OpenGL`!)
  в `~/.config/environment.d/99-a5xx-kwin.conf` и
  `/var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf`;
- **важно**: в `install.sh` был баг — `ACTION` сравнивался с `ops`, а
  приходило `--ops`, поэтому `--ops/--texture/--rollback` не срабатывали
  и ставился stable. Исправлено (`ACTION="${ACTION#--}"`).

Ограничения:
- только descriptor set 0 (SSBO/UBO/текстуры есть);
- sysmem-рендеринг, GMEM/binning ещё нет;
- buffer/image fill, update, copy и clear color — CPU-путём;
  depth/stencil clear — только через render pass (CPU-clear D/S пока нет);
- `vkCmdCopyImage`/`vkCmdClearColorImage` — CPU только для linear color,
  tiled/compressed падают обратно в общий путь;
- нет MSAA, LRZ, dual-source blend, независимого blend по RT;
- **системно установлен stable `08e28cf`** в `/usr/lib/turnip-a5xx/`
  (ICD заменён, `vulkaninfo` без env = Turnip A509); texture/copy
  драйвер ставится per-app через `VK_ICD_FILENAMES`.

Тест: `../../gpu-fix/mesa-a5xx/turnip-test/` (`./build.sh`,
`./run-on-device.sh`).

- **КРИТИЧНО**: после установки проверять, что в
  `~/.config/environment.d/99-a5xx-kwin.conf` и
  `/var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf` реально
  есть `KWIN_DISABLE_VULKAN=1`. Если строка потеряна, KWin 6.8 уходит на
  наш Turnip Vulkan: артефакты при вводе пароля/повороте, `Physical device
  Turnip ... doesn't support host query resets`, GPU fault `iova=0` и
  vblank timeout. `install.sh` теперь перезаписывает файл через `printf`.
