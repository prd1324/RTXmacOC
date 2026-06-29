/*
 * booter_run_linux.c — пробный dry-load Booter на SEC2 через VFIO (Linux, headless).
 *
 * Задача 6, фаза 3: проверить путь SEC2 на реальном железе — база регистров SEC2
 * (0x840000), reset, DMA-load HS-ucode, верификация подписи BROM, boot. GSP-RM ещё
 * НЕ подготовлен (нет WprMeta/radix3), поэтому WPR-handle в mailbox = 0 (dummy):
 * Booter штатно завершится с mbox0 != 0 (ошибка «нет валидной WPR meta»), и это
 * ОЖИДАЕМЫЙ результат — он доказывает, что reset+DMA+BROM(подпись)+boot на SEC2
 * работают. Полная загрузка GSP-RM — фазы 4-6.
 *
 * Метрика фазы 3: SEC2 HWCFG2 читается (не 0xBADF), reset OK, dma_load OK, falcon
 * дошёл до halted (boot прошёл BROM). Значение mbox0 фиксируем (ошибка ожидаема).
 *
 * Сборка:  make booter-run-linux
 * Запуск (root, RTX на vfio-pci):  sudo ./tools/booter_run_linux [pci-bdf]
 * Прогон без ребута: tools/run-booter-detached.sh (как для FWSEC).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>

#include "../driver/gsp/falcon.h"
#include "../driver/gsp/booter.h"
#include "../driver/gsp/fw_blob.h"

#define FWSEC_TIMEOUT_US (2u * 1000u * 1000u)
#define DMA_IOVA 0x10000000ULL
#define DMA_CAP  0x100000u   /* 1 MiB — booter data-region ~55 КБ */

/* ===================== glue: nv_mmio_t поверх mmap BAR0 ===================== */
static uint32_t bar_rd(void *ctx, uint32_t off)
{ return *(volatile uint32_t *)((volatile uint8_t *)ctx + off); }
static void bar_wr(void *ctx, uint32_t off, uint32_t val)
{ *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = val; }
static void bar_udelay(void *ctx, uint32_t usec)
{
    (void)ctx;
    struct timespec ts = { .tv_sec = usec / 1000000u,
                           .tv_nsec = (long)(usec % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}

/* ===================== VFIO setup (как в fwsec_run_linux.c) ===================== */
struct vfio_ctx {
    int container, group, device;
    volatile void *bar0; size_t bar0_size;
    int cfg_off_valid; uint64_t cfg_off;
};

static int read_iommu_group(const char *bdf, char *out, size_t outlen)
{
    char link[256], target[256];
    snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n < 0) { fprintf(stderr, "readlink(%s): %s\n", link, strerror(errno)); return -1; }
    target[n] = '\0';
    const char *base = strrchr(target, '/'); base = base ? base + 1 : target;
    snprintf(out, outlen, "%s", base);
    return 0;
}

static int vfio_open(struct vfio_ctx *v, const char *bdf)
{
    memset(v, 0, sizeof(*v));
    v->container = v->group = v->device = -1;
    char grpid[64];
    if (read_iommu_group(bdf, grpid, sizeof(grpid)) != 0) return -1;
    printf("VFIO: устройство %s, IOMMU-группа %s\n", bdf, grpid);

    v->container = open("/dev/vfio/vfio", O_RDWR);
    if (v->container < 0) { perror("open /dev/vfio/vfio"); return -1; }
    if (ioctl(v->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        fprintf(stderr, "VFIO: неподдерживаемая версия API\n"); return -1; }
    if (!ioctl(v->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        fprintf(stderr, "VFIO: нет TYPE1_IOMMU (intel_iommu=on?)\n"); return -1; }

    char grppath[80];
    snprintf(grppath, sizeof(grppath), "/dev/vfio/%s", grpid);
    v->group = open(grppath, O_RDWR);
    if (v->group < 0) { perror("open group"); return -1; }
    struct vfio_group_status gs = { .argsz = sizeof(gs) };
    if (ioctl(v->group, VFIO_GROUP_GET_STATUS, &gs)) { perror("GROUP_GET_STATUS"); return -1; }
    if (!(gs.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        fprintf(stderr, "VFIO: группа не viable\n"); return -1; }
    if (ioctl(v->group, VFIO_GROUP_SET_CONTAINER, &v->container)) { perror("SET_CONTAINER"); return -1; }
    if (ioctl(v->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) { perror("SET_IOMMU"); return -1; }

    v->device = ioctl(v->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (v->device < 0) { perror("GET_DEVICE_FD"); return -1; }

    struct vfio_region_info reg = { .argsz = sizeof(reg), .index = VFIO_PCI_BAR0_REGION_INDEX };
    if (ioctl(v->device, VFIO_DEVICE_GET_REGION_INFO, &reg)) { perror("REGION_INFO BAR0"); return -1; }
    if (!(reg.flags & VFIO_REGION_INFO_FLAG_MMAP)) { fprintf(stderr, "BAR0 не mmap-able\n"); return -1; }
    v->bar0_size = (size_t)reg.size;
    v->bar0 = mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED, v->device, reg.offset);
    if (v->bar0 == MAP_FAILED) { perror("mmap BAR0"); v->bar0 = NULL; return -1; }
    printf("VFIO: BAR0 смаплен, size=0x%zx\n", v->bar0_size);

    struct vfio_region_info creg = { .argsz = sizeof(creg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    if (ioctl(v->device, VFIO_DEVICE_GET_REGION_INFO, &creg) == 0) {
        v->cfg_off = creg.offset; v->cfg_off_valid = 1; }
    return 0;
}

static void vfio_enable_busmaster(struct vfio_ctx *v)
{
    if (!v->cfg_off_valid) return;
    uint16_t cmd = 0;
    if (pread(v->device, &cmd, sizeof(cmd), v->cfg_off + 0x04) == (ssize_t)sizeof(cmd)) {
        cmd |= 0x0002 | 0x0004; /* MEMORY_SPACE | BUS_MASTER */
        if (pwrite(v->device, &cmd, sizeof(cmd), v->cfg_off + 0x04) == (ssize_t)sizeof(cmd))
            printf("VFIO: Memory Space + Bus Master включены (CMD=0x%04x)\n", cmd);
    }
}

static int vfio_map_dma(struct vfio_ctx *v, void *vaddr, uint64_t iova, size_t size)
{
    struct vfio_iommu_type1_dma_map map = {
        .argsz = sizeof(map), .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uint64_t)(uintptr_t)vaddr, .iova = iova, .size = size };
    if (ioctl(v->container, VFIO_IOMMU_MAP_DMA, &map)) { perror("IOMMU_MAP_DMA"); return -1; }
    return 0;
}

static void vfio_close(struct vfio_ctx *v)
{
    if (v->bar0) munmap((void *)v->bar0, v->bar0_size);
    if (v->device >= 0) close(v->device);
    if (v->group >= 0) close(v->group);
    if (v->container >= 0) close(v->container);
}

/* ===================== Прогон Booter dry-load ===================== */
static int run_booter(const nv_mmio_t *io, uint64_t dma_iova, uint8_t *dma_buf, size_t dma_cap)
{
    uint32_t boot0 = bar_rd(io->ctx, 0x0);
    printf("PMC_BOOT_0 = 0x%08x\n", boot0);

    /* GFW boot должен быть завершён (как для FWSEC) до работы с движками. */
    int gfw = nv_wait_gfw_boot_completed(io, 4u * 1000u * 1000u);
    printf("GFW boot completed rc=%d (raw=0x%08x)\n", gfw, nv_gfw_boot_raw(io));
    if (gfw != NV_OK) { fprintf(stderr, "FAIL: GFW boot не завершился\n"); return -1; }

    /* Проверка базы SEC2: HWCFG2 не должен быть priv-фолтом (0xBADFxxxx). */
    uint32_t sec2_hwcfg2 = bar_rd(io->ctx, NV_PSEC_FALCON_BASE + NV_PFALCON_FALCON_HWCFG2_OFF);
    printf("SEC2 HWCFG2 (@0x%x) = 0x%08x\n",
           NV_PSEC_FALCON_BASE + NV_PFALCON_FALCON_HWCFG2_OFF, sec2_hwcfg2);
    if ((sec2_hwcfg2 & 0xFFFF0000u) == 0xBADF0000u) {
        fprintf(stderr, "FAIL: SEC2 HWCFG2 = priv-фолт → база SEC2 неверна или движок недоступен\n");
        return -1;
    }

    /* Загрузить и разобрать booter_load. */
    uint8_t *blob = NULL; size_t blob_len = 0;
    if (nv_fw_blob_get("booter_load", &blob, &blob_len) != NV_FW_BLOB_OK) {
        fprintf(stderr, "FAIL: не загрузить booter_load (zstd? путь RTX_FW_DIR?)\n"); return -1; }
    printf("booter_load: %zu байт\n", blob_len);

    nv_booter_desc_t d;
    if (nv_booter_parse(blob, blob_len, &d) != NV_BOOTER_OK) {
        fprintf(stderr, "FAIL: разбор booter_load\n"); nv_fw_blob_free(blob); return -1; }
    printf("Booter: data_abs=%u data_size=%u num_sig=%u engine_id_mask=0x%x ucode_id=%u\n",
           d.data_abs, d.data_size, d.num_sig, d.engine_id_mask, d.ucode_id);
    printf("  imem_sec: src=%u len=%u (app0); dmem: src=%u len=%u; bootvec=%u; pkc=%u\n",
           d.app[0].code_offset, d.app[0].code_size, d.os_data_offset, d.os_data_size,
           d.app[0].code_offset, d.pkc_data_offset);

    if (d.data_size > dma_cap) { fprintf(stderr, "FAIL: data > DMA\n"); nv_fw_blob_free(blob); return -1; }

    /* Fuse-версия SEC2 → выбор подписи. */
    uint32_t reg_fuse = 0xFFFFFFFFu;
    if (nv_falcon_signature_fuse_version_ga102(io, (uint16_t)d.engine_id_mask,
                                               (uint8_t)d.ucode_id, &reg_fuse) != NV_OK) {
        fprintf(stderr, "FAIL: чтение SEC2 fuse-версии\n"); nv_fw_blob_free(blob); return -1; }
    uint32_t sig_idx = 0;
    if (nv_booter_select_signature(&d, reg_fuse, &sig_idx) != NV_BOOTER_OK) {
        fprintf(stderr, "FAIL: выбор подписи (fuse_ver=%u reg_fuse=%u num_sig=%u)\n",
                d.sig_fuse_ver, reg_fuse, d.num_sig); nv_fw_blob_free(blob); return -1; }
    size_t sig_at = (size_t)d.sig_prod_abs + (size_t)sig_idx * NV_BOOTER_SIG_SIZE;
    printf("signature: reg_fuse=%u fuse_ver=%u → idx=%u @blob+%zu\n",
           reg_fuse, d.sig_fuse_ver, sig_idx, sig_at);
    if (sig_at + NV_BOOTER_SIG_SIZE > blob_len) {
        fprintf(stderr, "FAIL: подпись вне блоба\n"); nv_fw_blob_free(blob); return -1; }

    /* DMA-буфер = регион данных; патч подписи в patch_loc (data-relative). */
    memset(dma_buf, 0, dma_cap);
    memcpy(dma_buf, blob + d.data_abs, d.data_size);
    if ((size_t)d.patch_loc + NV_BOOTER_SIG_SIZE > d.data_size) {
        fprintf(stderr, "FAIL: patch_loc вне data\n"); nv_fw_blob_free(blob); return -1; }
    memcpy(dma_buf + d.patch_loc, blob + sig_at, NV_BOOTER_SIG_SIZE);

    uint32_t engine_id_mask = d.engine_id_mask;
    uint8_t  ucode_id = (uint8_t)d.ucode_id;
    uint32_t pkc = d.pkc_data_offset;
    nv_dma_load_target_t imem_sec = { d.app[0].code_offset, 0, d.app[0].code_size };
    nv_dma_load_target_t dmem     = { d.os_data_offset,     0, d.os_data_size };
    uint32_t bootvec = d.app[0].code_offset;
    nv_fw_blob_free(blob);

    /* reset SEC2. */
    if (nv_falcon_reset_ga102(io, NV_PSEC_FALCON_BASE, NV_PSEC_FALCON2_BASE,
                              boot0, FWSEC_TIMEOUT_US) != NV_OK) {
        fprintf(stderr, "FAIL: reset SEC2 (HWCFG2=0x%08x)\n",
                bar_rd(io->ctx, NV_PSEC_FALCON_BASE + NV_PFALCON_FALCON_HWCFG2_OFF));
        return -1;
    }
    printf("SEC2 reset OK\n");

    if (nv_falcon_dma_load_ga102(io, NV_PSEC_FALCON_BASE, NV_PSEC_FALCON2_BASE,
                                 dma_iova, &imem_sec, &dmem,
                                 pkc, (uint16_t)engine_id_mask, ucode_id,
                                 bootvec, FWSEC_TIMEOUT_US) != NV_OK) {
        fprintf(stderr, "FAIL: dma_load SEC2 (DMATRFCMD=0x%08x)\n",
                bar_rd(io->ctx, NV_PSEC_FALCON_BASE + NV_PFALCON_FALCON_DMATRFCMD_OFF));
        return -1;
    }
    printf("SEC2 dma_load OK\n");

    /* boot: WPR-handle в mbox0/mbox1 = 0 (dummy, GSP-RM не подготовлен). */
    uint32_t mbox0 = 0xFFFFFFFFu;
    int brc = nv_falcon_boot(io, NV_PSEC_FALCON_BASE, 1, 0, 1, 0, FWSEC_TIMEOUT_US, &mbox0);
    uint32_t mbox1 = nv_falcon_read_mailbox1(io, NV_PSEC_FALCON_BASE);
    printf("Booter boot: rc=%d mbox0=0x%08x mbox1=0x%08x\n", brc, mbox0, mbox1);

    if (brc != NV_OK) {
        fprintf(stderr, "Booter не дошёл до halted (timeout) — BROM отверг подпись или фолт.\n");
        return -1;
    }
    /* Halted достигнут → reset+DMA+BROM(подпись)+boot на SEC2 работают.
       mbox0 != 0 ОЖИДАЕМ (нет валидной WPR meta). mbox0==0 было бы сюрпризом. */
    printf("\n*** SEC2-путь Booter подтверждён: reset+dma_load+BROM+boot OK (halted). "
           "mbox0=0x%08x %s ***\n", mbox0,
           mbox0 == 0 ? "(неожиданно 0 — проверить!)" : "(ошибка ожидаема: нет WPR meta)");
    return 0;
}

/* ===================== авто-детект BDF NVIDIA ===================== */
static int find_nvidia_bdf(char *out, size_t outlen)
{
    DIR *dd = opendir("/sys/bus/pci/devices");
    if (!dd) return -1;
    struct dirent *e; char best[40] = {0};
    while ((e = readdir(dd)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[300], buf[32]; FILE *f; unsigned vendor = 0, cls = 0;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fgets(buf, sizeof(buf), f)) vendor = (unsigned)strtoul(buf, NULL, 16);
        fclose(f);
        if (vendor != 0x10de) continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fgets(buf, sizeof(buf), f)) cls = (unsigned)strtoul(buf, NULL, 16);
        fclose(f);
        if ((cls >> 16) == 0x03) {
            if ((cls >> 8) == 0x0300) { snprintf(out, outlen, "%s", e->d_name); closedir(dd); return 0; }
            if (best[0] == 0) snprintf(best, sizeof(best), "%s", e->d_name);
        }
    }
    closedir(dd);
    if (best[0]) { snprintf(out, outlen, "%s", best); return 0; }
    return -1;
}

int main(int argc, char **argv)
{
    char auto_bdf[40];
    const char *bdf = (argc >= 2) ? argv[1] : NULL;
    if (!bdf) {
        if (find_nvidia_bdf(auto_bdf, sizeof(auto_bdf)) == 0) { bdf = auto_bdf;
            printf("BDF (авто-детект): %s\n", bdf);
        } else { fprintf(stderr, "usage: %s [pci-bdf]\n", argv[0]); return 2; }
    }

    struct vfio_ctx v;
    if (vfio_open(&v, bdf) != 0) { vfio_close(&v); return 1; }
    vfio_enable_busmaster(&v);

    void *dma_buf = mmap(NULL, DMA_CAP, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dma_buf == MAP_FAILED) { perror("mmap dma_buf"); vfio_close(&v); return 1; }
    if (vfio_map_dma(&v, dma_buf, DMA_IOVA, DMA_CAP) != 0) {
        munmap(dma_buf, DMA_CAP); vfio_close(&v); return 1; }
    printf("DMA-буфер: VA=%p IOVA=0x%llx size=0x%x\n",
           dma_buf, (unsigned long long)DMA_IOVA, DMA_CAP);

    nv_mmio_t io = { .ctx = (void *)v.bar0, .rd = bar_rd, .wr = bar_wr, .udelay = bar_udelay };
    int rc = run_booter(&io, DMA_IOVA, (uint8_t *)dma_buf, DMA_CAP);

    struct vfio_iommu_type1_dma_unmap unmap = {
        .argsz = sizeof(unmap), .iova = DMA_IOVA, .size = DMA_CAP };
    ioctl(v.container, VFIO_IOMMU_UNMAP_DMA, &unmap);
    munmap(dma_buf, DMA_CAP);
    vfio_close(&v);

    printf("\n=== РЕЗУЛЬТАТ: %s ===\n",
           rc == 0 ? "OK (SEC2-путь Booter работает; mbox0-ошибка ожидаема)" : "FAIL (см. лог)");
    return rc == 0 ? 0 : 1;
}
