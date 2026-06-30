# Слой 2 — полный bring-up GSP-RM до `GSP_INIT_DONE` (РЕШЕНО на железе)

**Статус:** 🟢 завершён на RTX 4070 Super (AD104), Linux/VFIO, 2026-06-30.
**Доказательство:** `docs/hw-dumps/20260630-rtx4070s-gsp-INIT_DONE-OK.log`.
**Оркестратор:** `tools/gsp_boot_linux.c` (один прогон делает всё).
**Прогон без ребута:** `tools/run-gsp-boot-detached.sh` (юнит `rtx-booter`).

Этот документ — ПОЛНАЯ запись того, как поднимается GSP-RM и присылает
`GSP_INIT_DONE`. Если что-то регрессует — здесь все детали, чтобы восстановить.
Эталон везде — **nouveau `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c`** (v6.7,
прошивка 535.113.01) и nvrm-заголовки `include/nvrm/535.113.01/`.

---

## 0. Карта целиком (что за чем)

```
1. FWSEC-FRTS         → создаёт WPR2 (FRTS-регион вверху VRAM), mbox0=0
2. staging GSP-RM в sysmem:
     .fwimage (радикс3) + bootloader + signature + GspFwWprMeta + libos-args
     + очереди RPC (cmdq/msgq) + rmargs (GSP_ARGUMENTS_CACHED)
3. ПРЕ-БУТ RPC в cmdq: SET_SYSTEM_INFO + SET_REGISTRY   ← КРИТИЧНО (см. §4)
4. reset GSP-фалкона в RISC-V, GSP mbox = libos-адрес
5. SEC2 Booter (mbox = WprMeta) → грузит GSP-RM в WPR2, стартует RISC-V, mbox0=0
6. дренаж msgq (GSP→CPU), исполняя по пути:
     0x101b GSP_SEND_USER_SHARED_DATA   (игнор)
     0x1002 GSP_RUN_CPU_SEQUENCER       ← ИСПОЛНИТЬ (mid-init ресет, см. §5)
     0x100c UCODE_LIBOS_PRINT           (игнор)
     0x1001 GSP_INIT_DONE               ← ЦЕЛЬ
```

Реальный поток событий на железе (msgq.writePtr дошёл до 7):
```
msg#1 slot0  0x101b  ec=1
msg#2 slot1  0x1002  ec=2  → выполнить CPU-секвенсер (1576 команд) → GSP-RM возобновлён
msg#3 slot3  0x101b  ec=1
msg#4 slot4  0x100c  ec=1
msg#5 slot5  0x100c  ec=1
msg#6 slot6  0x1001  ec=1  ★ GSP_INIT_DONE ★
```

---

## 1. Параметры железа (RTX 4070S, AD104)

- `PMC_BOOT_0 = 0x194000A1` (Ada AD104, chipset 0x194, rev A1).
- VRAM = 12282 МиБ (`0x2ffa00000`).
- FRTS-регион (FWSEC): `frts_addr=0x2ff800000`, запросили `0x100000`, фактический
  WPR2 после FWSEC: `[0x2ff800000..0x2ff8e0000]` → реальный `frtsSize=0xe0000`
  (FWSEC клампит под VGA-workspace на ~128К).
- WPR2-раскладка (computed): `start=0x2f5400000 end=0x2ff900000 heap=0x2f5500000
  elf=0x2fd3b0000 boot=0x2ff7f8000` (heap ≈ 126 МиБ).
- **PCI BAR-физадреса** (из `/sys/bus/pci/devices/0000:01:00.0/resource`,
  нужны для `GspSystemInfo`): BAR0=`0x52000000`, BAR1=`0x40000000`,
  BAR3=`0x50000000`. PCI BDF id = `(bus<<8)|(dev<<3)|func` = `0x100`.
- Falcon-базы: GSP `0x110000` (PFALCON2 `0x111000`), SEC2 `0x840000` (`0x841000`).

---

## 2. WPR2-раскладка и GspFwWprMeta (тонкости, на которых легко споткнуться)

Раскладка считается формулой nouveau `r535_gsp_oneinit` (ИДЕНТИЧНА нашей
`nv_gsp_fb_layout`, файл `driver/gsp/gsp_fw.c`):
```
frts.size = 0x100000                                    ← НОМИНАЛЬНЫЙ, не фактический!
frts.addr = ALIGN_DOWN(bios.addr,0x20000) - frts.size
boot.addr = ALIGN_DOWN(frts.addr - boot.size, 0x1000)
elf.addr  = ALIGN_DOWN(boot.addr - elf.size, 0x10000)
heap.size = os_carveout(20M)+base(8M)+ALIGN(96K*fb_gb,1M)+ALIGN(client_alloc(96M),1M), min 84M
heap.addr = ALIGN_DOWN(elf.addr - heap.size, 0x100000)
wpr2.addr = ALIGN_DOWN(heap.addr - sizeof(GspFwWprMeta=256), 0x100000)
nonwpr_heap = 0x100000, сразу под wpr2.addr
gspFwWprEnd = frts.addr + frts.size  (= 0x2ff900000)
```

⚠️ **ГРАБЛИ (проверено железом):** `gspFwWprEnd` в WprMeta должен быть
`frts_addr + НОМИНАЛЬНЫЕ 0x100000` = `0x2ff900000`, а НЕ фактический верх
HW-WPR2 `0x2ff8e0000`. Любое другое значение → Booter ОТВЕРГАЕТ meta
(`mbox0=0x8d`, RISC-V не стартует). Рассинхрон `meta(end=0x2ff900000) ↔
HW-WPR2(end=0x2ff8e0000)` на `0x20000` — ШТАТНЫЙ, его создаёт сам Booter,
GSP-RM к нему устойчив. Не «чинить»!

`GspFwWprMeta` (256 байт, magic `0xdc3aae21371a60b3`) — поля по смещениям, см.
`nv_gsp_wpr_meta_build` в `gsp_fw.c`. radix3 — 3 уровня 4К, u64-записи.

---

## 3. Очереди RPC + rmargs (sysmem)

Общий регион (IOVA в DMA-арене): `[PTE-таблица 0x1000][cmdq 0x40000][msgq 0x40000]`.
- `ptes_nr = 129` (1 стр. PTE + 64 cmdq + 64 msgq), PTE[i] = shm_dma + i*0x1000.
- `cmdq_off = 0x1000`, `msgq_off = 0x41000`. Записи с `entryOff=0x1000`, слот=0x1000.
- `msgCount = (0x40000 - 0x1000)/0x1000 = 63`.

Заголовок очереди `msgqTxHeader` (32б, все u32): version@0, size@4=0x40000,
msgSize@8=0x1000, msgCount@12=63, writePtr@16, flags@20=1, rxHdrOff@24=32,
entryOff@28=0x1000. Затем `msgqRxHeader.readPtr` @ rxHdrOff(32). См.
`nv_gsp_shm_init` в `driver/gsp/gsp_rpc.c`.

`rmargs` = `GSP_ARGUMENTS_CACHED` (передаётся как libos-регион "RMARGS"):
`messageQueueInitArguments` @0: sharedMemPhysAddr@0 (u64), pageTableEntryCount@8
(u32), cmdQueueOffset@16 (u64), statQueueOffset@24 (u64). Остальное (srInit,
gpuInstance, profiler) = 0 при холодном старте. `NvLength=8` (u64). См.
`nv_gsp_rmargs_build`.

libos init-args — РОВНО 4 региона (порядок важен): `LOGINIT`, `LOGINTR`,
`LOGRM` (по 0x10000), `RMARGS` (0x1000). Каждый: id8(имя)+pa+size+kind(CONTIGUOUS)
+loc(SYSMEM). Лог-буферы: put-указатель @0 (u64), затем PTE-массив @8.

---

## 4. ⭐ КЛЮЧ №1: пре-бутовые RPC в cmdq (SET_SYSTEM_INFO + SET_REGISTRY)

**Без этого GSP-RM грузится, делает раннюю RM-init и НАМЕРТВО виснет** (логи
`Δ=0`, RISC-V active, msgq пуст). Причина: GSP-RM в ранней инициализации ЧИТАЕТ
из cmdq команды `SET_SYSTEM_INFO` и `SET_REGISTRY` и блокируется, пока их там нет.
nouveau кладёт их в cmdq в `r535_gsp_oneinit` ДО ресета/бута GSP (строки ~2262–2266).

Делаем то же ДО шага 4 (reset GSP). См. `gsp_boot_linux.c` блок «2.5».

### Формат элемента очереди (GSP_MSG_QUEUE_ELEMENT, 48б) + RPC-заголовок (32б)
Элемент пишется в `cmdq + entryOff(0x1000) + slot*0x1000`:
```
@0   auth_tag[16] = 0          (не-CC режим)
@16  aad[16]      = 0
@32  checksum (u32)            ← XOR всех u64 элемента, потом верх^низ (поле=0 при расчёте)
@36  sequence (u32)
@40  elem_count (u32)          = размер_элемента / 0x1000
@44  pad (u32) = 0
@48  rpc header (nvfw_gsp_rpc, 32б):
       @48 header_version = 0x03000000
       @52 signature      = 0x43505256  ('C'<<24|'P'<<16|'R'<<8|'V')
       @56 length         = 32 + payload_len   (НЕ паддингованный)
       @60 function       = номер RPC
       @64 rpc_result          = 0xffffffff
       @68 rpc_result_private   = 0xffffffff
       @72 sequence = 0, @76 spare = 0
@80  payload
```
Размер элемента = `ALIGN(48 + ALIGN(32+payload_len, 8), 0x1000)` (для sysinfo/registry
= 1 страница, elem_count=1). **checksum**: занулить поле, XOR всех u64 по всему
элементу (размер кратен 8), `checksum = (csum>>32) ^ (csum & 0xffffffff)`. После
записи всех элементов — выставить `cmdq.tx.writePtr` = число занятых слотов.
См. `nv_gsp_cmdq_write` / `nv_gsp_cmdq_set_writeptr`.

### Номера функций (rpc_global_enums.h 535.113.01)
- `GSP_SET_SYSTEM_INFO = 72` · `SET_REGISTRY = 73` · `CONTINUATION_RECORD = 71`
- События (msgq): `FIRST_EVENT=0x1000`, **`GSP_INIT_DONE=0x1001`**,
  `RUN_CPU_SEQUENCER=0x1002`, `UCODE_LIBOS_PRINT=0x100c`, `GSP_SEND_USER_SHARED_DATA=0x101b`.

### GspSystemInfo (gsp_static_config.h) — sizeof = **664** байта
Зануляем целиком, заполняем (остальное 0, ACPI не нужен для бута):
```
@0   gpuPhysAddr          = BAR0  (0x52000000)
@8   gpuPhysFbAddr        = BAR1  (0x40000000)
@16  gpuPhysInstAddr      = BAR3  (0x50000000)
@24  nvDomainBusDeviceFunc= BDF   (0x100)
@56  maxUserVa            = 0x00007ffffffff000   (TASK_SIZE x86-64)
@64  pciConfigMirrorBase  = 0x088000
@68  pciConfigMirrorSize  = 0x001000
@120 acpiMethodData       = 0     (bValid=0)
```
(Структура: u64×8, затем u32 mirrorBase/Size, u8 oorArch, u64 clPdbProperties@80,
u32 Chipset@88, NvBool×6, BUSINFO×2, ACPI_METHOD_DATA@120, GSP_VF_INFO в конце.)
См. `nv_gsp_build_sysinfo`.

### SET_REGISTRY (PACKED_REGISTRY_TABLE, g_os_nvoc.h) — 2 ключа, payload 82 байта
```
@0  size      = 8        (= sizeof заголовка, как nouveau)
@4  numEntries= 2
@8  entry[0] (16б): nameOffset=40, type=1, data=1, length=4   ← "RMSecBusResetEnable"
@24 entry[1] (16б): nameOffset=60, type=1, data=1, length=4   ← "RMForcePcieConfigSave"
@40 строки: "RMSecBusResetEnable\0" (20) "RMForcePcieConfigSave\0" (22)
```
PACKED_REGISTRY_ENTRY (16б, естественное выравнивание): nameOffset@0, type@4(u8),
data@8(u32), length@12. См. `nv_gsp_build_registry`.

---

## 5. ⭐ КЛЮЧ №2: GSP_RUN_CPU_SEQUENCER (0x1002) — «mid-init ресет GSP-ядра»

После пре-бутовых RPC GSP-RM оживает и шлёт поток событий. Среди них —
`GSP_RUN_CPU_SEQUENCER`: GSP просит CPU исполнить список I/O-команд (специальный
ресет GSP-ядра в середине init). **Если не исполнить — GSP снова виснет.**
Эталон: nouveau `r535_gsp_msg_run_cpu_sequencer`. Опкоды: `rmgspseq.h`.
Структура `rpc_run_cpu_sequencer_v17_00` (payload @ element+80):
```
@0  bufferSizeDWord (u32)
@4  cmdIndex (u32)         ← число DWORD в commandBuffer для обработки
@8  regSaveArea[8] (u32×8)
@40 commandBuffer[] (u32)  ← поток [opCode][payload...]
```
Парсинг: `op = cb[ptr++]; ptr += PAYLOAD_DWORDS(op)`. Размеры payload (DWORD):
REG_WRITE=2, REG_MODIFY=3, REG_POLL=5, DELAY_US=1, REG_STORE=2, CORE_*=0.

### Опкоды (GSP_SEQ_BUF_OPCODE) и что делать (все reg-адреса АБСОЛЮТНЫЕ)
| код | имя | действие |
|---|---|---|
| 0 | REG_WRITE  | `wr32(addr,val)` |
| 1 | REG_MODIFY | `wr32(addr,(rd32(addr)&~mask)\|val)` |
| 2 | REG_POLL   | ждать `(rd32(addr)&mask)==val`, timeout (0→4с); +error (игнор) |
| 3 | DELAY_US   | `udelay(val)` |
| 4 | REG_STORE  | `regSave[index]=rd32(addr)` |
| 5 | CORE_RESET | `nvkm_falcon_reset(gsp)` + `mask(GSP+0x624,0x80,0x80)` + `wr(GSP+0x10c,0)` |
| 6 | CORE_START | если `rd(GSP+0x100)&0x40`: `wr(GSP+0x130,2)` иначе `wr(GSP+0x100,2)` |
| 7 | CORE_WAIT_FOR_HALT | ждать `rd(GSP+0x100)&0x10` |
| 8 | CORE_RESUME | см. ниже |

Реальный буфер (1576 DWORD, 425 команд): REG_POLL(ждать GSP mbox bit31)→
REG_WRITE(сброс mbox)→CORE_RESET→длинный DMA-loop (рег `0x110114/11c/118` =
falcon DMATRF, догрузка HS-ucode в GSP-фалкон чанками)→финальные REG_WRITE→
CORE_START→CORE_WAIT_HALT→CORE_RESUME.

`nv_falcon_start`/`nv_falcon_wait_halted` ТОЧНО соответствуют CORE_START/WAIT.
`nvkm_falcon_reset = disable()+enable()` для GSP-фалкона (gm200_flcn_disable/enable):
select(ga102) + IRQ-clear(0x048,0x014) + reset_eng(mask 0x3c0 bit0, дважды) +
wait_mem_scrubbing + `wr(0x084,PMC_BOOT_0)`. Мы переиспользуем рабочий
`nv_falcon_reset_ga102` (он же грузит FWSEC на GSP-фалконе).

### ⭐ CORE_RESUME (на нём был провал — критично сделать ТОЧНО по nouveau)
```
1. nv_falcon_gsp_reset_riscv(GSP)         ← gsp->func->reset (reset_eng + BCR 0x111668|=0x111)
2. GSP mbox0/1 = libos-адрес
3. nv_falcon_start(SEC2)                   ← РЕСТАРТ уже загруженного Booter,
                                              БЕЗ reset/reload SEC2!
4. ждать rd32(0x1180f8) & 0x04000000       (2с) — SEC2 Booter завершён
5. проверить SEC2 mbox0 (0x840040) == 0
6. wr(GSP+0x080) = app_version (FALCON_OS)
7. проверить GSP RISC-V active
```
⚠️ **ГРАБЛИ:** НЕ делать reset+dma_load+boot SEC2 заново на CORE_RESUME!
Свежий `booter_load` при УЖЕ расширенном WPR2 (от первого бута) возвращает
`mbox0=0x29`. nouveau просто рестартует загруженный Booter (`nvkm_falcon_start`).
См. `seq_core_resume` в `gsp_boot_linux.c`.

После исполнения секвенсера ОТВЕТ GSP НЕ шлём (CORE_RESUME сам возобновляет
GSP-RM). Сообщение-секвенсер consume-им (двигаем msgq.readPtr на elem_count),
продолжаем дренаж — приходит `GSP_INIT_DONE`.

---

## 6. Дренаж msgq до INIT_DONE

Читаем `msgq.tx.writePtr` (GSP→CPU). Пока `readPtr != writePtr`:
читаем элемент в слоте `readPtr`, берём `elem_count` (@element+40), `function`
(@rpc+12), `signature` (@rpc+4). Если `0x1002` — исполняем секвенсер. Двигаем
`readPtr += elem_count` (с wrap по msg_count=63), пишем в `msgq.rxHdr.readPtr`
(consume). Если `function == 0x1001` (GSP_INIT_DONE) — **УСПЕХ**. Между опросами
звоним в дверной звонок `wr(GSP+0xc00, 0)` (как `nvkm_falcon_wr32(&gsp->falcon,
0xc00,0)`). См. цикл в `gsp_boot_linux.c`.

---

## 7. Среда исполнения (VFIO/IOMMU) — важные нюансы

- Прогон headless через **VFIO** (`vfio-pci`), GPU одна → гасим gdm/fbcon,
  выгружаем nvidia, после прогона возвращаем. `tools/run-gsp-boot-detached.sh`.
- VFIO делает **FLR** при захвате (dmesg `resetting`/`reset done`) → GPU чист,
  GFW re-boot отрабатывает (`nv_wait_gfw_boot_completed`, SCRATCH GFW_BOOT==0xFF).
- **Заглушка IOVA[0..16M]**: под нативным nouveau стоит `iommu=pt` (passthrough,
  GPU DMA любой физадрес); под VFIO — строгий IOMMU. GSP-RM иногда читает IOVA 0 →
  DMAR-фолт. Маппим зануленный регион на IOVA 0, чтобы не падало. После этого
  DMAR-фолтов 0 (`dmesg | grep DMAR`).
- DMA-арена: 64 МиБ на IOVA `0x10000000`, бамп-аллокатор (всё staging внутри).

---

## 8. Тупики, которые НЕ надо повторять (потрачено много времени)

- **Декод libos-логов статически НЕВОЗМОЖЕН** из linux-firmware-блоба: строки
  формата есть в `.fwimage`, но индекс метаданных (runtime-VA `0x20015xxx`/
  `0x20a05xxx` → строка) — это LOOS-сегмент логирования RM-таска, СТРИПнутый
  (лежит за концом `.fwimage`). Нужен проприетарный decode-ELF NVIDIA. Не копать.
- **Не «чинить» рассинхрон gspFwWprEnd** (см. §2) — Booter его требует.
- **Не ресетить/перезагружать SEC2 на CORE_RESUME** (см. §5) → `mbox0=0x29`.
- Замирание GSP-RM было НЕ из-за: наших структур (все верны), DMA-фолтов
  (заглушка убрала), грязного GPU (FLR чистит), дверного звонка (INIT_DONE шлётся
  сам). Причина — отсутствие SET_SYSTEM_INFO/REGISTRY + неисполнение секвенсера.

---

## 9. Источники (точные)

- nouveau `r535.c` (v6.7): `r535_gsp_oneinit` (порядок: libos_init → wpr_meta_init →
  **rpc_set_system_info → rpc_set_registry** → reset → booter), `r535_gsp_init`
  (booter_load → rpc_poll INIT_DONE), `r535_gsp_msg_run_cpu_sequencer`,
  `r535_gsp_cmdq_push` (формат элемента + checksum + звонок 0xc00).
- nvrm 535.113.01: `gsp_static_config.h` (GspSystemInfo), `g_os_nvoc.h`
  (PACKED_REGISTRY_TABLE), `rmgspseq.h` (опкоды секвенсера),
  `g_rpc-structures.h` (rpc_run_cpu_sequencer_v17_00), `rpc_global_enums.h`
  (номера функций/событий), `gpu_acpi_data.h` (ACPI_METHOD_DATA).
- Falcon HAL nouveau: `falcon/base.c` (nvkm_falcon_reset/start),
  `falcon/gm200.c` (disable/enable), `falcon/gp102.c` (reset_eng),
  `falcon/ga102.c` (select/scrub), `subdev/gsp/ga102.c` (gsp reset).

Реализация наша: `driver/gsp/gsp_rpc.{c,h}`, `driver/gsp/gsp_fw.{c,h}`,
`driver/gsp/falcon.{c,h}`, `driver/gsp/booter.{c,h}`, оркестратор
`tools/gsp_boot_linux.c`.
