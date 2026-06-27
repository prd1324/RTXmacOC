/*
 * falcon.h — примитивы микроконтроллера Falcon (GSP) для bring-up (слой 2).
 *
 * Реализует последовательность из nova-core falcon.rs поверх абстрактного
 * MMIO-интерфейса, чтобы один и тот же код работал в:
 *   - kext  (через IOPCIDevice + смапленный BAR0),
 *   - dext  (через MemoryRead32/MemoryWrite32),
 *   - тесте (через буфер-заглушку).
 *
 * Что покрыто (только MMIO, без DMA):
 *   reset движка, ожидание окончания скраббинга памяти, старт CPU, ожидание
 *   halt, mailbox, проверка WPR2 и прогресса GFW boot.
 * DMA-загрузка ucode (dma_wr) требует kernel DMA-буферов — отдельный шаг.
 *
 * Источник последовательности/регистров: nova-core falcon.rs / regs.rs / gsp.rs.
 * Пересказано для соответствия лицензии.
 */
#ifndef RTXMACOC_FALCON_H
#define RTXMACOC_FALCON_H

#include <stdint.h>
#include "../../falcon_regs.h"

/*
 * Абстракция доступа к регистрам GPU (BAR0) + задержка.
 *   rd/wr  — чтение/запись 32-бит по абсолютному смещению в BAR0.
 *   udelay — пауза в микросекундах (для poll-циклов и reset).
 *   ctx    — контекст реализации (IOPCIDevice*, указатель на буфер и т.п.).
 */
typedef struct {
    void     *ctx;
    uint32_t (*rd)(void *ctx, uint32_t off);
    void     (*wr)(void *ctx, uint32_t off, uint32_t val);
    void     (*udelay)(void *ctx, uint32_t usec);
} nv_mmio_t;

/* Коды возврата. */
#define NV_OK         0
#define NV_ERR_TIMEOUT (-1)
#define NV_ERR_STATE   (-2)

/* Сброс движка Falcon (regs.rs reset_engine): reset=1, ждать, reset=0. */
void nv_falcon_reset_engine(const nv_mmio_t *io, uint32_t base);

/*
 * Ждать завершения скраббинга IMEM/DMEM. На Ampere+ — бит HWCFG2.mem_scrubbing
 * становится 0; дополнительно проверяем DMACTL scrubbing-биты. Возвращает
 * NV_OK или NV_ERR_TIMEOUT.
 */
int nv_falcon_wait_mem_scrubbing(const nv_mmio_t *io, uint32_t base,
                                 uint32_t timeout_us);

/* Полный reset: сброс движка + ожидание скраба. */
int nv_falcon_reset(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us);

/* Старт CPU Falcon (falcon.rs start): через CPUCTL_ALIAS или CPUCTL. */
void nv_falcon_start(const nv_mmio_t *io, uint32_t base);

/* Ждать, пока CPU встанет в halt (CPUCTL.halted). */
int nv_falcon_wait_halted(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us);

/* Mailbox. */
void     nv_falcon_write_mailbox0(const nv_mmio_t *io, uint32_t base, uint32_t v);
void     nv_falcon_write_mailbox1(const nv_mmio_t *io, uint32_t base, uint32_t v);
uint32_t nv_falcon_read_mailbox0(const nv_mmio_t *io, uint32_t base);
uint32_t nv_falcon_read_mailbox1(const nv_mmio_t *io, uint32_t base);

/*
 * Boot (falcon.rs boot): записать mailbox0/1 (если want_mbox*), стартовать CPU,
 * дождаться halt, вернуть mbox0 через out_mbox0. NV_OK если дождались.
 */
int nv_falcon_boot(const nv_mmio_t *io, uint32_t base,
                   int set_mbox0, uint32_t mbox0,
                   int set_mbox1, uint32_t mbox1,
                   uint32_t timeout_us, uint32_t *out_mbox0);

/* Проверка результата FWSEC-FRTS: WPR2 задан (ADDR_HI != 0). */
int nv_wpr2_is_set(const nv_mmio_t *io, uint64_t *out_lo, uint64_t *out_hi);

/* Прогресс GFW boot завершён (0xFF). */
int nv_gfw_boot_completed(const nv_mmio_t *io);

#endif /* RTXMACOC_FALCON_H */
