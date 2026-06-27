# HW-дамп: PMC_BOOT_0 с RTX 4070 Super (Windows / RW-Everything)

**Дата:** 2026-06-28
**Стенд:** i5-12400F, Gigabyte B760M DS3H DDR4, RTX 4070 Super (AD104, `10DE:2783`), Windows 11.
**Метод:** RW-Everything (kernel-MMIO), чтение физической памяти BAR0.

## Что прочитано

- **BAR0 (регистры MMIO):** `0x52000000` (подтверждён из Windows PnP).
- **Адрес чтения:** `0x52000000 + 0x0` = `NV_PMC_BOOT_0`.
- **Сырые байты (offset 0x00, little-endian):** `A1 00 40 19`
- **NV_PMC_BOOT_0 = `0x194000A1`**

Вокруг — осмысленные значения регистров (`40 50 DF BA …`), не `0xFF`, что
подтверждает: читается реальный регистровый блок карты.

## Декод (ada_regs.h)

| Поле | Вычисление | Значение |
|---|---|---|
| architecture | `(0x194000A1 >> 24) & 0x1F` | `0x19` → **Ada Lovelace** |
| chipset | `(>>20) & 0x1FF` | `0x194` → **AD104** |
| implementation | `(>>20) & 0xF` | `0x4` |
| major revision | `(>>4) & 0xF` | `0xA` |
| minor revision | `& 0xF` | `0x1` |

→ **Ada AD104, chipset 0x194, revision 0xA1.**

## Перекрёстная сверка

- `chipset 0x194` = ровно то, что предсказывал декодер по nova-core (AD104).
- `revision 0xA1` совпадает с `revision-id = 0xA1`, независимо снятым из Windows PnP
  (`PCI\VEN_10DE&DEV_2783&...&REV_A1`).

## Статус

🟢 **Подтверждено на железе.** Декодер `NV_PMC_BOOT_0` (ada_regs.h) и адрес BAR0
верны на реальной RTX 4070 Super. Закрывает декод-часть Requirement 2 (R2.2)
по факту, а не по сверке с исходником.

Не закрывает: загрузку kext на macOS (нужен стенд) — это про то, что НАШ драйвер
прочитает то же значение в среде macOS.
