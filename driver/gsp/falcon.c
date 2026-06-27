/*
 * falcon.c — реализация примитивов Falcon (см. falcon.h).
 *
 * Последовательности портированы из nova-core falcon.rs / regs.rs:
 *   - reset_engine: NV_PFALCON_FALCON_ENGINE.reset = 1 -> delay -> 0;
 *   - wait_mem_scrubbing: HWCFG2.mem_scrubbing == 0 (Ampere+);
 *   - start: если CPUCTL.alias_en -> CPUCTL_ALIAS.startcpu, иначе CPUCTL.startcpu;
 *   - wait_till_halted: CPUCTL.halted;
 *   - boot: write mailboxes -> start -> wait halted -> read mbox0.
 * Пересказано для соответствия лицензии.
 *
 * Только MMIO. DMA-загрузка ucode — отдельный шаг (нужны kernel DMA-буферы).
 */
#include "falcon.h"

/* Шаг опроса в poll-циклах. */
#define NV_POLL_STEP_US 10u

static inline uint32_t rd(const nv_mmio_t *io, uint32_t off) { return io->rd(io->ctx, off); }
static inline void     wr(const nv_mmio_t *io, uint32_t off, uint32_t v) { io->wr(io->ctx, off, v); }
static inline void     udelay(const nv_mmio_t *io, uint32_t us) { if (io->udelay) io->udelay(io->ctx, us); }

void nv_falcon_reset_engine(const nv_mmio_t *io, uint32_t base)
{
    uint32_t eng = PFALCON(base, NV_PFALCON_FALCON_ENGINE_OFF);
    uint32_t v = rd(io, eng);
    wr(io, eng, v | NV_PFALCON_ENGINE_RESET);
    /* falcon.rs: сброс движка занимает не более ~10us. */
    udelay(io, 10);
    v = rd(io, eng);
    wr(io, eng, v & ~NV_PFALCON_ENGINE_RESET);
    udelay(io, 10);
}

int nv_falcon_wait_mem_scrubbing(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us)
{
    uint32_t hwcfg2 = PFALCON(base, NV_PFALCON_FALCON_HWCFG2_OFF);
    uint32_t waited = 0;
    for (;;) {
        uint32_t v = rd(io, hwcfg2);
        /* HWCFG2.mem_scrubbing == 0 -> скраб завершён (regs.rs mem_scrubbing_done). */
        if ((v & NV_PFALCON_HWCFG2_MEM_SCRUBBING) == 0)
            return NV_OK;
        if (waited >= timeout_us)
            return NV_ERR_TIMEOUT;
        udelay(io, NV_POLL_STEP_US);
        waited += NV_POLL_STEP_US;
    }
}

int nv_falcon_reset(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us)
{
    nv_falcon_reset_engine(io, base);
    return nv_falcon_wait_mem_scrubbing(io, base, timeout_us);
}

void nv_falcon_start(const nv_mmio_t *io, uint32_t base)
{
    uint32_t cpuctl = PFALCON(base, NV_PFALCON_FALCON_CPUCTL_OFF);
    uint32_t v = rd(io, cpuctl);
    if (v & NV_PFALCON_CPUCTL_ALIAS_EN) {
        uint32_t alias = PFALCON(base, NV_PFALCON_FALCON_CPUCTL_ALIAS_OFF);
        wr(io, alias, NV_PFALCON_CPUCTL_ALIAS_STARTCPU);
    } else {
        wr(io, cpuctl, NV_PFALCON_CPUCTL_STARTCPU);
    }
}

int nv_falcon_wait_halted(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us)
{
    uint32_t cpuctl = PFALCON(base, NV_PFALCON_FALCON_CPUCTL_OFF);
    uint32_t waited = 0;
    for (;;) {
        if (rd(io, cpuctl) & NV_PFALCON_CPUCTL_HALTED)
            return NV_OK;
        if (waited >= timeout_us)
            return NV_ERR_TIMEOUT;
        udelay(io, NV_POLL_STEP_US);
        waited += NV_POLL_STEP_US;
    }
}

void nv_falcon_write_mailbox0(const nv_mmio_t *io, uint32_t base, uint32_t v)
{
    wr(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX0_OFF), v);
}
void nv_falcon_write_mailbox1(const nv_mmio_t *io, uint32_t base, uint32_t v)
{
    wr(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX1_OFF), v);
}
uint32_t nv_falcon_read_mailbox0(const nv_mmio_t *io, uint32_t base)
{
    return rd(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX0_OFF));
}
uint32_t nv_falcon_read_mailbox1(const nv_mmio_t *io, uint32_t base)
{
    return rd(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX1_OFF));
}

int nv_falcon_boot(const nv_mmio_t *io, uint32_t base,
                   int set_mbox0, uint32_t mbox0,
                   int set_mbox1, uint32_t mbox1,
                   uint32_t timeout_us, uint32_t *out_mbox0)
{
    if (set_mbox0) nv_falcon_write_mailbox0(io, base, mbox0);
    if (set_mbox1) nv_falcon_write_mailbox1(io, base, mbox1);

    nv_falcon_start(io, base);

    int rc = nv_falcon_wait_halted(io, base, timeout_us);
    if (out_mbox0)
        *out_mbox0 = nv_falcon_read_mailbox0(io, base);
    return rc;
}

int nv_wpr2_is_set(const nv_mmio_t *io, uint64_t *out_lo, uint64_t *out_hi)
{
    uint32_t lo = rd(io, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    uint32_t hi = rd(io, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    if (out_lo) *out_lo = nv_wpr2_bound_from_reg(lo);
    if (out_hi) *out_hi = nv_wpr2_bound_from_reg(hi);
    /* regs.rs is_wpr2_set: hi_val != 0. */
    return (hi >> 4) != 0;
}

int nv_gfw_boot_completed(const nv_mmio_t *io)
{
    uint32_t v = rd(io, NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
    return (v & 0xFFu) == NV_GFW_BOOT_PROGRESS_COMPLETED;
}
