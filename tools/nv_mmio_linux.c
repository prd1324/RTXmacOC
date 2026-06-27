/*
 * nv_mmio_linux.c — проверка чтения NV_PMC_BOOT_0 с РЕАЛЬНОЙ карты БЕЗ macOS.
 *
 * Зачем: самое неподтверждённое в проекте — раскладка регистра PMC_BOOT_0
 * (вернёт ли Ada arch=0x19). Эту гипотезу можно проверить из Linux live-USB,
 * не трогая Windows и не имея Мака. Если здесь выйдет Ada AD104 — значит и
 * наш kext/dext на macOS прочитает то же самое (декодер общий — ada_regs.h).
 *
 * Что делает:
 *   1. Находит NVIDIA-устройство в /sys/bus/pci/devices (vendor 0x10de, class 0x03).
 *   2. Включает устройство и mmap'ит BAR0 (resource0).
 *   3. Читает offset 0x0 (NV_PMC_BOOT_0) и декодит architecture/implementation.
 *
 * Это user-space MMIO ровно как делает драйвер, только на Linux. Платформа
 * разработки иная, но ЖЕЛЕЗО и регистр те же — для проверки декодера достаточно.
 *
 * Требует root и чтобы BAR не держал другой драйвер. На live-USB грузиться с
 *   nouveau.modeset=0 modprobe.blacklist=nouveau,nvidia,nvidia_drm
 * (тогда resource0 свободен). Иначе временно отвязать:
 *   echo 0000:01:00.0 | sudo tee /sys/bus/pci/drivers/nouveau/unbind
 *
 * Сборка (на Linux):  cc nv_mmio_linux.c -o nv_mmio_linux
 * Запуск:             sudo ./nv_mmio_linux            (автопоиск карты)
 *                     sudo ./nv_mmio_linux 0000:01:00.0   (явный BDF)
 *
 * Windows при этом не затрагивается вообще — Linux запускается с флешки.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../ada_regs.h"

#define PCI_VENDOR_NVIDIA 0x10de
#define SYS_PCI "/sys/bus/pci/devices"
#define MAP_SPAN 0x1000u  /* достаточно одной страницы: PMC_BOOT_0 в offset 0 */

/* Прочитать hex-число из sysfs-файла (vendor/device/class содержат "0x...."). */
static int read_hex_file(const char *path, unsigned long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    unsigned long v = 0;
    int n = fscanf(f, "%lx", &v);
    fclose(f);
    if (n != 1) {
        return -1;
    }
    *out = v;
    return 0;
}

/* Найти первый NVIDIA-дисплейный девайс. Пишет BDF в out (size>=32). */
static int find_nvidia_bdf(char *out, size_t out_sz)
{
    DIR *d = opendir(SYS_PCI);
    if (!d) {
        perror("opendir " SYS_PCI);
        return -1;
    }

    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }

        char path[512];
        unsigned long vendor = 0, classcode = 0;

        snprintf(path, sizeof(path), SYS_PCI "/%s/vendor", e->d_name);
        if (read_hex_file(path, &vendor) != 0 || vendor != PCI_VENDOR_NVIDIA) {
            continue;
        }

        snprintf(path, sizeof(path), SYS_PCI "/%s/class", e->d_name);
        if (read_hex_file(path, &classcode) != 0) {
            continue;
        }
        /* class = 0x03xxxx → display controller (старший байт 0x03). */
        if (((classcode >> 16) & 0xFF) != 0x03) {
            continue;
        }

        snprintf(out, out_sz, "%s", e->d_name);
        found = 0;
        break;
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    char bdf[64] = {0};

    if (argc >= 2) {
        snprintf(bdf, sizeof(bdf), "%s", argv[1]);
    } else if (find_nvidia_bdf(bdf, sizeof(bdf)) != 0) {
        fprintf(stderr, "NVIDIA display device не найден в %s\n", SYS_PCI);
        fprintf(stderr, "Передай BDF явно: sudo %s 0000:01:00.0\n", argv[0]);
        return 2;
    }

    printf("=== RTXmacOC :: проверка PMC_BOOT_0 на реальном железе (Linux) ===\n");
    printf("PCI device: %s\n", bdf);

    char path[512];
    unsigned long vendor = 0, device = 0, classcode = 0;
    snprintf(path, sizeof(path), SYS_PCI "/%s/vendor", bdf);
    read_hex_file(path, &vendor);
    snprintf(path, sizeof(path), SYS_PCI "/%s/device", bdf);
    read_hex_file(path, &device);
    snprintf(path, sizeof(path), SYS_PCI "/%s/class", bdf);
    read_hex_file(path, &classcode);
    printf("vendor=0x%04lx device=0x%04lx class=0x%06lx\n",
           vendor, device, classcode);

    /* Включаем устройство (memory space), иначе BAR может быть недоступен. */
    snprintf(path, sizeof(path), SYS_PCI "/%s/enable", bdf);
    int ef = open(path, O_WRONLY);
    if (ef >= 0) {
        if (write(ef, "1", 1) < 0) {
            perror("warn: enable write");
        }
        close(ef);
    }

    /* Мапим BAR0 = resource0. */
    snprintf(path, sizeof(path), SYS_PCI "/%s/resource0", bdf);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        fprintf(stderr, "Нужен root и свободный BAR0. Отвяжи драйвер:\n");
        fprintf(stderr, "  echo %s | sudo tee /sys/bus/pci/drivers/nouveau/unbind\n", bdf);
        return 3;
    }

    volatile void *map = mmap(NULL, MAP_SPAN, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap resource0: %s\n", strerror(errno));
        close(fd);
        return 4;
    }

    /* Читаем NV_PMC_BOOT_0 (offset 0x0). */
    uint32_t boot0 = *((volatile uint32_t *)((const char *)map + NV_PMC_BOOT_0));
    printf("NV_PMC_BOOT_0 = 0x%08x\n", boot0);

    if (!nv_boot0_plausible(boot0)) {
        printf("Значение неправдоподобно (карта не запитана / BAR держит драйвер).\n");
    } else {
        nv_boot0_t b = nv_decode_boot0(boot0);
        printf("arch=0x%02x impl=0x%x chipset=0x%03x rev=%u.%u -> %s / %s\n",
               b.architecture, b.implementation, b.chipset,
               b.major_revision, b.minor_revision,
               nv_arch_name(b.architecture), nv_chipset_name(b.chipset));
        if (b.architecture == NV_ARCHITECTURE_AD) {
            printf("*** Декодер подтверждён: Ada %s (chipset 0x%03x). ***\n",
                   nv_chipset_name(b.chipset), b.chipset);
            printf("Значит kext/dext на macOS прочитают то же — раскладка полей верна.\n");
        } else {
            printf("arch != 0x19. Раскладку полей в ada_regs.h нужно поправить\n");
            printf("(сверить с dev_boot.h из open-gpu-kernel-modules).\n");
        }
    }

    munmap((void *)map, MAP_SPAN);
    close(fd);
    return 0;
}
