/*
 * gsp_rpc.c — реализация очередей сообщений GSP-RM (см. gsp_rpc.h).
 * Порт nouveau r535_gsp_shared_init: PTE-таблица + cmdq + msgq, заголовки очередей.
 */
#include "gsp_rpc.h"

#define GSP_PAGE 0x1000u

static void st32(uint8_t *p, uint32_t v)
{ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void st64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

int nv_gsp_shm_compute(nv_gsp_shm_layout_t *out)
{
    if (!out) return NV_GSP_RPC_ERR_ARG;
    uint32_t nr = (NV_GSP_QUEUE_SIZE + NV_GSP_QUEUE_SIZE) / GSP_PAGE; /* страниц cmdq+msgq */
    nr += (uint32_t)(((uint64_t)nr * 8u + GSP_PAGE - 1) / GSP_PAGE);  /* + страницы под сами PTE */
    uint64_t ptes_size = (((uint64_t)nr * 8u) + GSP_PAGE - 1) & ~(uint64_t)(GSP_PAGE - 1);
    out->ptes_nr   = nr;
    out->ptes_size = ptes_size;
    out->cmdq_off  = ptes_size;
    out->msgq_off  = ptes_size + NV_GSP_QUEUE_SIZE;
    out->total_size= ptes_size + (uint64_t)NV_GSP_QUEUE_SIZE * 2u;
    out->msg_count = (NV_GSP_QUEUE_SIZE - NV_GSP_QUEUE_ENTRYOFF) / NV_GSP_QUEUE_MSGSIZE;
    return NV_GSP_RPC_OK;
}

static void init_queue_hdr(uint8_t *q, uint32_t msg_count)
{
    st32(q + NV_MSGQ_TX_VERSION_OFF,  0u);
    st32(q + NV_MSGQ_TX_SIZE_OFF,     NV_GSP_QUEUE_SIZE);
    st32(q + NV_MSGQ_TX_MSGSIZE_OFF,  NV_GSP_QUEUE_MSGSIZE);
    st32(q + NV_MSGQ_TX_MSGCOUNT_OFF, msg_count);
    st32(q + NV_MSGQ_TX_WRITEPTR_OFF, 0u);
    st32(q + NV_MSGQ_TX_FLAGS_OFF,    1u);
    st32(q + NV_MSGQ_TX_RXHDROFF_OFF, NV_GSP_MSGQ_RXHDROFF);
    st32(q + NV_MSGQ_TX_ENTRYOFF_OFF, NV_GSP_QUEUE_ENTRYOFF);
    st32(q + NV_GSP_MSGQ_RXHDROFF,    0u); /* rx.readPtr = 0 */
}

int nv_gsp_shm_init(uint8_t *buf, size_t buflen, uint64_t dma_base, nv_gsp_shm_layout_t *out)
{
    if (!buf || !out) return NV_GSP_RPC_ERR_ARG;
    nv_gsp_shm_compute(out);
    if (out->total_size > buflen) return NV_GSP_RPC_ERR_BOUNDS;

    for (size_t i = 0; i < out->total_size; i++) buf[i] = 0;
    /* PTE-таблица: страница за страницей весь регион. */
    for (uint32_t i = 0; i < out->ptes_nr; i++)
        st64(buf + (size_t)i * 8u, dma_base + (uint64_t)i * GSP_PAGE);
    /* Заголовки очередей. */
    init_queue_hdr(buf + out->cmdq_off, out->msg_count);
    init_queue_hdr(buf + out->msgq_off, out->msg_count);
    return NV_GSP_RPC_OK;
}

int nv_gsp_rmargs_build(uint8_t *buf, size_t buflen, uint64_t shm_dma,
                        const nv_gsp_shm_layout_t *lay)
{
    if (!buf || !lay) return NV_GSP_RPC_ERR_ARG;
    if (buflen < NV_GSP_RMARGS_SIZE) return NV_GSP_RPC_ERR_BOUNDS;
    for (unsigned i = 0; i < NV_GSP_RMARGS_SIZE; i++) buf[i] = 0;
    st64(buf + NV_RMARGS_SHARED_MEM_PHYS_OFF, shm_dma);            /* sharedMemPhysAddr */
    st32(buf + NV_RMARGS_PTE_COUNT_OFF,       lay->ptes_nr);       /* pageTableEntryCount */
    st64(buf + NV_RMARGS_CMDQ_OFF_OFF,        lay->cmdq_off);      /* cmdQueueOffset */
    st64(buf + NV_RMARGS_STATQ_OFF_OFF,       lay->msgq_off);      /* statQueueOffset */
    /* lockless-очереди / srInit / gpuInstance / profilerArgs = 0 (cold boot). */
    return NV_GSP_RPC_OK;
}

uint32_t nv_gsp_msgq_writeptr(const uint8_t *shm, const nv_gsp_shm_layout_t *lay)
{
    const uint8_t *q = shm + lay->msgq_off + NV_MSGQ_TX_WRITEPTR_OFF;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

/* ============ Пре-бутовые RPC в cmdq ============ */

static uint64_t ld64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static size_t cstrcpy(uint8_t *d, const char *s)   /* копирует строку + '\0', возвращает длину с терминатором */
{
    size_t i = 0;
    while (s[i]) { d[i] = (uint8_t)s[i]; i++; }
    d[i] = 0;
    return i + 1;
}

void nv_gsp_build_sysinfo(uint8_t *buf, uint64_t bar0, uint64_t bar1, uint64_t bar3, uint64_t bdf)
{
    for (unsigned i = 0; i < NV_GSP_SYSINFO_SIZE; i++) buf[i] = 0;
    st64(buf + NV_GSP_SYSINFO_GPUPHYS_OFF,  bar0);                 /* gpuPhysAddr   (BAR0) */
    st64(buf + NV_GSP_SYSINFO_FBPHYS_OFF,   bar1);                 /* gpuPhysFbAddr (BAR1) */
    st64(buf + NV_GSP_SYSINFO_INSTPHYS_OFF, bar3);                 /* gpuPhysInstAddr (BAR3) */
    st64(buf + NV_GSP_SYSINFO_BDF_OFF,      bdf);                  /* nvDomainBusDeviceFunc */
    st64(buf + NV_GSP_SYSINFO_MAXUSERVA_OFF, 0x00007ffffffff000ull); /* maxUserVa = TASK_SIZE (x86-64) */
    st32(buf + NV_GSP_SYSINFO_MIRRORBASE_OFF, 0x088000u);          /* pciConfigMirrorBase */
    st32(buf + NV_GSP_SYSINFO_MIRRORSIZE_OFF, 0x001000u);          /* pciConfigMirrorSize */
    /* acpiMethodData @120 (bValid=0) и остальное = 0. */
}

uint32_t nv_gsp_build_registry(uint8_t *buf, size_t buflen)
{
    static const char *names[2] = { "RMSecBusResetEnable", "RMForcePcieConfigSave" };
    const uint32_t num = 2;
    const uint32_t hdr = 8;                 /* size(u32) + numEntries(u32) */
    uint32_t str_off = hdr + num * 16u;     /* entries[num] = 8 + 32 = 40 */
    if (buflen < str_off + 64u) return 0;
    for (uint32_t i = 0; i < str_off; i++) buf[i] = 0;
    st32(buf + 0, hdr);                      /* rpc->size = sizeof(*rpc) = 8 (как nouveau) */
    st32(buf + 4, num);                      /* numEntries */
    uint32_t so = str_off;
    for (uint32_t i = 0; i < num; i++) {
        uint8_t *e = buf + hdr + i * 16u;
        st32(e + 0, so);                     /* nameOffset */
        e[4] = 1;                            /* type = 1 (DWORD) */
        st32(e + 8, 1u);                     /* data = 1 */
        st32(e + 12, 4u);                    /* length = 4 */
        so += (uint32_t)cstrcpy(buf + so, names[i]);
    }
    return so;                               /* полная длина payload */
}

uint32_t nv_gsp_cmdq_write(uint8_t *cmdq, uint32_t slot, uint32_t seq, uint32_t function,
                           const uint8_t *payload, uint32_t payload_len)
{
    uint32_t rpc_len = NV_GSP_RPC_HDR_SIZE + payload_len;              /* sizeof(rpc) + argc */
    uint32_t argc    = NV_GSP_MSG_ELEM_HDR_SIZE + ((rpc_len + 7u) & ~7u);
    uint32_t total   = (argc + (GSP_PAGE - 1u)) & ~(GSP_PAGE - 1u);    /* размер элемента (стр.) */
    uint32_t elem_count = total / GSP_PAGE;
    uint8_t *e = cmdq + NV_GSP_QUEUE_ENTRYOFF + (size_t)slot * NV_GSP_QUEUE_MSGSIZE;

    for (uint32_t i = 0; i < total; i++) e[i] = 0;
    st32(e + NV_GSP_MSG_SEQUENCE_OFF,  seq);
    st32(e + NV_GSP_MSG_ELEMCOUNT_OFF, elem_count);
    /* rpc_message_header_v03 */
    uint8_t *r = e + NV_GSP_RPC_HDR_OFF;
    st32(r + 0,  NV_GSP_RPC_HEADER_VERSION);   /* header_version = 0x03000000 */
    st32(r + 4,  NV_GSP_RPC_SIGNATURE);        /* signature = 'CPRV' */
    st32(r + NV_GSP_RPC_F_LENGTH,   rpc_len);  /* length = sizeof(rpc)+argc */
    st32(r + NV_GSP_RPC_F_FUNCTION, function);
    st32(r + 16, 0xffffffffu);                 /* rpc_result */
    st32(r + 20, 0xffffffffu);                 /* rpc_result_private */
    /* sequence@24, spare@28 = 0 */
    if (payload && payload_len)
        for (uint32_t i = 0; i < payload_len; i++) e[NV_GSP_RPC_PAYLOAD_OFF + i] = payload[i];
    /* checksum = XOR всех u64 элемента (поле checksum=0 на момент расчёта), затем верх^низ */
    uint64_t csum = 0;
    for (uint32_t o = 0; o < total; o += 8) csum ^= ld64(e + o);
    st32(e + NV_GSP_MSG_CHECKSUM_OFF, (uint32_t)(csum >> 32) ^ (uint32_t)csum);
    return elem_count;
}

void nv_gsp_cmdq_set_writeptr(uint8_t *shm, const nv_gsp_shm_layout_t *lay, uint32_t wptr)
{
    st32(shm + lay->cmdq_off + NV_MSGQ_TX_WRITEPTR_OFF, wptr);
}
