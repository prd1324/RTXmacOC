/*
 * fwsec_locate.h — поиск дескриптора FWSEC Falcon-ucode в образе VBIOS.
 *
 * Переиспользуемая библиотека (без I/O и без main): логика портирована из
 * nova-core vbios.rs (PCI-образы → BIT token 0x70 → PmuLookupTable app 0x85 →
 * FalconUCodeDesc). Используется и утилитой tools/vbios_dump, и kext-оркестратором.
 *
 * Источник/смещения — docs/PORTING-MAP.md. Пересказано для соответствия лицензии.
 */
#ifndef RTXMACOC_FWSEC_LOCATE_H
#define RTXMACOC_FWSEC_LOCATE_H

#include <stdint.h>
#include <stddef.h>

#define NV_FWSEC_LOC_OK          0
#define NV_FWSEC_LOC_ERR_NOSIG   (-1) /* нет сигнатуры PCI ROM / BIT */
#define NV_FWSEC_LOC_ERR_NOIMG   (-2) /* нет PciAt или FWSEC образа */
#define NV_FWSEC_LOC_ERR_PARSE   (-3) /* не нашли токен/запись */
#define NV_FWSEC_LOC_ERR_BOUNDS  (-4)

/* Результат поиска: абсолютное смещение дескриптора FalconUCodeDesc в vbios. */
typedef struct {
    size_t desc_abs;   /* смещение FalconUCodeDescV2/V3 в буфере vbios */
} nv_fwsec_location_t;

/*
 * Найти дескриптор FWSEC в образе VBIOS (vbios/len). При успехе — NV_FWSEC_LOC_OK
 * и out->desc_abs. Дальше дескриптор парсится через nv_fwsec_desc_parse
 * (fwsec_patch.h), а ucode-блоб = desc_abs + hdr_size, длина imem_load+dmem_load.
 */
int nv_fwsec_locate(const uint8_t *vbios, size_t len, nv_fwsec_location_t *out);

#endif /* RTXMACOC_FWSEC_LOCATE_H */
