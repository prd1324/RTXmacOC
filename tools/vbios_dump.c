/*
 * vbios_dump.c — слой 2, шаг 1: чтение VBIOS карты и разбор образов PCI ROM.
 *
 * Зачем: bring-up GSP начинается с извлечения FWSEC из VBIOS (см.
 * docs/gsp-bringup-notes.md §3, §7.1). FWSEC лежит в расширенном образе ROM
 * (code type 0xE0). Эта утилита читает ROM и раскладывает его на образы PCI,
 * находит NVIDIA-образы и сигнатуру BIT — фундамент для последующего извлечения
 * FWSEC.
 *
 * Что делает сейчас:
 *   - читает VBIOS из файла-дампа ИЛИ из Linux sysfs (/sys/.../rom);
 *   - валидирует сигнатуру 0x55AA, идёт по образам через структуры PCIR
 *     (по PCI Firmware Spec — без догадок);
 *   - печатает для каждого образа: code type, vendor/device, длину, last-флаг;
 *   - помечает NVIDIA-образы и расширенный образ 0xE0 (где живёт FWSEC);
 *   - ищет сигнатуру BIT (точка входа в таблицы VBIOS NVIDIA).
 *
 * Чего пока НЕ делает (намеренно, следующий шаг):
 *   - не парсит PmuLookupTable / FalconUCodeDescV3 (точные смещения портировать
 *     из nova-core vbios.rs, не выдумывать — см. AGENTS.md правило 1).
 *
 * Сборка (любая ОС): cc tools/vbios_dump.c -o vbios_dump
 * Использование:
 *   vbios_dump <rom_dump_file>
 *   vbios_dump --pci 0000:01:00.0      (только Linux: читает sysfs rom)
 *
 * Как снять дамп VBIOS:
 *   Linux:   (как root) echo 1 > /sys/bus/pci/devices/0000:01:00.0/rom;
 *            cat .../rom > vbios.rom; echo 0 > .../rom
 *   Windows: GPU-Z → кнопка с чипом → Save to file (.rom), затем сюда файлом.
 *   macOS:   TODO — VBIOS публикуется в IORegistry (свойство "ROM" узла GPU);
 *            добавить IOKit-путь чтения отдельно.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PCI_VENDOR_NVIDIA 0x10DEu
#define ROM_SIG           0xAA55u   /* первые два байта образа: 0x55 0xAA */

/* Прочитать u16 little-endian из буфера по смещению (с проверкой границ). */
static int rd16(const uint8_t *buf, size_t len, size_t off, uint16_t *out)
{
    if (off + 2 > len) return -1;
    *out = (uint16_t)(buf[off] | (buf[off + 1] << 8));
    return 0;
}

static const char *code_type_name(uint8_t t)
{
    switch (t) {
        case 0x00: return "x86 legacy BIOS";
        case 0x03: return "EFI";
        case 0x70: return "NVIDIA NBSI";
        case 0xE0: return "NVIDIA extended (FWSEC/ucode тут)";
        default:   return "прочее";
    }
}

/*
 * Разбор цепочки образов PCI expansion ROM (PCI Firmware Spec 3.0).
 * Каждый образ:
 *   +0x00: 0x55 0xAA
 *   +0x18: u16 указатель на структуру PCIR (отн. начала образа)
 * PCIR:
 *   +0x00: "PCIR"
 *   +0x04: u16 vendor id
 *   +0x06: u16 device id
 *   +0x10: u16 длина образа в блоках по 512 байт
 *   +0x14: u8  code type
 *   +0x15: u8  indicator (bit7 = последний образ)
 */
static void parse_pci_images(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    int idx = 0;

    while (off + 0x1A <= len) {
        uint16_t sig = 0;
        if (rd16(buf, len, off, &sig) != 0 || sig != ROM_SIG) {
            if (idx == 0)
                printf("  [нет сигнатуры 0x55AA по смещению 0x%zx — не PCI ROM?]\n", off);
            break;
        }

        uint16_t pcir_off = 0;
        if (rd16(buf, len, off + 0x18, &pcir_off) != 0) break;
        size_t pcir = off + pcir_off;
        if (pcir + 0x16 > len) break;

        if (memcmp(buf + pcir, "PCIR", 4) != 0) {
            printf("  [образ #%d @0x%zx: PCIR не найден по 0x%zx]\n", idx, off, pcir);
            break;
        }

        uint16_t vendor = 0, device = 0, blocks = 0;
        rd16(buf, len, pcir + 0x04, &vendor);
        rd16(buf, len, pcir + 0x06, &device);
        rd16(buf, len, pcir + 0x10, &blocks);
        uint8_t code_type = buf[pcir + 0x14];
        uint8_t indicator = buf[pcir + 0x15];
        int last = (indicator & 0x80) != 0;
        size_t img_len = (size_t)blocks * 512;

        printf("  образ #%d @0x%06zx: vendor=0x%04X device=0x%04X "
               "type=0x%02X (%s) len=0x%zx%s%s\n",
               idx, off, vendor, device, code_type, code_type_name(code_type),
               img_len,
               vendor == PCI_VENDOR_NVIDIA ? "  [NVIDIA]" : "",
               last ? "  [LAST]" : "");

        if (img_len == 0) {
            printf("  [длина образа 0 — стоп]\n");
            break;
        }
        if (last) break;
        off += img_len;
        idx++;
    }
}

/*
 * Поиск сигнатуры BIT (BIOS Information Table) — точка входа в таблицы VBIOS
 * NVIDIA. Сигнатура: 0xFF 0xB8 'B' 'I' 'T' 0x00 (envytools/nouveau).
 * Дальнейший разбор токенов BIT и через них PMU-таблицы с FWSEC — следующий шаг
 * (смещения из nova-core vbios.rs).
 */
static void find_bit(const uint8_t *buf, size_t len)
{
    static const uint8_t sig[6] = { 0xFF, 0xB8, 'B', 'I', 'T', 0x00 };
    for (size_t i = 0; i + sizeof(sig) <= len; i++) {
        if (memcmp(buf + i, sig, sizeof(sig)) == 0) {
            printf("  BIT signature @0x%06zx — точка входа в таблицы VBIOS.\n", i);
            printf("  TODO(слой2): распарсить токены BIT -> PMU table -> FWSEC\n");
            printf("              (смещения портировать из nova-core vbios.rs).\n");
            return;
        }
    }
    printf("  BIT signature не найдена (дамп урезан? не NVIDIA VBIOS?).\n");
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_len = got;
    return buf;
}

#ifdef __linux__
/* Включить и прочитать ROM через sysfs: /sys/bus/pci/devices/<bdf>/rom */
static uint8_t *read_sysfs_rom(const char *bdf, size_t *out_len)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/rom", bdf);
    FILE *en = fopen(path, "w");
    if (en) { fputc('1', en); fclose(en); }   /* enable ROM decode */
    uint8_t *buf = read_file(path, out_len);
    FILE *dis = fopen(path, "w");
    if (dis) { fputc('0', dis); fclose(dis); }
    return buf;
}
#endif

int main(int argc, char **argv)
{
    printf("=== RTXmacOC :: VBIOS dump/parse (слой 2, шаг 1) ===\n");

    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <rom_file>\n"
#ifdef __linux__
            "       %s --pci 0000:01:00.0   (Linux sysfs)\n"
#endif
            , argv[0]
#ifdef __linux__
            , argv[0]
#endif
        );
        return 1;
    }

    uint8_t *buf = NULL;
    size_t len = 0;

#ifdef __linux__
    if (strcmp(argv[1], "--pci") == 0 && argc >= 3) {
        buf = read_sysfs_rom(argv[2], &len);
    } else
#endif
    {
        buf = read_file(argv[1], &len);
    }

    if (!buf) {
        fprintf(stderr, "не удалось прочитать VBIOS\n");
        return 2;
    }

    printf("Прочитано %zu байт VBIOS.\n", len);
    printf("--- Образы PCI ROM ---\n");
    parse_pci_images(buf, len);
    printf("--- Поиск BIT ---\n");
    find_bit(buf, len);

    free(buf);
    printf("Готово. Дальше: извлечь FWSEC из образа 0xE0 (см. gsp-bringup-notes §7.1).\n");
    return 0;
}
