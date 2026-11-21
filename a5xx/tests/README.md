# Turnip a5xx compute smoke test

Проверка изолированного compute-пути Turnip для Adreno 509.
Один storage buffer (set 0, binding 0), прямой dispatch, host readback.

```sh
./build.sh
./run-on-device.sh user@host
# PORT=22 по умолчанию, можно переопределить: PORT=24 ./run-on-device.sh ...
```

Скрипт запуска копирует собранный `libvulkan_freedreno.so` и тест в
`/tmp/tu5-test` на телефоне и подсовывает их через `VK_ICD_FILENAMES`,
не трогая установленный драйвер.

Ожидаемый результат:

```
device: Turnip Adreno (TM) 509 (api 1.3.368)
debug: host[0]=0x00000001 host[511]=0x000005fe
PASS: 512/512 values correct
```

Проверка indirect dispatch:

```sh
ssh ... 'VK_ICD_FILENAMES=/tmp/tu5-test/freedreno_a5xx.json /tmp/tu5-test/tu5_test indirect'
```

Ограничения MVP: только descriptor set 0, нет UBO, изображений, push
constants и массивов дескрипторов. Одиночный SSBO можно вешать на любой
binding (проверено на binding 1); `.length()` работает.
