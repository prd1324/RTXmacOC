/*
 * fwsec_patch.c — реализация патча FWSEC (см. fwsec_patch.h).
 *
 * Портировано из nova-core fwsec.rs::new_fwsec и firmware.rs (FalconUCodeDescV2/V3).
 * Пересказано для соответствия лицензии. Структуры (offsets):
 *
 *   FalconAppifHdrV1      (4б):  version u8@0, header_size u8@1, entry_size u8@2,
 *                                entry_count u8@3
 *   FalconAppifV1 (packed,8б):   id u32@0, dmem_base u32@4
 *   FalconAppifDmemmapperV3:     cmd_in_buffer_offset u32@8, init_cmd u32@44
 *   ReadVbios   (packed,24б):    ver u32@0, hdr u32@4, addr u64@8, size u32@16, flags u32@20
 *   FrtsRegion  (packed,20б):    ver u32@0, hdr u32@4, addr u32@8, size u32@12, ftype u32@16
 *   FrtsCmd     (packed,44б):    ReadVbios@0, FrtsRegion@24
 *
 * Все смещения внутри ucode-блоба считаются от imem_load_size для DMEM-структур
 * (DMEM идёт сразу после IMEM в блобе).
 */
#include "fwsec_patch.h"

#define NVFW_FALCON_APPIF_ID_DMEMMAPPER 0x4u
#define NVFW_FRTS_REGION_TYPE_FB        2u

#define DMEMMAPPER_CMD_IN_BUFFER_OFFSET 8u
#define DMEMMAPPER_INIT_CMD_OFFSET      44u
#define SZ_READVBIOS                    24u
#define SZ_FRTSREGION                   20u

/* --- little-endian читатели/писатели с проверкой границ --- */
static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void st64(uint8_t *p, uint64_t v) {
    st32(p, (uint32_t)v); st32(p + 4, (uint32_t)(v >> 32));
}
static int fits(size_t off, size_t need, size_t len) { return off <= len && need <= len - off; }

static uint32_t popcount32(uint32_t v) {
    uint32_t c = 0;
    while (v) { v &= v - 1; c++; }
    return c;
}

int nv_fwsec_desc_parse(const uint8_t *desc, size_t len, nv_fwsec_desc_t *out)
{
    if (!desc || !out || len < 4) return NV_FWSEC_ERR_BOUNDS;
    uint8_t ver = desc[1];
    uint32_t hdr = ld32(desc);
    out->version  = ver;
    out->hdr_size = (hdr >> 16) & 0xFFFFu;

    if (ver == 3) {
        if (len < 44) return NV_FWSEC_ERR_BOUNDS;
        out->pkc_data_offset    = ld32(desc + 8);
        out->interface_offset   = ld32(desc + 12);
        out->imem_phys_base     = ld32(desc + 16);
        out->imem_load_size     = ld32(desc + 20);
        out->dmem_phys_base     = ld32(desc + 28);
        out->dmem_load_size     = ld32(desc + 32);
        out->engine_id_mask     = (uint16_t)(ld32(desc + 36) & 0xFFFFu);
        out->ucode_id           = desc[38];
        out->signature_count    = desc[39];
        out->signature_versions = (uint16_t)(ld32(desc + 40) & 0xFFFFu);
        return NV_FWSEC_OK;
    } else if (ver == 2) {
        if (len < 60) return NV_FWSEC_ERR_BOUNDS;
        out->interface_offset   = ld32(desc + 16);
        out->imem_phys_base     = ld32(desc + 20);
        out->imem_load_size     = ld32(desc + 24);
        out->dmem_phys_base     = ld32(desc + 44);
        out->dmem_load_size     = ld32(desc + 48);
        out->pkc_data_offset    = 0;
        out->engine_id_mask     = 0;
        out->ucode_id           = 0;
        out->signature_count    = 0;
        out->signature_versions = 0;
        return NV_FWSEC_OK;
    }
    return NV_FWSEC_ERR_PARSE;
}

int nv_fwsec_patch_frts(uint8_t *ucode, size_t ucode_len, const nv_fwsec_desc_t *d,
                        uint64_t frts_addr, uint64_t frts_size)
{
    if (!ucode || !d) return NV_FWSEC_ERR_PARSE;

    /* Application Interface Table headers: imem_load_size + interface_offset. */
    size_t hdr_off = (size_t)d->imem_load_size + (size_t)d->interface_offset;
    if (!fits(hdr_off, 4, ucode_len)) return NV_FWSEC_ERR_BOUNDS;

    uint8_t  appif_version = ucode[hdr_off + 0];
    uint8_t  header_size   = ucode[hdr_off + 1];
    uint8_t  entry_size    = ucode[hdr_off + 2];
    uint8_t  entry_count   = ucode[hdr_off + 3];
    if (appif_version != 1) return NV_FWSEC_ERR_PARSE;

    for (unsigned i = 0; i < entry_count; i++) {
        size_t entry_off = hdr_off + header_size + (size_t)i * entry_size;
        if (!fits(entry_off, 8, ucode_len)) return NV_FWSEC_ERR_BOUNDS;

        uint32_t id        = ld32(ucode + entry_off);
        uint32_t dmem_base = ld32(ucode + entry_off + 4);
        if (id != NVFW_FALCON_APPIF_ID_DMEMMAPPER) continue;

        /* DMEMMAPPER находится в DMEM: imem_load_size + dmem_base. */
        size_t mapper_off = (size_t)d->imem_load_size + (size_t)dmem_base;
        if (!fits(mapper_off, DMEMMAPPER_INIT_CMD_OFFSET + 4, ucode_len))
            return NV_FWSEC_ERR_BOUNDS;

        /* init_cmd = FRTS. */
        st32(ucode + mapper_off + DMEMMAPPER_INIT_CMD_OFFSET, NVFW_DMEMMAPPER_CMD_FRTS);

        uint32_t cmd_in_off = ld32(ucode + mapper_off + DMEMMAPPER_CMD_IN_BUFFER_OFFSET);

        size_t frts_cmd_off = (size_t)d->imem_load_size + (size_t)cmd_in_off;
        if (!fits(frts_cmd_off, SZ_READVBIOS + SZ_FRTSREGION, ucode_len))
            return NV_FWSEC_ERR_BOUNDS;

        /* ReadVbios { ver=1, hdr=sizeof(ReadVbios), addr=0, size=0, flags=2 }. */
        uint8_t *rv = ucode + frts_cmd_off;
        st32(rv + 0, 1u);
        st32(rv + 4, SZ_READVBIOS);
        st64(rv + 8, 0u);
        st32(rv + 16, 0u);
        st32(rv + 20, 2u);

        /* FrtsRegion { ver=1, hdr=sizeof(FrtsRegion), addr=frts>>12, size>>12, FB }. */
        uint8_t *fr = ucode + frts_cmd_off + SZ_READVBIOS;
        st32(fr + 0, 1u);
        st32(fr + 4, SZ_FRTSREGION);
        st32(fr + 8, (uint32_t)(frts_addr >> 12));
        st32(fr + 12, (uint32_t)(frts_size >> 12));
        st32(fr + 16, NVFW_FRTS_REGION_TYPE_FB);

        return NV_FWSEC_OK;
    }
    return NV_FWSEC_ERR_PARSE; /* DMEMMAPPER не найден */
}

int nv_fwsec_select_signature_index(const nv_fwsec_desc_t *d, uint32_t fuse_version,
                                    uint32_t *out_index)
{
    if (!d || !out_index) return NV_FWSEC_ERR_PARSE;
    if (d->signature_count == 0) return NV_FWSEC_ERR_NOSIG;
    if (fuse_version >= 32) return NV_FWSEC_ERR_NOSIG;

    uint32_t sig_versions = (uint32_t)d->signature_versions;
    uint32_t bit = 1u << fuse_version;
    if ((sig_versions & bit) == 0)
        return NV_FWSEC_ERR_NOSIG; /* нет подходящей подписи */

    /* Индекс = число единичных бит ниже бита fuse-версии. */
    uint32_t mask = bit - 1u;
    *out_index = popcount32(sig_versions & mask);
    return NV_FWSEC_OK;
}

int nv_fwsec_patch_signature(uint8_t *ucode, size_t ucode_len, const nv_fwsec_desc_t *d,
                             const uint8_t *sig384)
{
    if (!ucode || !d || !sig384) return NV_FWSEC_ERR_PARSE;
    size_t off = (size_t)d->imem_load_size + (size_t)d->pkc_data_offset;
    if (!fits(off, NV_BCRT30_RSA3K_SIG_SIZE, ucode_len)) return NV_FWSEC_ERR_BOUNDS;
    for (size_t i = 0; i < NV_BCRT30_RSA3K_SIG_SIZE; i++)
        ucode[off + i] = sig384[i];
    return NV_FWSEC_OK;
}
