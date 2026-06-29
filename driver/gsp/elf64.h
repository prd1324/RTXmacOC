/*
 * elf64.h — минимальный поиск секции ELF64 по имени (для образа GSP-RM).
 *
 * GSP-RM (gsp-<ver>.bin) — это ELF64, где прошивка лежит в секции `.fwimage`, а
 * подписи — в `.fwsignature_<chip>` (для AD104 — `.fwsignature_ad10x`). Нужен лишь
 * разбор таблицы секций + строк; без зависимостей, портируемо (kext/Linux).
 */
#ifndef RTXMACOC_ELF64_H
#define RTXMACOC_ELF64_H

#include <stdint.h>
#include <stddef.h>

#define NV_ELF_OK          0
#define NV_ELF_ERR_ARG   (-1)
#define NV_ELF_ERR_FORMAT (-2)  /* не ELF64-LE */
#define NV_ELF_ERR_BOUNDS (-3)
#define NV_ELF_ERR_NOSEC  (-4)  /* секция не найдена */

/*
 * Найти секцию по имени. При успехе NV_ELF_OK и out_off,out_size — смещение и
 * размер данных секции в буфере buf. Проверяет границы len.
 */
int nv_elf64_section(const uint8_t *buf, size_t len, const char *name,
                     uint64_t *out_off, uint64_t *out_size);

#endif /* RTXMACOC_ELF64_H */
