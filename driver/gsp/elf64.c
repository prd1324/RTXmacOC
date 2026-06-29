/*
 * elf64.c — реализация поиска секции ELF64 по имени (см. elf64.h).
 * Только ELF64 little-endian. Раскладка — стандарт ELF (Elf64_Ehdr/Elf64_Shdr).
 */
#include "elf64.h"
#include <string.h>

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t *p)
{ return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }

static int fits(uint64_t off, uint64_t need, size_t len)
{ return off <= len && need <= (uint64_t)len - off; }

/* Elf64_Ehdr: e_shoff@0x28(u64), e_shentsize@0x3a(u16), e_shnum@0x3c(u16),
   e_shstrndx@0x3e(u16). Elf64_Shdr: sh_name@0(u32), sh_offset@0x18(u64), sh_size@0x20(u64). */
int nv_elf64_section(const uint8_t *buf, size_t len, const char *name,
                     uint64_t *out_off, uint64_t *out_size)
{
    if (!buf || !name || !out_off || !out_size) return NV_ELF_ERR_ARG;
    if (len < 64) return NV_ELF_ERR_BOUNDS;
    /* magic 0x7f 'E' 'L' 'F', класс=2 (64-bit), data=1 (LE). */
    if (!(buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F'))
        return NV_ELF_ERR_FORMAT;
    if (buf[4] != 2 || buf[5] != 1) return NV_ELF_ERR_FORMAT;

    uint64_t shoff   = r64(buf + 0x28);
    uint16_t shentsz = r16(buf + 0x3a);
    uint16_t shnum   = r16(buf + 0x3c);
    uint16_t shstrnd = r16(buf + 0x3e);
    if (shentsz < 64 || shnum == 0) return NV_ELF_ERR_FORMAT;
    if (!fits(shoff, (uint64_t)shnum * shentsz, len)) return NV_ELF_ERR_BOUNDS;
    if (shstrnd >= shnum) return NV_ELF_ERR_FORMAT;

    /* Таблица строк секций. */
    const uint8_t *strsh = buf + shoff + (uint64_t)shstrnd * shentsz;
    uint64_t str_off  = r64(strsh + 0x18);
    uint64_t str_size = r64(strsh + 0x20);
    if (!fits(str_off, str_size, len)) return NV_ELF_ERR_BOUNDS;

    size_t namelen = strlen(name);
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = buf + shoff + (uint64_t)i * shentsz;
        uint32_t sh_name = r32(sh + 0);
        if (sh_name >= str_size) continue;
        const char *s = (const char *)(buf + str_off + sh_name);
        /* Имя должно завершиться NUL в пределах таблицы строк. */
        uint64_t maxn = str_size - sh_name;
        if (namelen + 1 > maxn) continue;
        if (memcmp(s, name, namelen) != 0 || s[namelen] != '\0') continue;

        uint64_t so = r64(sh + 0x18), ss = r64(sh + 0x20);
        if (!fits(so, ss, len)) return NV_ELF_ERR_BOUNDS;
        *out_off = so; *out_size = ss;
        return NV_ELF_OK;
    }
    return NV_ELF_ERR_NOSEC;
}
