/*
 * booter.h — разбор контейнера подписанной прошивки Booter (HS, для SEC2).
 *
 * Booter — Heavy-Secure ucode NVIDIA, исполняемый на SEC2; на Turing/Ampere/Ada он
 * грузит прошивку GSP-RM в WPR2 и стартует GSP в RISC-V. Блоб — внешний файл
 * (booter_load / booter_unload из linux-firmware), формат — «bin»-контейнер с
 * вложенным Heavy-Secure firmware v2 (НЕ VBIOS-дескриптор FalconUCodeDescV3).
 *
 * Этот модуль ТОЛЬКО разбирает контейнер (offline). Запуск на SEC2 (reset/dma_load/
 * program_brom/boot) — отдельная фаза (использует generic driver/gsp/falcon.c).
 *
 * Источники структур (пересказано для лицензии, сверено с дампом 535.113.01):
 *   nvfw_bin_hdr            — nouveau include/nvfw/fw.h
 *   nvfw_hs_header_v2       — nouveau include/nvfw/hs.h (nova-core firmware/hs.rs)
 *   nvfw_hs_load_header_v2  — nouveau include/nvfw/hs.h
 * Раскладка — docs/PORTING-MAP.md.
 */
#ifndef RTXMACOC_BOOTER_H
#define RTXMACOC_BOOTER_H

#include <stdint.h>
#include <stddef.h>

#define NV_BOOTER_OK            0
#define NV_BOOTER_ERR_ARG     (-1)
#define NV_BOOTER_ERR_BOUNDS  (-2)
#define NV_BOOTER_ERR_MAGIC   (-3)  /* не bin-контейнер NVIDIA (0x10de) */
#define NV_BOOTER_ERR_LAYOUT  (-4)  /* несогласованные размеры/смещения */

#define NV_BOOTER_BIN_MAGIC   0x10deu
#define NV_BOOTER_MAX_APPS    8u
#define NV_BOOTER_SIG_SIZE    384u   /* RSA3K-подпись (как FWSEC, NV_BCRT30_RSA3K_SIG_SIZE) */

/* Одна запись приложения в load header v2 (16 байт). Смещения — относительно data. */
typedef struct {
    uint32_t code_offset;
    uint32_t code_size;
    uint32_t data_offset;
    uint32_t data_size;
} nv_booter_app_t;

/*
 * Разобранный дескриптор Booter. Смещения os_code/os_data/app/patch_loc — ОТНОСИТЕЛЬНО
 * региона данных (data_abs); подписи и meta — абсолютные в блобе. Адресация ucode-байтов:
 *   ucode = blob + data_abs + os_code_offset (и т.д.).
 */
typedef struct {
    /* bin header */
    uint32_t data_abs;          /* абсолютное смещение региона ucode в блобе (data_offset) */
    uint32_t data_size;

    /* HS header v2 */
    uint32_t sig_prod_abs;      /* абсолют: массив подписей */
    uint32_t sig_prod_size;     /* = num_sig * 384 */
    uint32_t num_sig;           /* разрешённое значение (число подписей) */
    uint32_t patch_loc;         /* data-relative: куда вставлять выбранную подпись */
    uint32_t patch_sig;         /* индекс/значение из контейнера */
    uint32_t meta_abs;          /* абсолют: meta_data (HsSignatureParams) */
    uint32_t meta_size;
    /* HsSignatureParams (meta_data, 3×u32; nova firmware/booter.rs): */
    uint32_t sig_fuse_ver;      /* @0 fuse_ver (для выбора подписи) */
    uint32_t engine_id_mask;    /* @4 маска движков (SEC2=0x0001) */
    uint32_t ucode_id;          /* @8 id микрокода */

    /* load header v2 (data-relative смещения) */
    uint32_t os_code_offset;
    uint32_t os_code_size;
    uint32_t os_data_offset;
    uint32_t os_data_size;
    uint32_t num_apps;
    nv_booter_app_t app[NV_BOOTER_MAX_APPS];

    /* вычислено: смещение под подпись для BROM (как pkc_data_offset у FWSEC) */
    uint32_t pkc_data_offset;   /* = patch_loc - os_data_offset */
} nv_booter_desc_t;

/* Особое значение: 0 в HW fuse-версии → использовать последнюю подпись. */
#define NV_BOOTER_FUSE_VERSION_USE_LAST_SIG 0u

/*
 * Разобрать блоб Booter (buf/len). При успехе NV_BOOTER_OK и заполненный out.
 * Все смещения проверяются на границы len.
 */
int nv_booter_parse(const uint8_t *buf, size_t len, nv_booter_desc_t *out);

/*
 * Выбрать индекс подписи по HW fuse-версии (nova firmware/booter.rs):
 *   reg_fuse_version == 0  → последняя подпись (num_sig-1);
 *   иначе                  → idx = desc->sig_fuse_ver - reg_fuse_version.
 * reg_fuse_version читается вызывающим из HW (nv_falcon_signature_fuse_version_ga102
 * с engine_id_mask=desc->engine_id_mask, ucode_id=desc->ucode_id).
 * Возвращает NV_BOOTER_OK и *out_index, либо NV_BOOTER_ERR_LAYOUT при некорректной версии.
 */
int nv_booter_select_signature(const nv_booter_desc_t *desc, uint32_t reg_fuse_version,
                               uint32_t *out_index);

#endif /* RTXMACOC_BOOTER_H */
