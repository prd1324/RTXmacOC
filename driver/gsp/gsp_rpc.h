/*
 * gsp_rpc.h — очереди сообщений GSP-RM в общей памяти (задача 7).
 *
 * GSP-RM общается с драйвером через две кольцевые очереди в sysmem:
 *   cmdq (CPU→GSP, команды/RPC) и msgq/statq (GSP→CPU, ответы/события).
 * Регион: [PTE-таблица][cmdq 0x40000][msgq 0x40000]; PTE маппят весь регион.
 * Заголовок каждой очереди — msgqTxHeader(32б)+msgqRxHeader(4б); записи с entryOff=4K,
 * размер записи 4K. Элемент = GSP_MSG_QUEUE_ELEMENT (48б шапка) + rpc.
 *
 * Источники: OGK msgq_priv.h (msgqTxHeader/RxHeader), message_queue_priv.h
 * (GSP_MSG_QUEUE_ELEMENT), nouveau r535_gsp_shared_init. Пересказано для лицензии.
 */
#ifndef RTXMACOC_GSP_RPC_H
#define RTXMACOC_GSP_RPC_H

#include <stdint.h>
#include <stddef.h>

#define NV_GSP_RPC_OK           0
#define NV_GSP_RPC_ERR_ARG    (-1)
#define NV_GSP_RPC_ERR_BOUNDS (-2)

#define NV_GSP_QUEUE_SIZE     0x40000u            /* размер cmdq и msgq (по 256 КиБ) */
#define NV_GSP_QUEUE_ENTRYOFF 0x1000u             /* записи с 1-й страницы */
#define NV_GSP_QUEUE_MSGSIZE  0x1000u             /* размер записи = 4K */
#define NV_GSP_MSGQ_RXHDROFF  32u                 /* offsetof(rx.readPtr) = sizeof(msgqTxHeader) */

/* GSP_MSG_QUEUE_ELEMENT: authTag[16]+aad[16]+checkSum+seqNum+elemCount, затем rpc@48. */
#define NV_GSP_MSG_ELEM_HDR_SIZE 48u
/* rpc_message_header_v03: header_version@0, signature@4, ... */
#define NV_GSP_RPC_HEADER_VERSION 0x03000000u
#define NV_GSP_RPC_SIGNATURE      0x43505256u     /* 'VRPC' */

/* msgqTxHeader (OGK msgq_priv.h), все u32: */
#define NV_MSGQ_TX_VERSION_OFF   0u
#define NV_MSGQ_TX_SIZE_OFF      4u
#define NV_MSGQ_TX_MSGSIZE_OFF   8u
#define NV_MSGQ_TX_MSGCOUNT_OFF  12u
#define NV_MSGQ_TX_WRITEPTR_OFF  16u
#define NV_MSGQ_TX_FLAGS_OFF     20u
#define NV_MSGQ_TX_RXHDROFF_OFF  24u
#define NV_MSGQ_TX_ENTRYOFF_OFF  28u
/* msgqRxHeader: readPtr@0 (по rxHdrOff от начала очереди). */

/* Раскладка общего региона очередей. */
typedef struct {
    uint32_t ptes_nr;        /* число PTE (страниц во всём регионе) */
    uint64_t ptes_size;      /* размер PTE-таблицы (= смещение cmdq) */
    uint64_t cmdq_off;       /* смещение cmdq от начала региона */
    uint64_t msgq_off;       /* смещение msgq */
    uint64_t total_size;     /* ptes_size + 2*QUEUE_SIZE */
    uint32_t msg_count;      /* записей в очереди = (size-entryOff)/msgSize */
} nv_gsp_shm_layout_t;

/* Рассчитать раскладку (без буфера). */
int nv_gsp_shm_compute(nv_gsp_shm_layout_t *out);

/*
 * Инициализировать общий регион очередей в buf (>= total_size, обнуляется): PTE-таблица
 * (ptes[i]=dma_base+i*4K) + заголовки cmdq/msgq (msgqTxHeader/RxHeader). dma_base — IOVA
 * региона. *out — раскладка. Порт nouveau r535_gsp_shared_init.
 */
int nv_gsp_shm_init(uint8_t *buf, size_t buflen, uint64_t dma_base, nv_gsp_shm_layout_t *out);

/* --- rmargs (GSP_ARGUMENTS_CACHED, 80б) --- */
/* OGK gsp_init_args.h: MESSAGE_QUEUE_INIT_ARGUMENTS(@0)+GSP_SR_INIT_ARGUMENTS(@48)+
   gpuInstance(@60)+profilerArgs(@64). Поля u64 (RmPhysAddr/NvLength), NvBool=u8. */
#define NV_GSP_RMARGS_SIZE                 80u
#define NV_RMARGS_SHARED_MEM_PHYS_OFF      0u    /* sharedMemPhysAddr (u64) */
#define NV_RMARGS_PTE_COUNT_OFF            8u    /* pageTableEntryCount (u32) */
#define NV_RMARGS_CMDQ_OFF_OFF             16u   /* cmdQueueOffset (u64) */
#define NV_RMARGS_STATQ_OFF_OFF            24u   /* statQueueOffset (u64) */

/*
 * Заполнить GSP_ARGUMENTS_CACHED (rmargs) в buf (>=80, обнуляется): messageQueueInitArguments
 * из раскладки очередей (shm_dma — IOVA shared-региона). Прочее (srInit/gpuInstance/profiler)
 * = 0 (cold boot). Порт nouveau r535_gsp_rmargs_init.
 */
int nv_gsp_rmargs_build(uint8_t *buf, size_t buflen, uint64_t shm_dma,
                        const nv_gsp_shm_layout_t *lay);

/* Текущий write-указатель msgq (GSP→CPU) = msgqTxHeader.writePtr статус-очереди.
   shm — указатель на начало shared-региона; lay — его раскладка. */
uint32_t nv_gsp_msgq_writeptr(const uint8_t *shm, const nv_gsp_shm_layout_t *lay);

/* ============ Пре-бутовые RPC в cmdq (как r535_gsp_oneinit) ============
 * GSP-RM при ранней инициализации ЧИТАЕТ из cmdq команды SET_SYSTEM_INFO и
 * SET_REGISTRY и блокируется, пока их там нет. Кладём их ДО бута GSP.
 * Источник: nouveau r535_gsp_rpc_set_system_info / set_registry,
 * rpc_global_enums.h (535.113.01). */
#define NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO  72u
#define NV_VGPU_MSG_FUNCTION_SET_REGISTRY         73u
#define NV_VGPU_MSG_EVENT_GSP_INIT_DONE           0x1001u

/* GSP_MSG_QUEUE_ELEMENT (48б): authTag[16]+aad[16]+checksum@32+sequence@36+
   elemCount@40+pad@44, далее rpc_message_header_v03 @48 (32б), payload @80. */
#define NV_GSP_MSG_CHECKSUM_OFF   32u
#define NV_GSP_MSG_SEQUENCE_OFF   36u
#define NV_GSP_MSG_ELEMCOUNT_OFF  40u
#define NV_GSP_RPC_HDR_OFF        48u
#define NV_GSP_RPC_HDR_SIZE       32u
#define NV_GSP_RPC_PAYLOAD_OFF    80u   /* 48 + 32 */
#define NV_GSP_RPC_F_LENGTH        8u   /* от NV_GSP_RPC_HDR_OFF */
#define NV_GSP_RPC_F_FUNCTION     12u

/* GspSystemInfo — 664б (gsp_static_config.h 535.113.01, sizeof проверен). */
#define NV_GSP_SYSINFO_SIZE          664u
#define NV_GSP_SYSINFO_GPUPHYS_OFF     0u
#define NV_GSP_SYSINFO_FBPHYS_OFF      8u
#define NV_GSP_SYSINFO_INSTPHYS_OFF   16u
#define NV_GSP_SYSINFO_BDF_OFF        24u
#define NV_GSP_SYSINFO_MAXUSERVA_OFF  56u
#define NV_GSP_SYSINFO_MIRRORBASE_OFF 64u
#define NV_GSP_SYSINFO_MIRRORSIZE_OFF 68u

/* Заполнить GspSystemInfo (>=664б, обнуляется): BAR-физадреса GPU + PCI BDF.
   ACPI/прочее = 0 (для базового бута достаточно). */
void nv_gsp_build_sysinfo(uint8_t *buf, uint64_t bar0, uint64_t bar1, uint64_t bar3, uint64_t bdf);

/* Заполнить PACKED_REGISTRY_TABLE (2 ключа: RMSecBusResetEnable=1,
   RMForcePcieConfigSave=1). Возвращает длину payload (байт). */
uint32_t nv_gsp_build_registry(uint8_t *buf, size_t buflen);

/* Записать RPC-команду в cmdq: элемент в слот `slot` (страница 0x1000 от entryOff),
   с контрольной суммой (XOR u64 ^ верх/низ). cmdq — указатель на начало cmdq-региона
   (shm + cmdq_off). Возвращает число занятых слотов (elem_count). */
uint32_t nv_gsp_cmdq_write(uint8_t *cmdq, uint32_t slot, uint32_t seq, uint32_t function,
                           const uint8_t *payload, uint32_t payload_len);

/* Установить cmdq.tx.writePtr (число готовых к чтению слотов). */
void nv_gsp_cmdq_set_writeptr(uint8_t *shm, const nv_gsp_shm_layout_t *lay, uint32_t wptr);

#endif /* RTXMACOC_GSP_RPC_H */
