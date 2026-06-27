/*
 * ada_regs.h — определения регистров NVIDIA Ada (AD10x), которые нужны
 * на ранних шагах bring-up. Заполняется по мере разведки.
 *
 * ВАЖНО: точные битовые поля сверяются с NVIDIA open-gpu-kernel-modules
 * (заголовки вида dev_*.h, hw ref). Пока что-то помечено TODO — не считать
 * это истиной, проверять на реальном железе.
 */
#ifndef RTXMACOC_ADA_REGS_H
#define RTXMACOC_ADA_REGS_H

#include <stdint.h>

/*
 * NV_PMC_BOOT_0 — самый первый регистр, по смещению 0x0 в BAR0 (регистровый
 * MMIO-блок). Возвращает идентификатор чипа: architecture + implementation +
 * revision. Это первое, что читает любой драйвер, чтобы понять, с каким GPU
 * он говорит. Чтение этого регистра из нашего dext — метрика успеха месяца 1.
 */
#define NV_PMC_BOOT_0 0x00000000u

/*
 * Декодирование NV_PMC_BOOT_0.
 *
 * TODO(week3): сверить точные маски/сдвиги полей ARCHITECTURE / IMPLEMENTATION
 * с open-gpu-kernel-modules (NVIDIA) для Ada. Ниже — историческая раскладка
 * (envytools/nouveau), которую НУЖНО подтвердить для AD10x перед тем, как
 * полагаться на неё в логике драйвера.
 *
 * Известно достоверно: регистр лежит по offset 0x0; ненулевое осмысленное
 * значение уже подтверждает, что мы реально читаем регистровый блок карты.
 */
typedef struct {
    uint32_t raw;
    uint32_t architecture;   /* семейство (Ada и т.п.) — поле сверить */
    uint32_t implementation; /* конкретный чип (AD104 и т.п.) — поле сверить */
    uint32_t revision;       /* ревизия кремния — поле сверить */
} nv_boot0_t;

static inline nv_boot0_t nv_decode_boot0(uint32_t v)
{
    nv_boot0_t b;
    b.raw = v;
    /* Раскладка ниже ПРЕДВАРИТЕЛЬНАЯ, подлежит проверке (TODO week3). */
    b.architecture   = (v >> 24) & 0x1F;
    b.implementation = (v >> 20) & 0x0F;
    b.revision       = v & 0xFF;
    return b;
}

#endif /* RTXMACOC_ADA_REGS_H */
