# hw-dumps — реальные дампы железа

Сюда складываются **реальные** логи с Hackintosh: вывод `pcie_probe`, дампы
system_profiler, логи сборки kext/dext. Это доказательная база Недели 1.

## Соглашение об именах

```
YYYYMMDD-<карта>-<что>.log
```

Примеры:
- `20260627-rtx4070s-pcie_probe.log` — основной вывод пробы
- `20260627-SPDisplays.txt`         — system_profiler SPDisplaysDataType
- `build-emptykext.log`             — лог сборки пустого kext
- `build-emptydext.log`             — лог сборки пустого dext

## Сводная таблица (заполнять из логов)

> Выписать ключевые значения из последнего дампа `pcie_probe`. Это то, на что
> будут опираться Недели 3–4 (маппинг BAR0, чтение `PMC_BOOT_0`).
>
> Идентификаторы предзаполнены по данным из Windows (`Get-PnpDevice`, см.
> `docs/testbed.md`). После прогона `pcie_probe` на macOS — подтвердить совпадение
> и дописать BAR-адреса.

| Поле | Значение | Источник (файл) |
|---|---|---|
| device-id | `0x2783` (AD104, RTX 4070 Super) — _подтвердить пробой_ | Windows PnP |
| subsystem-id | `0xF302` — _подтвердить пробой_ | Windows PnP |
| subsystem-vendor | `0x1569` (Palit) — _подтвердить пробой_ | Windows PnP |
| revision-id | `0xA1` — _подтвердить пробой_ | Windows PnP |
| class-code | `0x030000` — _подтвердить пробой_ | Windows PnP |
| **BAR0** (регистры MMIO) | base `0x52000000` size `0x1000000` (16 MB) | Windows Win32_PnPAllocatedResource |
| **BAR1** (VRAM aperture) | base `0x40000000` size `0x80000000` (2 GB) | Windows Win32_PnPAllocatedResource |
| **BAR3** | base `0x50000000` size `0x2000000` (32 MB) | Windows Win32_PnPAllocatedResource |

> ⚠️ Адреса BAR назначены **Windows/UEFI**. На macOS базовые адреса будут другими
> (их назначит macOS/OpenCore) — снять заново через `pcie_probe`. Размеры BAR
> (16 MB / 2 GB / 32 MB) и **offset PMC_BOOT_0 (0x0 в BAR0)** не зависят от ОС.
>
> **PMC_BOOT_0 на этой Windows-машине = DWORD по физ. адресу `0x52000000`** —
> читается через RW-Everything (Memory → 0x52000000 → Read DWORD). См.
> `docs/read-pmc-boot0-windows.md`.

## Интерпретация (ожидания, подтвердить по факту)

- **BAR0** — самый функционально важный регион: регистровый блок (MMIO). Через
  него на Неделе 4 будем читать `PMC_BOOT_0` (offset 0x0). На Ada обычно ~16 MB.
- **BAR1** — апертура VRAM, крупный регион (зависит от объёма памяти / resizable BAR).
- **BAR3** — вторичный регион; роль уточнить чтением сигнатурных регистров (Шаг 2 дорожной карты).

Точные размеры/роли фиксируются по реальному выводу, а не по этим ожиданиям.
