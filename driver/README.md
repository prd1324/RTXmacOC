# driver/ — реальные драйверы RTXmacOC (слой 1: PCIe bring-up)

Два драйвера делают одно и то же — матчатся на RTX 4070 Super (`10DE:2783`),
мапят BAR0, читают `NV_PMC_BOOT_0` и печатают chip id Ada. Это метрика месяца 1.

- **`RTXProbe/`** — kext (основной канал, инжект через OpenCore). `IOPCIDevice` →
  `mapDeviceMemoryWithRegister(BAR0)` → читаем offset 0x0.
- **`RTXProbeDext/`** — dext (альтернатива, PCIDriverKit). То же через `MemoryRead32`.

Общий декодер регистра — `../ada_regs.h`.

## Сборка

Оба собираются в Xcode (нужен полный Xcode). Создать соответствующий таргет
(Kernel Extension / DriverKit Driver), подключить исходники из папки, затем:

```sh
# kext
xcodebuild -project RTXProbe.xcodeproj -scheme RTXProbe -configuration Debug build
# dext
xcodebuild -project RTXProbeDext.xcodeproj -scheme RTXProbeDext -configuration Debug build
```

## Загрузка и проверка (на железе)

**kext через OpenCore** (рекомендовано на Hackintosh):
1. Релакс SIP: `csr-active-config` в NVRAM (снять `CSR_ALLOW_UNTRUSTED_KEXTS`).
2. Положить `RTXProbe.kext` в `EFI/OC/Kexts`, добавить в `config.plist` → `Kernel/Add`.
3. Загрузиться, смотреть лог:
   ```sh
   log show --last 2m --predicate 'eventMessage CONTAINS "RTXProbe"'
   ```

**dext** (для разработки/тестов): требует Apple developer-провижининга под
`com.apple.developer.driverkit*`, либо dev-режима DriverKit. Лог:
```sh
log show --last 2m --predicate 'eventMessage CONTAINS "RTXProbeDext"'
```

## Что ждём в логе

```
RTXProbe: matched 10de:2783
RTXProbe: BAR0 mapped at kv=0x... len=0x...
RTXProbe: NV_PMC_BOOT_0 = 0x........
RTXProbe: chip arch=0x19 impl=0x4 rev=... -> Ada Lovelace (AD10x)
RTXProbe: *** ВИЖУ Ada AD104 — стена пробита ***
```

`arch=0x19` (Ada) + ненулевой `PMC_BOOT_0` = слой 1 пройден.

## Статус полей (честно)

Раскладка полей `PMC_BOOT_0` в `ada_regs.h` взята из публичной документации/
open-gpu-kernel-modules и **подлежит подтверждению на железе**. Если `arch`
выйдет не `0x19` — сверить shift/mask с `dev_boot.h` из open-gpu-kernel-modules
(для Ampere+ возможна потребность в `NV_PMC_BOOT_42`). Логика маппинга и чтения
от этого не меняется.
