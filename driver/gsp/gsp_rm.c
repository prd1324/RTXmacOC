/*
 * gsp_rm.c — реализация двустороннего RPC к GSP-RM + базовых RM-объектов (см. gsp_rm.h).
 * Порт nouveau r535.c. Платформенная привязка (MMIO/задержка) — через колбэки канала.
 *
 * PROBE: смещения GspStaticConfigInfo (NV_GSP_SCI_*) получены компиляцией
 * структуры из gsp_static_config.h (535.113.01) против SDK-заголовков с offsetof:
 *   sizeof=2168, fbRegionInfoParams=840, bar1PdeBase=2088, bar2PdeBase=2096,
 *   hInternalClient=2152, hInternalDevice=2156, hInternalSubdevice=2160.
 * На железе подкрепляются самопроверкой FB-карты (см. nv_gsp_get_static_info).
 */
#include "gsp_rm.h"

#define GSP_PAGE 0x1000u

static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t ld64(const uint8_t *p)
{ uint64_t v=0; for (int i=0;i<8;i++) v |= (uint64_t)p[i]<<(8*i); return v; }
static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* Прочитать msgqRxHeader.readPtr нашего msgq (мы — потребитель статус-очереди). */
static void msgq_set_readptr(nv_gsp_rpc_chan *ch, uint32_t rptr)
{
    volatile uint8_t *q = ch->shm + ch->lay.msgq_off + NV_GSP_MSGQ_RXHDROFF;
    st32((uint8_t *)q, rptr);
}

int nv_gsp_rpc_call(nv_gsp_rpc_chan *ch, uint32_t fn,
                    const uint8_t *req, uint32_t req_len,
                    uint8_t *reply_out, uint32_t reply_cap, uint32_t *reply_len,
                    uint32_t *rpc_result, uint32_t timeout_us)
{
    if (!ch || !ch->shm) return NV_GSP_RM_ERR_ARG;
    if (!timeout_us) timeout_us = 4000000u;

    /* --- send: построить элемент в текущем слоте cmdq, продвинуть writePtr, позвонить --- */
    uint8_t *cmdq = ch->shm + ch->lay.cmdq_off;
    uint32_t ec = nv_gsp_cmdq_write(cmdq, ch->cmdq_wptr, ch->seq, fn, req, req_len);
    ch->cmdq_wptr += ec;
    while (ch->cmdq_wptr >= ch->lay.msg_count) ch->cmdq_wptr -= ch->lay.msg_count;
    nv_gsp_cmdq_set_writeptr(ch->shm, &ch->lay, ch->cmdq_wptr);
    ch->seq++;
    if (ch->ring) ch->ring(ch->io_ctx);

    /* --- recv: опрос msgq до ответа той же функции (async-события по пути — пропуск) --- */
    const uint32_t step = 1000u;           /* шаг опроса, мкс */
    uint32_t waited = 0;
    for (;;) {
        uint32_t wptr = nv_gsp_msgq_writeptr(ch->shm, &ch->lay);
        while (ch->msgq_rptr != wptr) {
            const uint8_t *e = ch->shm + ch->lay.msgq_off + NV_GSP_QUEUE_ENTRYOFF
                             + (size_t)ch->msgq_rptr * NV_GSP_QUEUE_MSGSIZE;
            uint32_t mec = ld32(e + NV_GSP_MSG_ELEMCOUNT_OFF);
            if (!mec || mec > ch->lay.msg_count) mec = 1;
            const uint8_t *r = e + NV_GSP_RPC_HDR_OFF;
            uint32_t mfn  = ld32(r + NV_GSP_RPC_F_FUNCTION);
            uint32_t mlen = ld32(r + NV_GSP_RPC_F_LENGTH);
            uint32_t mres = ld32(r + 16);   /* rpc_result @ rpc-заголовок+16 */

            if (mfn == fn) {
                uint32_t plen = (mlen >= NV_GSP_RPC_HDR_SIZE) ? mlen - NV_GSP_RPC_HDR_SIZE : 0u;
                /* Защита от заворачивания элемента за конец кольца (в этом проходе
                   ответы ≤ 1 страницы и rptr мал → не срабатывает). */
                if (ch->msgq_rptr + mec > ch->lay.msg_count) return NV_GSP_RM_ERR_BOUNDS;
                if (reply_out && reply_cap) {
                    uint32_t n = plen < reply_cap ? plen : reply_cap;
                    const uint8_t *p = e + NV_GSP_RPC_PAYLOAD_OFF;
                    for (uint32_t i = 0; i < n; i++) reply_out[i] = p[i];
                }
                if (reply_len)  *reply_len  = plen;
                if (rpc_result) *rpc_result = mres;
                ch->msgq_rptr += mec;
                while (ch->msgq_rptr >= ch->lay.msg_count) ch->msgq_rptr -= ch->lay.msg_count;
                msgq_set_readptr(ch, ch->msgq_rptr);
                return NV_GSP_RM_OK;
            }

            /* Не наш ответ — async-событие GSP. Пропускаем (consume), продолжаем. */
            ch->msgq_rptr += mec;
            while (ch->msgq_rptr >= ch->lay.msg_count) ch->msgq_rptr -= ch->lay.msg_count;
            msgq_set_readptr(ch, ch->msgq_rptr);
        }
        if (waited >= timeout_us) return NV_GSP_RM_ERR_TIMEOUT;
        if (ch->udelay) ch->udelay(ch->io_ctx, step);
        waited += step;
        if ((waited % 200000u) == 0u && ch->ring) ch->ring(ch->io_ctx); /* периодич. звонок */
    }
}

int nv_gsp_get_static_info(nv_gsp_rpc_chan *ch, nv_gsp_static_info *out)
{
    if (!ch || !out) return NV_GSP_RM_ERR_ARG;

    static uint8_t req[NV_GSP_SCI_SIZE];
    static uint8_t rep[NV_GSP_SCI_SIZE];
    for (unsigned i = 0; i < NV_GSP_SCI_SIZE; i++) req[i] = 0;

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO,
                             req, NV_GSP_SCI_SIZE, rep, NV_GSP_SCI_SIZE,
                             &rlen, &rres, 4000000u);
    if (rc != NV_GSP_RM_OK) return rc;
    if (rres != 0) return NV_GSP_RM_ERR_BOUNDS;          /* GSP вернул ошибку RPC */
    if (rlen < NV_GSP_SCI_SIZE) return NV_GSP_RM_ERR_BOUNDS;

    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    const uint8_t *fb = rep + NV_GSP_SCI_FBREGION_OFF;
    uint32_t n = ld32(fb + NV_GSP_FBREGION_NUM_OFF);
    if (n == 0 || n > NV_GSP_FBREGION_MAX) return NV_GSP_RM_ERR_BOUNDS;  /* самопроверка */
    out->num_regions = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rg = fb + NV_GSP_FBREGION_ARR_OFF + (size_t)i * NV_GSP_FBREGION_STRIDE;
        out->regions[i].base       = ld64(rg + 0);
        out->regions[i].limit      = ld64(rg + 8);
        out->regions[i].reserved   = ld64(rg + 16);
        out->regions[i].performance= ld32(rg + 24);
        out->regions[i].compressed = rg[28];
        out->regions[i].iso        = rg[29];
        out->regions[i].prot       = rg[30];
    }
    out->bar1_pde     = ld64(rep + NV_GSP_SCI_BAR1PDE_OFF);
    out->bar2_pde     = ld64(rep + NV_GSP_SCI_BAR2PDE_OFF);
    out->h_client     = ld32(rep + NV_GSP_SCI_HINTCLIENT_OFF);
    out->h_device     = ld32(rep + NV_GSP_SCI_HINTDEVICE_OFF);
    out->h_subdevice  = ld32(rep + NV_GSP_SCI_HINTSUBDEV_OFF);
    return NV_GSP_RM_OK;
}

int nv_gsp_rm_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hParent,
                    uint32_t hObject, uint32_t hClass,
                    const uint8_t *params, uint32_t params_len, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;

    /* req = rpc_gsp_rm_alloc_v03_00 (шапка 32б) + params. */
    static uint8_t req[NV_RM_ALLOC_HDR_SIZE + 256u];
    static uint8_t rep[NV_RM_ALLOC_HDR_SIZE + 256u];
    if (params_len > 256u) return NV_GSP_RM_ERR_BOUNDS;
    uint32_t req_len = NV_RM_ALLOC_HDR_SIZE + params_len;
    for (uint32_t i = 0; i < req_len; i++) req[i] = 0;
    st32(req + NV_RM_ALLOC_HCLIENT_OFF,   hClient);
    st32(req + NV_RM_ALLOC_HPARENT_OFF,   hParent);
    st32(req + NV_RM_ALLOC_HOBJECT_OFF,   hObject);
    st32(req + NV_RM_ALLOC_HCLASS_OFF,    hClass);
    st32(req + NV_RM_ALLOC_STATUS_OFF,    0u);
    st32(req + NV_RM_ALLOC_PARAMSIZE_OFF, params_len);
    st32(req + NV_RM_ALLOC_FLAGS_OFF,     0u);
    if (params && params_len)
        for (uint32_t i = 0; i < params_len; i++) req[NV_RM_ALLOC_HDR_SIZE + i] = params[i];

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC,
                             req, req_len, rep, req_len, &rlen, &rres, 4000000u);
    if (rc != NV_GSP_RM_OK) return rc;
    if (rres != 0) return NV_GSP_RM_ERR_BOUNDS;          /* транспорт RPC дал ошибку */
    if (rlen < NV_RM_ALLOC_HDR_SIZE) return NV_GSP_RM_ERR_BOUNDS;
    if (status) *status = ld32(rep + NV_RM_ALLOC_STATUS_OFF);  /* NV_OK==0 */
    return NV_GSP_RM_OK;
}

int nv_gsp_rm_client_ctor(nv_gsp_rpc_chan *ch, uint32_t *out_client, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_CLIENT_HANDLE | 0u;
    /* NV0000_ALLOC_PARAMETERS (108б): hClient@0, processID@4=~0, processName[100]@8. */
    uint8_t p[108];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + 0, h);
    st32(p + 4, 0xffffffffu);                            /* processID = ~0 */
    int rc = nv_gsp_rm_alloc(ch, h, h, h, NV01_ROOT, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_client) *out_client = h;
    return rc;
}

int nv_gsp_rm_device_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient,
                          uint32_t *out_device, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_DEVICE_HANDLE;
    /* NV0080_ALLOC_PARAMETERS (56б): deviceId@0, hClientShare@4, …, vaSpaceSize@24… */
    uint8_t p[56];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + 4, hClient);                                /* hClientShare = client */
    int rc = nv_gsp_rm_alloc(ch, hClient, hClient, h, NV01_DEVICE_0, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_device) *out_device = h;
    return rc;
}

int nv_gsp_rm_subdevice_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t *out_subdev, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_SUBDEV_HANDLE;
    /* NV2080_ALLOC_PARAMETERS (4б): subDeviceId@0 = 0. */
    uint8_t p[4] = {0,0,0,0};
    int rc = nv_gsp_rm_alloc(ch, hClient, hDevice, h, NV20_SUBDEVICE_0, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_subdev) *out_subdev = h;
    return rc;
}
