English: [TESTING.en.md](TESTING.en.md)

# Turnip A5xx: тесты и проверка

Драйвер: `src/freedreno/vulkan/tu5_*` + правки в общих путях Turnip.
Рабочий тег: `a5xx-vkcube-working` (`af1f612`).

## Сборка тестов

Нужен кросс-компилятор и rootfs с libvulkan/wayland:

```sh
cd a5xx/tests
CROSS=/path/to/cross/bin/aarch64-alarm-gcc \
ROOTFS=/path/to/rootfs \
./build-all.sh
```

`build-all.sh` собирает все `tu5_*` тесты; `tu5_wl_wsi_test` дополнительно
требует `xdg-shell-protocol.c` и `libwayland-client` из rootfs.

## Запуск на устройстве (системный ICD)

```sh
unset VK_ICD_FILENAMES VK_DRIVER_FILES
cd /tmp/tu5-test
for t in tu5_test tu5_ubo_test tu5_array_test \
         tu5_tri_test tu5_small_test tu5_fsprobe_test tu5_vb_test \
         tu5_vspos_test tu5_vscount_test tu5_vsprobe_test \
         tu5_point_test tu5_pointprobe_test tu5_line_test \
         tu5_clear_test tu5_cleardraw_test tu5_bigclear_test \
         tu5_stencil_test tu5_mrt_test tu5_uvvb_test \
         tu5_texcopy_test tu5_imgops_test tu5_bufops_test \
         tu5_clearcopy_test tu5_texvb_test; do
  timeout 30 ./$t >/tmp/o 2>&1
  echo "$t rc=$? $(tail -1 /tmp/o)"
done
for a in "" d24s8; do timeout 30 ./tu5_depth_test $a; done
timeout 30 ./tu5_blend_test
timeout 30 ./tu5_blend_alpha_test
```

Ожидаемо: все PASS (на момент тега — 29/29, включая WSI).
`tu5_imgops_test` и `tu5_clearcopy_test` — texture clear/copy path,
иногда нестабильны на WIP-текстурах; при hang нужен power-off.

## WSI / vkcube

```sh
# headless WSI
./tu5_wsi_test

# реальный Wayland (нужен активный сеанс Plasma)
WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1001 \
  vkcube --wsi wayland --c 60
```

Ожидаемо: куб LUNAR G с текстурой и depth, rc=0.
`TU5_GFX_NODEPTH=1` — диагностический обход depth (для отладки).

## Установка/откат драйвера

```sh
# stable / ops / rollback
DRIVER=build-a5xx/src/freedreno/vulkan/libvulkan_freedreno.so \
  ./a5xx/install-turnip-a5xx.sh user@host
./a5xx/install-turnip-a5xx.sh user@host --rollback
```

Бэкапы: `$HOME/turnip-a5xx-backup-*`.

## Ограничения/известные баги

- LINEAR-сэмплер на app-mapped образе иногда даёт нули (WIP).
- HW index fetch в sysmem/bypass не работает — indexed draw разворачивается
  на CPU; ограничение: одна instance, без экзотических форматов.
- KWin ScreenShot2 + Vulkan-окно может подвесить GPU; для скриншотов
  использовать осторожно, не в автоматическом цикле.
- GPU hang = hardlockup, помогает только полный power-off.

## Следующий этап: стабильность/скорость/сон

1. 24–72h loop тестов + `vkcube`, журнал `dmesg`/`journalctl` на GPU fault,
   hardlockup, panic, ath10k/ADSP; pstore/ramoops.
2. Скорость: fillrate/texture/compute бенчмарки, частота GPU (fused 430 MHz),
   температура/батарея.
3. Артефакты: скриншоты/readback для форматов, depth, blend, MRT, текстур.
4. Deep sleep: ADSP `remoteproc` crash/recovery, `rproc_crash_handler_work`,
   cpuidle workaround; `echo mem` + RTC wake, без поломки сеанса.

- `tests/stress.sh` — reproducible stress loop with a disk log
  (`MODE=safe|full`, `VKCUBE=1`).
- `tests/collect-logs.sh` — dmesg/journal/pstore collector.
- `tests/bench.sh` — simple timing benchmark (bigclear, texcopy, UBO, compute,
  vkcube 300).
