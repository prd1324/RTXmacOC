/*
 * gsp_fw.c — реализация подготовки GSP-RM (см. gsp_fw.h).
 *
 * RM_RISCV_UCODE_DESC (nouveau nvrm/gsp.h, 21×u32 от header_offset bin-контейнера):
 *   version@0, bootloaderOffset@4, bootloaderSize@8, bootloaderParamOffset@12,
 *   bootloaderParamSize@16, riscvElfOffset@20, riscvElfSize@24, appVersion@28,
 *   manifestOffset@32, manifestSize@36, monitorDataOffset@40, monitorDataSize@44,
 *   monitorCodeOffset@48, monitorCodeSize@52, ... (всего 84 байта).
 * radix3 — nouveau r535 (3 уровня, 4K, u64-записи).
 */
#include "gsp_fw.h"
#include "elf64.h"

static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void st64(uint8_t *p, uint64_t v)
{ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int fits(uint64_t off, uint64_t need, size_t len)
{ return off <= len && need <= (uint64_t)len - off; }

#define BIN_MAGIC 0x10deu

int nv_gsp_rm_sections(const uint8_t *elf, size_t len, const char *chip_sig,
                       nv_gsp_sections_t *out)
{
    if (!elf || !chip_sig || !out) return NV_GSP_ERR_ARG;
    int rc = nv_elf64_section(elf, len, ".fwimage", &out->fwimage_off, &out->fwimage_size);
    if (rc == NV_ELF_ERR_NOSEC) return NV_GSP_ERR_NOSEC;
    if (rc != NV_ELF_OK) return (rc == NV_ELF_ERR_BOUNDS) ? NV_GSP_ERR_BOUNDS : NV_GSP_ERR_FORMAT;
    rc = nv_elf64_section(elf, len, chip_sig, &out->sig_off, &out->sig_size);
    if (rc == NV_ELF_ERR_NOSEC) return NV_GSP_ERR_NOSEC;
    if (rc != NV_ELF_OK) return (rc == NV_ELF_ERR_BOUNDS) ? NV_GSP_ERR_BOUNDS : NV_GSP_ERR_FORMAT;
    return NV_GSP_OK;
}

int nv_gsp_bootloader_parse(const uint8_t *buf, size_t len, nv_gsp_bootloader_t *out)
{
    if (!buf || !out) return NV_GSP_ERR_ARG;
    if (!fits(0, 24, len)) return NV_GSP_ERR_BOUNDS;
    if (ld32(buf + 0) != BIN_MAGIC) return NV_GSP_ERR_FORMAT;
    uint32_t hdr_off  = ld32(buf + 12);
    uint32_t data_off = ld32(buf + 16);
    uint32_t data_sz  = ld32(buf + 20);
    if (!fits(data_off, data_sz, len)) return NV_GSP_ERR_BOUNDS;
    if (!fits(hdr_off, 84, len)) return NV_GSP_ERR_BOUNDS; /* RM_RISCV_UCODE_DESC = 84б */

    const uint8_t *d = buf + hdr_off;
    out->data_abs        = data_off;
    out->data_size       = data_sz;
    out->app_version     = ld32(d + 28);
    out->manifest_offset = ld32(d + 32);
    out->manifest_size   = ld32(d + 36);
    out->data_offset     = ld32(d + 40);  /* monitorDataOffset */
    out->code_offset     = ld32(d + 48);  /* monitorCodeOffset */
    return NV_GSP_OK;
}

int nv_gsp_radix3_levels(uint64_t fwimage_size, uint32_t *n2, uint32_t *lvl2_pages)
{
    if (!n2 || !lvl2_pages || fwimage_size == 0) return NV_GSP_ERR_ARG;
    uint64_t pages = (fwimage_size + NV_GSP_PAGE_SIZE - 1) / NV_GSP_PAGE_SIZE; /* записей в lvl2 */
    uint64_t l2_bytes = pages * 8u;
    uint64_t l2_pages = (l2_bytes + NV_GSP_PAGE_SIZE - 1) / NV_GSP_PAGE_SIZE;  /* страниц lvl2 = записей lvl1 */
    /* lvl1 — одна страница: до 512 записей. Ёмкость radix3 = 512*512 страниц (1 ГБ). */
    if (l2_pages > NV_GSP_RADIX3_ENTRIES_PER_PAGE) return NV_GSP_ERR_CAP;
    *n2 = (uint32_t)pages;
    *lvl2_pages = (uint32_t)l2_pages;
    return NV_GSP_OK;
}

int nv_gsp_radix3_fill(uint64_t fwimage_size,
                       uint8_t *lvl0, uint8_t *lvl1, uint8_t *lvl2,
                       uint64_t dma_fwimage, uint64_t dma_lvl2, uint64_t dma_lvl1)
{
    if (!lvl0 || !lvl1 || !lvl2) return NV_GSP_ERR_ARG;
    uint32_t n2 = 0, l2_pages = 0;
    int rc = nv_gsp_radix3_levels(fwimage_size, &n2, &l2_pages);
    if (rc != NV_GSP_OK) return rc;

    /* lvl2: запись на каждую 4K-страницу образа. */
    for (uint32_t i = 0; i < n2; i++)
        st64(lvl2 + (size_t)i * 8u, dma_fwimage + (uint64_t)i * NV_GSP_PAGE_SIZE);
    /* lvl1: запись на каждую страницу lvl2. */
    for (uint32_t j = 0; j < l2_pages; j++)
        st64(lvl1 + (size_t)j * 8u, dma_lvl2 + (uint64_t)j * NV_GSP_PAGE_SIZE);
    /* lvl0: единственная запись → страница lvl1. */
    st64(lvl0 + 0, dma_lvl1);
    return NV_GSP_OK;
}
