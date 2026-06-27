/*
 * fwsec_patch.h — патч FWSEC-образа перед запуском на Falcon GSP (слой 2, шаг 3).
 *
 * FWSEC извлечён из VBIOS (tools/vbios_dump). Перед загрузкой его надо
 * пропатчить (nova-core fwsec.rs::new_fwsec):
 *   1. В DMEMMAPPER вписать команду (FRTS — создать WPR2);
 *   2. Записать FrtsCmd (ReadVbios + FrtsRegion) в командный буфер;
 *   3. Вставить нужную HS-подпись (RSA-3K) по fuse-версии.
 *
 * Работает с байтами ucode-блоба (imem||dmem), который пишет vbios_dump
 * --extract. Все структуры/смещения — из fwsec.rs/firmware.rs (см. PORTING-MAP).
 */
#ifndef RTXMACOC_FWSEC_PATCH_H
#define RTXMACOC_FWSEC_PATCH_H

#include <stdint.h>
#include <stddef.h>

/* Команды DMEMMAPPER (fwsec.rs). */
#define NVFW_DMEMMAPPER_CMD_FRTS 0x15u
#define NVFW_DMEMMAPPER_CMD_SB   0x19u

#define NV_FWSEC_OK         0
#define NV_FWSEC_ERR_PARSE  (-1)
#define NV_FWSEC_ERR_BOUNDS (-2)
#define NV_FWSEC_ERR_NOSIG  (-3)

/*
 * Поля дескриптора FWSEC, нужные для патча (из FalconUCodeDescV2/V3, firmware.rs).
 * Заполняется парсером nv_fwsec_desc_parse() из 44/60-байтного дескриптора.
 */
typedef struct {
    int      version;            /* 2 или 3 */
    uint32_t hdr_size;           /* (hdr>>16)&0xffff */
    uint32_t imem_load_size;
    uint32_t imem_phys_base;     /* куда грузить IMEM (dst_start) */
    uint32_t dmem_load_size;
    uint32_t dmem_phys_base;     /* куда грузить DMEM (dst_start) */
    uint32_t interface_offset;
    uint32_t pkc_data_offset;    /* V3 */
    uint16_t engine_id_mask;     /* V3 */
    uint16_t signature_versions; /* V3 */
    uint8_t  ucode_id;           /* V3 */
    uint8_t  signature_count;    /* V3 */
} nv_fwsec_desc_t;

/* Размер одной RSA-3K подписи FWSEC (fwsec.rs). */
#define NV_BCRT30_RSA3K_SIG_SIZE 384u

/*
 * Распарсить дескриптор FWSEC из сырых байт (то, что лежит по falcon_ucode_offset,
 * см. vbios_dump). Версия — байт +1. Возвращает NV_FWSEC_OK.
 */
int nv_fwsec_desc_parse(const uint8_t *desc, size_t len, nv_fwsec_desc_t *out);

/*
 * Пропатчить ucode-блоб (imem||dmem) под команду FRTS: найти DMEMMAPPER, выставить
 * init_cmd=FRTS и записать FrtsCmd с регионом [frts_addr, frts_size] (тип FB).
 * frts_addr/size — физический адрес и размер региона WPR2 в VRAM.
 */
int nv_fwsec_patch_frts(uint8_t *ucode, size_t ucode_len, const nv_fwsec_desc_t *d,
                        uint64_t frts_addr, uint64_t frts_size);

/*
 * Выбрать индекс подписи по fuse-версии (fwsec.rs FwsecFirmware::new):
 * проверить, что бит fuse-версии присутствует в signature_versions, индекс =
 * число единичных бит ниже него. fuse_version берётся из
 * nv_falcon_signature_fuse_version_ga102().
 */
int nv_fwsec_select_signature_index(const nv_fwsec_desc_t *d, uint32_t fuse_version,
                                    uint32_t *out_index);

/*
 * Вставить подпись (384 байта) в ucode по смещению imem_load_size + pkc_data_offset.
 */
int nv_fwsec_patch_signature(uint8_t *ucode, size_t ucode_len, const nv_fwsec_desc_t *d,
                             const uint8_t *sig384);

#endif /* RTXMACOC_FWSEC_PATCH_H */
