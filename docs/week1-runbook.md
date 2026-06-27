# Неделя 1 — Runbook для агента-исполнителя

> Этот документ — пошаговая инструкция для **другого агента** (или человека с
> железом), который выполняет Неделю 1 из [ROADMAP_MONTH1.md](./ROADMAP_MONTH1.md).
> Каждый шаг: что делать → точные команды → ожидаемый результат → куда положить
> артефакт → критерий готовности (DoD).
>
> **Где выполняется:** реальный Hackintosh под OpenCore с RTX 4070 Super
> (`10DE:2783`), macOS. Шаги сборки kext/dext — на той же или другой машине с Xcode.
>
> **Правило агента:** не отмечай задачу `[x]` в ROADMAP, пока не выполнен её DoD и
> артефакт не закоммичен. Если шаг не получается — фиксируй ошибку в разделе
> «Журнал блокеров» внизу и переходи к следующему независимому шагу.

---

## Результат недели (Definition of Done)

1. Зафиксирован таргет в `docs/testbed.md` (готовый шаблон — заполнить реальными данными).
2. `pcie_probe` собран и запущен на реальном железе, лог лежит в `docs/hw-dumps/`.
3. Из лога выписаны BAR0/BAR1/BAR3, subsystem-id, revision-id, class-code.
4. Драйверы **RTXProbe (kext) и RTXProbeDext (dext)** собираются без ошибок (лог сборки приложен).
5. `docs/testbed.md` заполнен: версия macOS, конфиг OpenCore, статус SIP.

Когда все 5 закрыты — Неделя 1 готова, отметить чек-боксы в ROADMAP_MONTH1.md.

---

## Предусловия (проверить до начала)

```sh
sw_vers                       # версия macOS
clang --version               # должен быть установлен (Command Line Tools)
xcodebuild -version           # нужен полный Xcode, не только CLT
git --version
```

Если `xcodebuild` ругается «requires Xcode» — поставить полный Xcode из App Store
и выполнить:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
sudo xcodebuild -license accept
```

---

## Шаг 1. Зафиксировать таргет

**Действие:** подтвердить, что работаем по сценарию из [ARCHITECTURE.md](./ARCHITECTURE.md):
x86 Hackintosh под OpenCore, RTX 4070 Super как основная карта.

**Что сделать:**
- Открыть `docs/testbed.md`, заполнить блок «Таргет».
- Убедиться, что карта физически основная (монитор подключён к ней или к iGPU —
  зафиксировать как именно).

**DoD:** блок «Таргет» в `docs/testbed.md` заполнен.

---

## Шаг 2. Собрать и запустить `pcie_probe`

**Действие:** собрать пробу из исходника в корне репозитория.

```sh
# из корня репозитория
make probe          # см. Makefile; эквивалент команды ниже
# либо напрямую:
clang pcie_probe.c -framework IOKit -framework CoreFoundation -o pcie_probe
./pcie_probe
```

**Ожидаемый результат:** вывод вида
```
[#1] IORegistry node: GFX0
  vendor-id : 0x10DE (NVIDIA)
  device-id : 0x2783  -> AD104 (RTX 4070 Super)
  ...
  --- BAR regions ---
  BAR0 (cfg 0x10) MEM ...
```

**Если NVIDIA-устройств не найдено** → записать в «Журнал блокеров»: проверить,
что карта видна в IORegistryExplorer, что EFI назначил ресурсы, что мы в macOS.

**DoD:** проба отработала, на экране видны device-id 0x2783 и BAR-регионы.

---

## Шаг 3. Снять лог железа в `docs/hw-dumps/`

**Действие:** сохранить полный вывод пробы + сырой config space для проверки.

```sh
# основной лог пробы
./pcie_probe | tee "docs/hw-dumps/$(date +%Y%m%d)-rtx4070s-pcie_probe.log"

# (опционально, для перекрёстной проверки) дамп из системного профайлера
system_profiler SPDisplaysDataType > "docs/hw-dumps/$(date +%Y%m%d)-SPDisplays.txt"
```

Затем выписать ключевые значения в шапку лога или в `docs/hw-dumps/README.md`
(таблица): **BAR0, BAR1, BAR3, subsystem-id, revision-id, class-code**.

Шаблон оформления — `docs/hw-dumps/README.md`.

**DoD:** в `docs/hw-dumps/` лежит лог с реальными адресами BAR и идентификаторами,
ключевые значения выписаны в таблицу.

---

## Шаг 4. Собрать kext-драйвер RTXProbe

**Действие:** собрать реальный kext из `driver/RTXProbe/` (матчинг + маппинг BAR0 +
чтение `PMC_BOOT_0`). Это уже не пустышка — это драйвер слоя 1.

```sh
cd driver/RTXProbe
xcodebuild -project RTXProbe.xcodeproj -scheme RTXProbe -configuration Debug build \
  | tee ../../docs/hw-dumps/build-rtxprobe-kext.log
```

Если Xcode-проекта ещё нет — создать таргет Kernel Extension, подключить
`RTXProbe.cpp/.hpp` + `Info.plist`. Детали — `driver/README.md`.

**DoD:** `BUILD SUCCEEDED`, лог приложен. Загрузка через OpenCore и проверка лога —
это Неделя 4.

---

## Шаг 5. Собрать dext-драйвер RTXProbeDext

**Действие:** собрать DriverKit-вариант из `driver/RTXProbeDext/` (альтернативный
канал, то же чтение через PCIDriverKit).

```sh
cd driver/RTXProbeDext
xcodebuild -project RTXProbeDext.xcodeproj -scheme RTXProbeDext -configuration Debug build \
  | tee ../../docs/hw-dumps/build-rtxprobe-dext.log
```

Проверить наличие DriverKit SDK: `xcodebuild -showsdks | grep -i driverkit`.

**DoD:** `BUILD SUCCEEDED`, лог приложен.

---

## Шаг 6. Заполнить стенд `docs/testbed.md`

**Действие:** собрать данные об окружении и занести в шаблон.

```sh
sw_vers                                              # ProductVersion / Build
csrutil status                                       # статус SIP
nvram csr-active-config 2>/dev/null                  # битовая маска SIP (OpenCore)
nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:nvda_drv  # (если есть) флаг nvidia
uname -a
```

Заполнить в `docs/testbed.md`: версия/билд macOS, версия OpenCore, ключевые
квирки/кексты (Lilu/WhateverGreen и т.п.), значение `csr-active-config` и его
расшифровку, путь к `config.plist` (без секретов).

**DoD:** `docs/testbed.md` заполнен реальными данными, без плейсхолдеров `<...>`.

---

## Журнал блокеров

> Сюда агент записывает всё, что не сработало, с точной ошибкой и гипотезой.
> Формат: дата — шаг — симптом — что проверено — статус.

- _(пусто — заполняется по ходу)_

---

## Чек-лист закрытия недели

- [ ] Шаг 1: таргет зафиксирован
- [ ] Шаг 2: `pcie_probe` собран и запущен
- [ ] Шаг 3: лог железа в `docs/hw-dumps/` + таблица BAR/ID
- [ ] Шаг 4: kext RTXProbe собирается (`BUILD SUCCEEDED`)
- [ ] Шаг 5: dext RTXProbeDext собирается (`BUILD SUCCEEDED`)
- [ ] Шаг 6: `docs/testbed.md` заполнен
- [ ] Чек-боксы Недели 1 в `ROADMAP_MONTH1.md` отмечены
