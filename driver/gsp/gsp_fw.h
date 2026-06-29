/*
 * gsp_fw.h — подготовка прошивки GSP-RM к загрузке (задача 6, фаза 4).
 *
 * Включает:
 *  - извлечение секций образа GSP-RM (ELF): .fwimage + .fwsignature_<chip>;
 *  - разбор дескриптора GSP-bootloader (bin-контейнер RM_RISCV_UCODE_DESC);
 *  - построение radix3 (3-уровневые 4K-таблицы над .fwimage для GSP MMU).
 *
 * GspFwWprMeta (раскладка региона WPR2) — отдельный модуль/шаг (математика layout).
 * Источники: nouveau r535 (radix3, RM_RISCV_UCODE_DESC), nova firmware/gsp.rs.
 * Пересказано для соответствия лицензии; раскладки — docs/PORTING-MAP.md.
 */
#ifndef RTXMACOC_GSP_FW_H
#define RTXMACOC_GSP_FW_H

#include <stdint.h>
#include <stddef.h>

#define NV_GSP_OK            0
#define NV_GSP_ERR_ARG     (-1)
#define NV_GSP_ERR_BOUNDS  (-2)
#define NV_GSP_ERR_FORMAT  (-3)
#define NV_GSP_ERR_NOSEC   (-4)
#define NV_GSP_ERR_CAP     (-5)  /* образ больше ёмкости radix3 (1 ГБ) */

#define NV_GSP_PAGE_SHIFT  12u
#define NV_GSP_PAGE_SIZE   (1u << NV_GSP_PAGE_SHIFT)   /* 4096 */
#define NV_GSP_RADIX3_ENTRIES_PER_PAGE (NV_GSP_PAGE_SIZE / 8u) /* 512 u64-записей */

/* Секции образа GSP-RM (ELF): прошивка и подпись под наш чип. */
typedef struct {
    uint64_t fwimage_off, fwimage_size;   /* .fwimage */
    uint64_t sig_off, sig_size;           /* .fwsignature_<chip> */
} nv_gsp_sections_t;

/*
 * Найти .fwimage и .fwsignature_<chip> в ELF GSP-RM. chip_sig — имя секции подписи
 * (напр. ".fwsignature_ad10x" для AD104).
 */
int nv_gsp_rm_sections(const uint8_t *elf, size_t len, const char *chip_sig,
                       nv_gsp_sections_t *out);

/* Дескриптор GSP-bootloader (нужные поля RM_RISCV_UCODE_DESC + payload-регион bin). */
typedef struct {
    uint32_t data_abs;          /* bin data_offset: начало payload bootloader */
    uint32_t data_size;         /* bin data_size */
    uint32_t app_version;       /* appVersion (пишется в FALCON_OS при init) */
    uint32_t code_offset;       /* monitorCodeOffset */
    uint32_t data_offset;       /* monitorDataOffset */
    uint32_t manifest_offset;   /* manifestOffset */
    uint32_t manifest_size;     /* manifestSize */
} nv_gsp_bootloader_t;

/* Разобрать bin-контейнер GSP-bootloader. */
int nv_gsp_bootloader_parse(const uint8_t *buf, size_t len, nv_gsp_bootloader_t *out);

/* Размеры radix3 для образа размером fwimage_size:
 *   n2  = ceil(fwimage_size / 4K)          — записей в lvl2 (по странице образа);
 *   lvl2_pages = ceil(n2*8 / 4K)           — страниц под lvl2 (= записей в lvl1);
 *   lvl1/lvl0 — по 1 странице. Ёмкость: 512*512 страниц = 1 ГБ. */
int nv_gsp_radix3_levels(uint64_t fwimage_size, uint32_t *n2, uint32_t *lvl2_pages);

/*
 * Заполнить radix3 для СПЛОШНЫХ (contiguous) буферов:
 *   lvl2[i] = dma_fwimage + i*4K  (i: 0..n2-1)
 *   lvl1[j] = dma_lvl2     + j*4K  (j: 0..lvl2_pages-1)
 *   lvl0[0] = dma_lvl1
 * Указатели lvlN — на выделенные (обнулённые) буферы; dma_* — их IOVA/физ-адреса.
 * lvl0/lvl1 — по 1 странице; lvl2 — lvl2_pages страниц.
 */
int nv_gsp_radix3_fill(uint64_t fwimage_size,
                       uint8_t *lvl0, uint8_t *lvl1, uint8_t *lvl2,
                       uint64_t dma_fwimage, uint64_t dma_lvl2, uint64_t dma_lvl1);

#endif /* RTXMACOC_GSP_FW_H */
