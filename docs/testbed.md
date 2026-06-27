# Тестовый стенд — RTXmacOC

> Шаблон. Агент/исполнитель заполняет реальными данными своей машины и коммитит.
> Один стенд = один блок. Если стендов несколько — копировать блок «Стенд №N».
> Не оставлять плейсхолдеры `<...>` в закрытой задаче.

---

## Таргет (зафиксировано)

- **Платформа:** x86 Hackintosh под OpenCore
- **GPU (основная карта):** NVIDIA RTX 4070 Super, Ada AD104, PCI ID `10DE:2783`
- **Роль карты:** основная и **единственная** видеокарта (CPU i5-12400**F** — без iGPU)
- **Куда подключён монитор:** RTX 4070 Super (другого видеовыхода нет)

> ⚠️ **Важно для Hackintosh:** i5-12400F не имеет встроенной графики. Значит нет
> резервного iGPU для вывода картинки и нет «headless iGPU» трюка на время bring-up.
> Пока NVIDIA-драйвер не выводит изображение, для установки/настройки macOS
> понадобится либо временная дешёвая GPU с поддержкой (AMD/совместимая), либо
> работа вслепую/по SSH. Зафиксировать как риск месяца.

---

## Снято из Windows (до Мака) — реальные данные карты

> Получено на этом же ПК через `Get-PnpDevice` (PowerShell). Это те же поля, что
> `pcie_probe` прочитает на macOS. После прогона пробы — сверить, что совпало.

| Поле | Значение (Windows) | Расшифровка |
|---|---|---|
| Hardware ID | `PCI\VEN_10DE&DEV_2783&SUBSYS_F3021569&REV_A1` | — |
| vendor-id | `0x10DE` | NVIDIA |
| device-id | `0x2783` | **AD104 — RTX 4070 Super** (целевой чип ✓) |
| subsystem-id | `0xF302` | ID платы |
| subsystem-vendor | `0x1569` | Palit Microsystems |
| revision-id | `0xA1` | ревизия кремния |
| class-code | `0x030000` | VGA display controller ✓ |
| FriendlyName | `NVIDIA GeForce RTX 4070 SUPER` | — |

**Что сверить после `pcie_probe` на macOS:** device-id, subsystem-id (`0xF302`),
subsystem-vendor (`0x1569`), revision-id (`0xA1`), class-code (`0x030000`). BAR-адреса
из Windows не берём — их назначает EFI/macOS, снимем уже пробой.

---

## Стенд №1

### Железо

| Поле | Значение |
|---|---|
| CPU | 12th Gen Intel Core i5-12400F (без iGPU) |
| Чипсет/мать | Gigabyte B760M DS3H DDR4 |
| iGPU (есть ли) | нет (суффикс F) |
| RTX-карта (вендор платы) | Palit (subsystem-vendor `0x1569`) |
| subsystem-id (из pcie_probe) | `0xF302` (снято из Windows, подтвердить пробой) |
| RAM | 32 GB |

### macOS

| Поле | Значение | Как получить |
|---|---|---|
| ProductVersion | `<напр. 14.5>` | `sw_vers` |
| Build | `<напр. 23F79>` | `sw_vers` |
| Kernel | `<uname -a>` | `uname -a` |

### OpenCore

| Поле | Значение |
|---|---|
| Версия OpenCore | `<напр. 1.0.1>` |
| Путь к config.plist | `<EFI/OC/config.plist>` |
| Ключевые kext (Lilu, WhateverGreen, ...) | `<список + версии>` |
| Значимые квирки (Booter/Kernel) | `<список>` |
| Спуф GPU / device-properties для GFX0 | `<есть/нет, что именно>` |

### SIP

| Поле | Значение | Как получить |
|---|---|---|
| `csrutil status` | `<enabled/disabled/custom>` | `csrutil status` |
| `csr-active-config` | `<0x...>` | `nvram csr-active-config` |
| Расшифровка маски | `<какие биты сняты>` | см. примечание ниже |
| Разрешена загрузка kext? | `<да/нет>` | следствие маски |

> **Примечание по SIP-маске.** `csr-active-config` хранится little-endian.
> Для загрузки сторонних kext обычно нужен снятый бит `CSR_ALLOW_UNTRUSTED_KEXTS`
> (0x1) и часто `CSR_ALLOW_UNRESTRICTED_FS` (0x2). Точную раскладку битов
> сверять с актуальной версией macOS — занести вывод как есть + расшифровку.

### Статус сборки тулчейна

| Артефакт | Статус | Лог |
|---|---|---|
| `clang --version` | `<ok/нет>` | — |
| `xcodebuild -version` | `<версия>` | — |
| Пустой kext собирается | `<BUILD SUCCEEDED / ошибка>` | `docs/hw-dumps/build-rtxprobe-kext.log` |
| Пустой dext собирается | `<BUILD SUCCEEDED / ошибка>` | `docs/hw-dumps/build-rtxprobe-dext.log` |

### Заметки

`<что нестандартного на этом стенде, известные проблемы, особенности загрузки>`
