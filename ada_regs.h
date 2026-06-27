/*
 * ada_regs.h — определения регистров NVIDIA Ada (AD10x) для bring-up.
 *
 * Используется и user-space пробой (pcie_probe.c), и драйверами (kext/dext).
 * Только C, без зависимостей кроме stdint — чтобы инклюдиться где угодно,
 * включая kernel-space C++.
 *
 * Источник раскладки полей — NVIDIA open-gpu-kernel-modules (dev_boot.h).
 * Содержимое пересказано/сведено для соответствия лицензии. Точные значения
 * сверяются на реальном железе чтением PMC_BOOT_0.
 */
#ifndef RTXMACOC_ADA_REGS_H
#define RTXMACOC_ADA_REGS_H

#include <stdint.h>

/*
 * NV_PMC_BOOT_0 — первый регистр, offset 0x0 в BAR0 (MMIO-блок). Возвращает
 * идентификатор чипа: architecture + implementation + revision. Первое, что
 * читает любой драйвер. Ненулевое осмысленное значение = мы реально говорим
 * с регистровым блоком карты. Чтение этого регистра — метрика успеха месяца 1.
 */
#define NV_PMC_BOOT_0 0x00000000u

/*
 * Раскладка полей NV_PMC_BOOT_0 (классическая, Tesla..Ada).
 *   [28:24] ARCHITECTURE   — семейство (Ada = 0x19)
 *   [23:20] IMPLEMENTATION — конкретный чип в семействе (AD104 → 0x4)
 *   [7:4]   MAJOR_REVISION
 *   [3:0]   MINOR_REVISION
 *
 * Для Ampere+ есть ещё NV_PMC_BOOT_42 с расширенным полем architecture —
 * пригодится для точной классификации позже. Для идентификации «это Ada»
 * PMC_BOOT_0 достаточно.
 */
#define NV_PMC_BOOT_0_MINOR_REVISION_SHIFT   0u
#define NV_PMC_BOOT_0_MINOR_REVISION_MASK    0xFu
#define NV_PMC_BOOT_0_MAJOR_REVISION_SHIFT   4u
#define NV_PMC_BOOT_0_MAJOR_REVISION_MASK    0xFu
#define NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT   20u
#define NV_PMC_BOOT_0_IMPLEMENTATION_MASK    0xFu
#define NV_PMC_BOOT_0_ARCHITECTURE_SHIFT     24u
#define NV_PMC_BOOT_0_ARCHITECTURE_MASK      0x1Fu

/* Известные значения поля ARCHITECTURE. */
#define NV_ARCHITECTURE_TU 0x16u /* Turing  */
#define NV_ARCHITECTURE_GA 0x17u /* Ampere  */
#define NV_ARCHITECTURE_AD 0x19u /* Ada Lovelace — наша цель */

/*
 * "chipset id" = (boot0 >> 20) & 0x1ff — 9-битное значение arch+impl, ровно как
 * декодит драйвер Linux nova-core. Подтверждено публично:
 *   - nova-core: значение 0x192 из регистра = чип AD102
 *     (docs.kernel.org/gpu/nova/core/todo.html);
 *   - пример декода nova-core «0x124a1002 -> architecture 0x12, implementation 0x4»
 *     подтверждает раскладку полей arch[28:24] / impl[23:20].
 * Содержимое источников пересказано для соответствия лицензии.
 *
 * Отсюда ряд Ada: AD102=0x192, AD103=0x193, AD104=0x194, AD106=0x196, AD107=0x197.
 * Наша карта (RTX 4070 Super, device 0x2783) — AD104 → ожидаем chipset 0x194.
 *
 * Примечание: для Ada достаточно BOOT_0 (offset 0x0). Будущие чипы (Rubin+) NVIDIA
 * переводит на NV_PMC_BOOT_42 — учтём, когда дойдём.
 */
#define NV_CHIPSET_AD102 0x192u
#define NV_CHIPSET_AD103 0x193u
#define NV_CHIPSET_AD104 0x194u /* RTX 4070 Super / 4070 Ti */
#define NV_CHIPSET_AD106 0x196u
#define NV_CHIPSET_AD107 0x197u

typedef struct {
    uint32_t raw;
    uint32_t architecture;
    uint32_t implementation;
    uint32_t chipset;        /* (raw >> 20) & 0x1ff — как в nova-core */
    uint32_t major_revision;
    uint32_t minor_revision;
} nv_boot0_t;

static inline nv_boot0_t nv_decode_boot0(uint32_t v)
{
    nv_boot0_t b;
    b.raw            = v;
    b.architecture   = (v >> NV_PMC_BOOT_0_ARCHITECTURE_SHIFT)   & NV_PMC_BOOT_0_ARCHITECTURE_MASK;
    b.implementation = (v >> NV_PMC_BOOT_0_IMPLEMENTATION_SHIFT) & NV_PMC_BOOT_0_IMPLEMENTATION_MASK;
    b.chipset        = (v >> 20) & 0x1FFu;
    b.major_revision = (v >> NV_PMC_BOOT_0_MAJOR_REVISION_SHIFT) & NV_PMC_BOOT_0_MAJOR_REVISION_MASK;
    b.minor_revision = (v >> NV_PMC_BOOT_0_MINOR_REVISION_SHIFT) & NV_PMC_BOOT_0_MINOR_REVISION_MASK;
    return b;
}

/* Человекочитаемое имя семейства по полю architecture. */
static inline const char *nv_arch_name(uint32_t architecture)
{
    switch (architecture) {
        case NV_ARCHITECTURE_TU: return "Turing (TU10x)";
        case NV_ARCHITECTURE_GA: return "Ampere (GA10x)";
        case NV_ARCHITECTURE_AD: return "Ada Lovelace (AD10x)";
        default:                 return "unknown arch";
    }
}

/* Человекочитаемое имя конкретного чипа по chipset id. */
static inline const char *nv_chipset_name(uint32_t chipset)
{
    switch (chipset) {
        case NV_CHIPSET_AD102: return "AD102";
        case NV_CHIPSET_AD103: return "AD103";
        case NV_CHIPSET_AD104: return "AD104";
        case NV_CHIPSET_AD106: return "AD106";
        case NV_CHIPSET_AD107: return "AD107";
        default:               return "unknown chipset";
    }
}

/*
 * Признак, что значение PMC_BOOT_0 выглядит правдоподобно (не 0x0 и не
 * 0xFFFFFFFF — типичные значения «маппинг не сработал / питание выключено»).
 */
static inline int nv_boot0_plausible(uint32_t v)
{
    return v != 0x00000000u && v != 0xFFFFFFFFu;
}

#endif /* RTXMACOC_ADA_REGS_H */
