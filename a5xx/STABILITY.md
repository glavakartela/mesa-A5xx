English: [STABILITY.en.md](STABILITY.en.md)

# Стабильность A5xx: журнал прогонов

## Прогон 1 (17:35, safe + texcopy + vkcube60, 20 мин)

- Устройство было запущено, системный ICD `git-a73cf2af75`, Plasma жива.
- Smoke до стресса: 27/27 PASS, кроме `tu5_texvb_test` (app-mapped WIP);
  `vkcube --c 60` rc=0.
- Запущен цикл `safe + tu5_texcopy_test + vkcube60` на 20 минут с логом
  `$HOME/stress-173537.log` (на устройстве, `sync` после
  каждой строки).
- Примерно через несколько минут устройство пропало с сети (`.73` dead,
  в скане только старый `.39`). Это либо GPU hardlock, либо ath10k/Wi-Fi.
- **Лог сохранился на диске.** После power-off/on нужно посмотреть
  последний `BEGIN/END` — он покажет тест-триггер.

## План биссекции после включения

1. Прочитать `stress-173537.log`:
   - последний `BEGIN tu5_texcopy_test`/`vkcube60` → подозрение на
     texture/WSI;
   - лог дошёл до `DONE` или последний `iter done` → скорее Wi-Fi/ath10k,
     а не GPU;
   - `STOP`/`timeout` → тест сам вернул ошибку.
2. Запустить `MODE=safe DURATION=1800` (без текстур и vkcube) — база.
3. Если safe стабилен, добавить `tu5_texcopy_test` (MODE=full).
4. Если full стабилен, добавить `VKCUBE=1`.
5. На каждом прогоне держать root-наблюдатель:
   `sudo sh -c 'dmesg -w > /var/log/a5xx-kmsg.log' &`
   и собирать `journalctl -k -b -1`, `/sys/fs/pstore` после ребута.
6. Фиксировать `gpuclk`, температуру, число итераций, rc.

## Инструменты

- `tests/stress.sh` — цикл с логом на диск (`MODE=safe|full`, `VKCUBE=1`).
- `tests/collect-logs.sh` — `dmesg`, journal текущей/прошлой загрузки,
  pstore.
- `tests/check-health` нет; health собирается collect-logs.

## Гипотезы по первому hang

- **Текстурный путь** (`tu5_texcopy_test`, vkcube): WIP TPL1/UCHE;
  повторные процессы могли накопить состояние.
- **WSI/KWin**: повторные swapchain/present, dmabuf import/export.
- **ath10k**: устройство часто теряло сеть; если лог дошёл до конца —
  это Wi-Fi, не GPU.
- **Питание/перегрев**: смотреть temp/батарею в логе.


## Прогон 2 (18:45, контролируемый)

- Свежая загрузка, `bigclear -> texcopy`: `bigclear` PASS, `texcopy` вернул
  **rc=1 (нули)**, не hang. Повторный запуск `texcopy`/`texvb` — устройство
  ушло в hang без вывода. Значит texture path (staging -> image -> sampler)
  флейки: иногда нули, иногда hang; vkcube при этом рисует куб.
- Kernel-лог прошлой загрузки: `a5xx_irq gpu fault ring 0 status 800001C1`,
  `hangcheck recover`, `msm_dpu CP opcode error`; отдельно
  `ath10k_snoc ... failed to create WMI vdev 0: -108`.
- Wi-Fi был на 5 ГГц `KB_36_5G`, сигнал -87..-92 dBm; power save выключен.
  Доступны 2.4 ГГц `KB_36_EXT` (79) и `KB_36` (67).
- Подготовлен `texfix`-драйвер: полный flush image BO после CPU-copy,
  `TU5_GFX_TEXWAIT` и отладочный вывод TPL1-дескрипторов.
- Дальше: прогнать `texcopy-stress.sh` с `texfix` и `TU5_GFX_DEBUG=1`,
  сравнить дескрипторы/адреса; при hang — последний лог на диске.

## KWin ушёл на Turnip Vulkan (19:30)

- В env-файлах KWin остался только `KWIN_USE_BUFFER_AGE=0` (плюс строка
  пробелов); `KWIN_DISABLE_VULKAN=1` потерялся.
- KWin 6.8 greeter выбрал Vulkan: лог `Physical device Turnip Adreno 509
  doesn't support host query resets`; при texture/rotation — GPU fault
  `iova=0000000000000000`, `vblank wait timed out`, kernel trace, reboot.
- Исправлено: оба env-файла перезаписаны (`KWIN_USE_BUFFER_AGE=0`,
  `KWIN_DISABLE_VULKAN=1`), `user@962/1001` перезапущены; greeter KWin
  теперь OpenGL, Turnip не использует.
- Одиночный `tu5_texcopy_test` с texflush/ptrlog (KWin остановлен) —
  PASS; указатели ненулевые. Дальше — headless stress без KWin и проверка
  артефактов OpenGL.

## OpenGL-артефакты Plasma (19:45+)

- Артефакты KWin/Plasma были на **OpenGL** (system Mesa `msm_dri`, fd5),
  не на Turnip: белые полоски/«кирпичики», текст/поворот, лаг шторки.
- В системе стоял workaround Mesa issue 8442: `FD_MESA_DEBUG=noblit`
  (`/etc/profile.d/sdm660-adreno-quirks.sh`). Он ломает blit/UI.
- Заменил на `FD_MESA_DEBUG=sysmem` (blit остаётся, GMEM/tiling off); KWin
  greeter/user больше не даёт `a5xx_irq`/vblank fault.
- Дополнительно: `KWIN_DRM_NO_DIRECT_SCANOUT=1`,
  `KWIN_DRM_USE_MODIFIERS=0`, blur отключён в `kwinrc`.
- Проверка: одиночный `tu5_texcopy_test` (KWin off) PASS; указатели
  ненулевые. Открытый вопрос — остаточные белые полоски/«кирпичики»:
  проверить с `sysmem` + no-scanout; если остаются, тестировать
  `noblit,inorder,gmem` или собирать/обновлять fd5 GL.

## Прогон 3: vkmark (2026-11-22)

- `samplerAnisotropy` для chip 5 (`43823af`) позволил vkmark создать
  Vulkan-устройство.
- Сцены shading, texture, effect2d, desktop и cube проходят.
- Двухминутный стресс: без reset/fault, температура ~49–53 °C.
- Отдельно проверен kernel progress-aware fix (`7.2-whyred-drivers`):
  длинный непрерываемый draw не вызывает ложный reset; на реальном hang
  ALU останавливается и watchdog срабатывает.
