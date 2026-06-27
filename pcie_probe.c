/*
 * pcie_probe.c — Шаг 1 проекта RTXmacOC
 *
 * Цель: из user-space на macOS найти NVIDIA RTX 4070 Super (Ada AD104)
 * на PCIe-шине и прочитать её конфигурационное пространство:
 *   - vendor / device id  (подтверждаем, что это именно Ada)
 *   - subsystem id        (производитель платы)
 *   - class code          (это VGA/3D-контроллер)
 *   - BAR0..BAR5          (адреса и размеры регионов: регистры, VRAM aperture)
 *   - IRQ / pin
 *
 * Что это НЕ делает (намеренно, это следующие шаги):
 *   - не мапит BAR в память (нужен DriverKit-драйвер + entitlement)
 *   - не пишет в регистры карты
 *   - не трогает прошивку
 *
 * Почему это работает без драйвера и без отключения SIP:
 *   IOKit публикует все PCIe-устройства в IORegistry как объекты
 *   IOPCIDevice. Их СВОЙСТВА (включая снимок config space и описание BAR)
 *   доступны на чтение любому user-space процессу. Маппинг памяти —
 *   уже привилегированная операция, ей займёмся в Шаге 3.
 *
 * Сборка (на самой macOS):
 *   clang pcie_probe.c -framework IOKit -framework CoreFoundation -o pcie_probe
 * Запуск:
 *   ./pcie_probe
 *
 * Зависит только от системных фреймворков macOS. Стороннего ничего не нужно.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* PCI vendor id NVIDIA. Любая их карта начинается с этого. */
#define PCI_VENDOR_NVIDIA 0x10DE

/* PCI class code для дисплейных контроллеров (старший байт class-кода). */
#define PCI_CLASS_DISPLAY 0x03

/*
 * Прочитать 32-битное число из свойства IORegistry.
 * Возвращает true и значение через out, если свойство найдено и это число.
 */
static bool read_u32_property(io_registry_entry_t entry,
                              CFStringRef key,
                              uint32_t *out)
{
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key,
                                                    kCFAllocatorDefault, 0);
    if (!ref) {
        return false;
    }

    bool ok = false;
    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
        long long value = 0;
        if (CFNumberGetValue((CFNumberRef)ref, kCFNumberLongLongType, &value)) {
            *out = (uint32_t)value;
            ok = true;
        }
    }
    CFRelease(ref);
    return ok;
}

/*
 * Прочитать сырой блок данных из свойства IORegistry (CFData).
 * Используется для свойств вроде "assigned-addresses", где лежит
 * массив структур, описывающих BAR. Возвращает скопированный буфер
 * (caller обязан free), длину пишет в out_len.
 */
static uint8_t *read_data_property(io_registry_entry_t entry,
                                   CFStringRef key,
                                   CFIndex *out_len)
{
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key,
                                                    kCFAllocatorDefault, 0);
    if (!ref) {
        return NULL;
    }

    uint8_t *buf = NULL;
    if (CFGetTypeID(ref) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)ref;
        CFIndex len = CFDataGetLength(data);
        if (len > 0) {
            buf = (uint8_t *)malloc((size_t)len);
            if (buf) {
                memcpy(buf, CFDataGetBytePtr(data), (size_t)len);
                *out_len = len;
            }
        }
    }
    CFRelease(ref);
    return buf;
}

/*
 * Распечатать человекочитаемое имя по device-id Ada.
 * Список неполный — добавляем по мере надобности. Главное: подтвердить,
 * что чип семейства Ada Lovelace (AD10x).
 */
static const char *ada_codename(uint32_t device_id)
{
    switch (device_id) {
        /* AD102 — RTX 4090 / 4080 Super и т.д. */
        case 0x2684: return "AD102 (RTX 4090)";
        case 0x2702: return "AD103 (RTX 4080 Super)";
        case 0x2704: return "AD103 (RTX 4080)";
        /* AD104 — сюда попадает 4070 Super / 4070 Ti */
        case 0x2705: return "AD103 (RTX 4070 Ti Super)";
        case 0x2782: return "AD104 (RTX 4070 Ti)";
        case 0x2783: return "AD104 (RTX 4070 Super)";   /* целевая карта */
        case 0x2786: return "AD104 (RTX 4070)";
        /* AD106 / AD107 — младшие */
        case 0x2860: return "AD106 (RTX 4070 Max-Q)";
        case 0x2882: return "AD107 (RTX 4060)";
        default:     return NULL;
    }
}

/*
 * Разбор свойства "assigned-addresses".
 *
 * Формат: массив записей PCI address (each = 5 x uint32 в формате
 * Open Firmware / IEEE 1275):
 *   word0: phys.hi  — содержит флаги пространства + номер BAR (регистр в config space)
 *   word1: phys.mid — старшие 32 бита базового адреса
 *   word2: phys.lo  — младшие 32 бита базового адреса
 *   word3: size.hi  — старшие 32 бита размера
 *   word4: size.lo  — младшие 32 бита размера
 *
 * В phys.hi младший байт (& 0xFF) — это смещение BAR-регистра в config
 * space (0x10 = BAR0, 0x14 = BAR1, ...). Биты пространства (bits 24-25)
 * различают memory / io.
 */
static void parse_assigned_addresses(const uint8_t *buf, CFIndex len)
{
    const size_t entry_size = 5 * sizeof(uint32_t); /* 20 байт на BAR */
    size_t count = (size_t)len / entry_size;

    if (count == 0) {
        printf("  [assigned-addresses пусто или нераспознано, len=%ld]\n",
               (long)len);
        return;
    }

    const uint32_t *w = (const uint32_t *)buf;
    for (size_t i = 0; i < count; i++) {
        uint32_t phys_hi = w[i * 5 + 0];
        uint32_t addr_hi = w[i * 5 + 1];
        uint32_t addr_lo = w[i * 5 + 2];
        uint32_t size_hi = w[i * 5 + 3];
        uint32_t size_lo = w[i * 5 + 4];

        uint64_t base = ((uint64_t)addr_hi << 32) | addr_lo;
        uint64_t size = ((uint64_t)size_hi << 32) | size_lo;

        uint8_t bar_reg = (uint8_t)(phys_hi & 0xFF);
        const char *space = ((phys_hi >> 24) & 0x3) == 0x1 ? "I/O" : "MEM";
        bool prefetch = (phys_hi & (1u << 30)) != 0;

        int bar_index = -1;
        if (bar_reg >= 0x10 && bar_reg <= 0x24 && (bar_reg & 0x3) == 0) {
            bar_index = (bar_reg - 0x10) / 4;
        }

        double size_mb = (double)size / (1024.0 * 1024.0);

        printf("  BAR%d (cfg 0x%02X) %-3s %s base=0x%010llX size=0x%llX (%.1f MB)\n",
               bar_index, bar_reg, space,
               prefetch ? "pref" : "    ",
               (unsigned long long)base,
               (unsigned long long)size,
               size_mb);

        /*
         * Интерпретация для Ada:
         *   - самый большой регион (часто 256 MB) — BAR0, регистровый блок (MMIO).
         *     Через него идёт вся низкоуровневая инициализация карты.
         *   - очень большой регион (равен/кратен объёму VRAM) — BAR1, апертура VRAM.
         *   - маленький (16 MB) — может быть instance memory / прочее.
         * Точную роль подтвердим на Шаге 2 чтением сигнатурных регистров.
         */
    }
}

int main(void)
{
    printf("=== RTXmacOC :: Шаг 1 — PCIe discovery ===\n");
    printf("Ищу устройства NVIDIA (vendor 0x%04X) в IORegistry...\n\n",
           PCI_VENDOR_NVIDIA);

    /*
     * Берём ВСЕ объекты класса IOPCIDevice. Фильтровать по vendor будем
     * сами, читая свойство, — так надёжнее, чем matching-словарь по
     * приватным ключам.
     */
    CFMutableDictionaryRef matching = IOServiceMatching("IOPCIDevice");
    if (!matching) {
        fprintf(stderr, "Не удалось создать matching dictionary\n");
        return 1;
    }

    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                    matching, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%X\n", kr);
        return 1;
    }

    int nvidia_found = 0;
    io_registry_entry_t dev = IO_OBJECT_NULL;

    while ((dev = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        uint32_t vendor = 0, device = 0;

        /*
         * Ключи "vendor-id" / "device-id" публикуются IOKit как CFData
         * из 4 байт little-endian, либо как CFNumber в зависимости от
         * версии. Пробуем как число; если не вышло — читаем как data.
         */
        bool have_vendor = read_u32_property(dev, CFSTR("vendor-id"), &vendor);
        if (!have_vendor) {
            CFIndex len = 0;
            uint8_t *d = read_data_property(dev, CFSTR("vendor-id"), &len);
            if (d && len >= 2) {
                vendor = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
                have_vendor = true;
            }
            free(d);
        }

        if (!have_vendor || vendor != PCI_VENDOR_NVIDIA) {
            IOObjectRelease(dev);
            continue;
        }

        /* device-id аналогично */
        if (!read_u32_property(dev, CFSTR("device-id"), &device)) {
            CFIndex len = 0;
            uint8_t *d = read_data_property(dev, CFSTR("device-id"), &len);
            if (d && len >= 2) {
                device = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
            }
            free(d);
        }

        nvidia_found++;

        /* Имя узла в IORegistry (часто "GFX0", "display" и т.п.) */
        io_name_t name = {0};
        IORegistryEntryGetName(dev, name);

        const char *codename = ada_codename(device);

        printf("[#%d] IORegistry node: %s\n", nvidia_found, name);
        printf("  vendor-id : 0x%04X (NVIDIA)\n", vendor);
        printf("  device-id : 0x%04X%s%s\n", device,
               codename ? "  -> " : "",
               codename ? codename : "  (не из таблицы Ada — проверь чип)");

        uint32_t subsys = 0, subvendor = 0, revision = 0, classcode = 0;
        if (read_u32_property(dev, CFSTR("subsystem-id"), &subsys))
            printf("  subsystem-id      : 0x%04X\n", subsys);
        if (read_u32_property(dev, CFSTR("subsystem-vendor-id"), &subvendor))
            printf("  subsystem-vendor  : 0x%04X\n", subvendor);
        if (read_u32_property(dev, CFSTR("revision-id"), &revision))
            printf("  revision-id       : 0x%02X\n", revision);
        if (read_u32_property(dev, CFSTR("class-code"), &classcode)) {
            uint8_t base_class = (uint8_t)((classcode >> 16) & 0xFF);
            printf("  class-code        : 0x%06X%s\n", classcode,
                   base_class == PCI_CLASS_DISPLAY ? " (display controller)" : "");
        }

        /* BAR-регионы из assigned-addresses */
        printf("  --- BAR regions ---\n");
        CFIndex aa_len = 0;
        uint8_t *aa = read_data_property(dev, CFSTR("assigned-addresses"), &aa_len);
        if (aa) {
            parse_assigned_addresses(aa, aa_len);
            free(aa);
        } else {
            printf("  [нет свойства assigned-addresses — карта не сконфигурирована BIOS/EFI?]\n");
        }

        printf("\n");
        IOObjectRelease(dev);
    }

    IOObjectRelease(iter);

    if (nvidia_found == 0) {
        printf("NVIDIA-устройств не найдено.\n");
        printf("Если карта физически стоит — проверь, что ты в macOS и\n");
        printf("что EFI/BIOS назначил карте ресурсы (она видна в lspci/IORegistryExplorer).\n");
        return 2;
    }

    printf("Готово. Найдено NVIDIA-устройств: %d\n", nvidia_found);
    printf("Следующий шаг: интерпретировать BAR0 как регистровый блок Ada\n");
    printf("и подготовить DriverKit-драйвер для его маппинга.\n");
    return 0;
}
