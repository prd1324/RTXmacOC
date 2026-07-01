# Слой 3 — двусторонний RPC к GSP-RM: проходы A + B (РЕШЕНО на железе)

**Статус:** 🟢 проход A (2026-06-30) + проход B (2026-07-01) на RTX 4070 Super
(AD104), Linux/VFIO.
**Доказательство:** `docs/hw-dumps/20260630-rtx4070s-layer3-rpc-OK.log` (A),
`docs/hw-dumps/20260701-rtx4070s-layer3-passB-OK.log` (B).
**Оркестратор:** `tools/gsp_boot_linux.c` (тот же прогон, что и слой 2; блоки «СЛОЙ 3»/«3B»).
**Портируемая логика:** `driver/gsp/gsp_rm.{c,h}`. Офлайн-тест: `tools/gsp_rm_test.c`
(`make gsp-rm-test`). Эталон — nouveau `r535.c`, nvrm-заголовки 535.113.01.

Этот документ — запись того, как после `GSP_INIT_DONE` (слой 2) поднимается
**двусторонний RPC** и делаются первые RPC слоя 3. Если регрессует — здесь детали.

---

## 0. Проходы и метрики

Слой 2 дал односторонний канал: cmdq до бута + дренаж событий из msgq. Слой 3
(память/GMMU/VRAM) — это набор RPC к живому GSP-RM, и для него нужен
**двусторонний RPC**: послать команду пост-бут, дождаться типизированного ответа.

**Проход A** (фундамент RPC + RM-клиент):
1. `GET_GSP_STATIC_INFO (65)`: GSP вернул `rpc_result=0` и **карту FB-регионов VRAM**.
   Двусторонний RPC работает + карта памяти GPU.
2. `GSP_RM_ALLOC (103)`: цепочка client→device→subdevice, `status=NV_OK(0)`.
   Наш RM-клиент на GPU — фундамент для аллокаций.

**Проход B** (память через RPC):
3. `GSP_RM_CONTROL (76)` / `FB_GET_INFO_V2 (0x20801303)`: `status=NV_OK`, реальный
   конфиг VRAM/heap (RAM=12282 МиБ, HEAP, BAR1=256 МиБ). Третий RPC-глагол работает.
4. `GSP_RM_ALLOC` `FERMI_VASPACE_A (0x90f1)`: `status=NV_OK` — **GPU VA-пространство
   (корень GMMU)** создано через RPC.

Дальше: аллокация VRAM-объекта (`NV01_MEMORY_LOCAL_USER`/`ALLOC_MEMORY`) и маппинг в
VA-пространство; RM-control'ы FIFO/GR для слоя 4 (каналы).

---

## 1. Реальный результат на железе (проходы A/B)

После `GSP_INIT_DONE` (msgq.writePtr=7): канал `cmdq.wptr=2 seq=2 msgq.rptr=7`.

`GET_GSP_STATIC_INFO` → 5 FB-регионов (вершина = размер VRAM):
```
FB[0] 0x0..0x30fffff           rsvd               (резерв внизу)
FB[1] 0x3100000..0x2f034ffff   comp=1 iso=1       ← большой пользовательский VRAM
FB[2] 0x2f0350000..0x2f528ffff comp=1 iso=1
FB[3] 0x2f5290000..0x2f52fffff rsvd
FB[4] 0x2f5300000..0x2ff9fffff                    ← limit+1 = 0x2ffa00000 = 12282 МиБ ✓
GSP internal: hClient=0xc2000005 hDevice=0xabcd0080 hSubdevice=0xabcd2080
              bar1Pde=0x2f3ca9000 bar2Pde=0x2f5293000
```
RM-цепочка:
```
RM client  status=0x0 handle=0xc1d00000
RM device  status=0x0 handle=0xde1d0000
RM subdev  status=0x0 handle=0x5d1d0000
```
Проход B (тот же канал, продолжение):
```
FB_GET_INFO_V2 status=0 (KiB): RAM=12576768 (=12282 МиБ) TOTAL_RAM=12576768
               HEAP=12355136 HEAP_FREE=57720 BAR1=262144 (=256 МиБ)
FERMI_VASPACE_A status=0 handle=0x90f10000       ← корень GMMU
```
`msgq.tx.writePtr` вырос 7→11 (A: static_info+3 alloc) →13 (B: control+vaspace).

---

## 2. Двусторонний RPC — `nv_gsp_rpc_call` (порт r535_gsp_cmdq_push + msg_recv)

Канал `nv_gsp_rpc_chan` держит `shm`, раскладку очередей, `cmdq_wptr`, `seq`,
`msgq_rptr` и колбэки `ring`(звонок 0xc00=0)/`udelay` (переносимость, как `nv_mmio_t`).

**Send:** строим элемент в `cmdq + entryOff + cmdq_wptr*0x1000` через
`nv_gsp_cmdq_write` (тот же, что у пре-бутовых RPC: rpc-заголовок + XOR-checksum +
elem_count). Продвигаем `cmdq_wptr += elem_count` (wrap по msgCount=63), пишем
`cmdq.tx.writePtr`, звоним в звонок, `seq++`.

**Recv (poll до 4с):** пока `msgq_rptr != msgq.tx.writePtr`: читаем элемент в слоте,
берём `elem_count`(@+40), `function`(@rpc+12), `length`(@rpc+8), `rpc_result`(@rpc+16).
Если `function==fn` — копируем payload (`length-32` байт) в выход, отдаём
`rpc_result`, двигаем `msgq_rptr += elem_count` (consume через `msgq.rx.readPtr`).
Иначе — async-событие GSP (0x101b/0x100c): пропускаем с consume и ждём дальше.
Периодически звоним в звонок.

**Продолжение указателей после INIT_DONE — критично:** `cmdq_wptr`/`seq` берём с
тех, что оставил пре-бут (writePtr=2, seq=2); `msgq_rptr` — финальный readPtr дренажа
(7). Не сбрасывать в 0 — иначе рассинхрон с GSP.

---

## 3. GET_GSP_STATIC_INFO (65) — `nv_gsp_get_static_info`

Запрос = `sizeof(GspStaticConfigInfo)=2168` нулей (GSP заполняет ответ, как
`nvkm_gsp_rpc_rd`). Ответ той же функции, `rpc_result=0`. Парсим по смещениям
(получены **compile-probe против SDK 535.113.01**, см. `gsp_rm.c` «PROBE»):
```
sizeof(GspStaticConfigInfo) = 2168
fbRegionInfoParams @840 : numFBRegions@0 (u32), fbRegion[16]@8 (48б/регион)
   регион: base@0 limit@8 reserved@16 performance@24 supportCompressed@28
           supportISO@29 bProtected@30
bar1PdeBase @2088, bar2PdeBase @2096 (u64)
hInternalClient @2152, hInternalDevice @2156, hInternalSubdevice @2160
```
Элемент req/reply = 48+32+2168 = 2248 ≤ 4096 → **одностраничный** (ec=1).
**Самопроверка на железе:** `numFBRegions∈[1..16]`; карта согласована с известным
размером VRAM (вершина FB[4]+1 = 0x2ffa00000). Неверное смещение → мусор → ловим.

---

## 4. GSP_RM_ALLOC (103) — `nv_gsp_rm_alloc` + конструкторы цепочки

`rpc_gsp_rm_alloc_v03_00` (шапка 32б, g_rpc-structures.h):
```
hClient@0 hParent@4 hObject@8 hClass@12 status@16 paramsSize@20 flags@24 reserved[4]@28 params@32
```
req payload = шапка + params; ответ — та же шапка с `status` (NV_OK==0).
Канонические хэндлы/классы (ровно как nouveau r535_gsp_*_ctor):

| объект | hObject | hParent | hClass | params (sizeof) |
|---|---|---|---|---|
| client (root) | `0xc1d00000` | self | `NV01_ROOT` (0x0) | NV0000{hClient, processID=~0, name[100]} (108) |
| device | `0xde1d0000` | client | `NV01_DEVICE_0` (0x80) | NV0080{hClientShare=client, …} (56) |
| subdevice | `0x5d1d0000` | device | `NV20_SUBDEVICE_0` (0x2080) | NV2080{subDeviceId=0} (4) |

Делаем последовательно (ждём ответ каждого до следующего), `status` каждого = 0.

---

## 4B. Проход B — RM_CONTROL + FB_GET_INFO_V2 + FERMI_VASPACE_A

### GSP_RM_CONTROL (76) — `nv_gsp_rm_control` (порт r535_gsp_rpc_rm_ctrl_get/push)
`rpc_gsp_rm_control_v03_00` (шапка 24б, g_rpc-structures.h):
```
hClient@0 hObject@4 cmd@8 status@12 paramsSize@16 flags@20 params@24
```
params — **IN/OUT**: запрос копируется из буфера вызывающего, ответ GSP пишется
обратно в тот же буфер. `status` (@12 ответа) = NV_OK(0). Тот же двусторонний
`nv_gsp_rpc_call` с fn=76.

### FB_GET_INFO_V2 (0x20801303) — `nv_gsp_fb_get_info`
params = `NV2080_CTRL_FB_GET_INFO_V2_PARAMS` (436б): `fbInfoListSize@0` (u32),
`fbInfoList[54]@4` — каждый `NV2080_CTRL_FB_INFO = {index(u32), data(u32)}` (8б).
Заполняем `index` для каждого запрошенного, `data=0`; GSP возвращает `data`.
Индексы (значения в KiB): RAM_SIZE=0x07, TOTAL_RAM_SIZE=0x08, HEAP_SIZE=0x09,
HEAP_FREE=0x16, BAR1_SIZE=0x05, USABLE_RAM_SIZE=0x20.
На железе: RAM=12576768 KiB = ровно 12282 МиБ (совпало с PMC/GET_STATIC_INFO).
Контрол шлём на **наш** subdevice (`0x5d1d0000`). cmd номер — из полного
`ctrl2080fb.h` (OGK 535.113.01; в nouveau-выжимке отсутствовал, поэтому дотянут
из того же публичного репозитория).

### FERMI_VASPACE_A (0x90f1) — `nv_gsp_rm_vaspace_ctor`
RM_ALLOC под device (hParent=device, hObject=`0x90f10000`, hClass=0x90f1).
params = `NV_VASPACE_ALLOCATION_PARAMETERS` (nvos.h, 48б):
```
index@0 flags@4 vaSize@8 vaStartInternal@16 vaLimitInternal@24 bigPageSize@32 vaBase@40
```
Заполняем `index=NV_VASPACE_ALLOCATION_INDEX_GPU_NEW(0)`, прочее 0 → дефолтное
новое полное VA-пространство. На железе: `status=NV_OK`. Это корень GMMU —
основа для маппинга VRAM в GPU-адреса (следующий шаг).

---

## 5. Тупики/нюансы (чтобы не потерять)

- **`cmdq.rx.readPtr` остаётся 0** в диагностике — это НЕ значит, что GSP не читает
  cmdq. Факт обработки виден по росту `msgq.tx.writePtr` (7→11) и `status=0` ответов.
  Не «чинить».
- **Указатели канала продолжаются** после INIT_DONE (см. §2). Сброс = рассинхрон.
- **Смещения GspStaticConfigInfo версионны** (535.113.01). При смене версии прошивки
  пере-снять compile-probe. Самопроверка FB-карты — страховка.
- Ответы этого прохода ≤ 1 страницы; multi-page сборка реализована и протестирована
  офлайн (`tools/gsp_rm_test.c`), но на железе пока не задействована.

---

## 6. Источники (точные)

- nouveau `r535.c`: `r535_gsp_cmdq_push`, `r535_gsp_msgq_wait/recv`,
  `r535_gsp_msg_recv`, `r535_gsp_rpc_send`, `r535_gsp_rpc_rm_alloc_get/push`,
  `r535_gsp_rpc_rm_ctrl_get/push`, `r535_gsp_client_ctor`/`device_ctor`/
  `subdevice_ctor`, `r535_gsp_rpc_get_gsp_static_info`/`postinit`.
- nvrm 535.113.01: `g_rpc-structures.h` (rpc_gsp_rm_alloc/control_v03_00),
  `rpc_global_enums.h` (GET_GSP_STATIC_INFO=65, GSP_RM_CONTROL=76, GSP_RM_ALLOC=103),
  `cl0000.h`/`cl0080.h`/`cl2080.h` (классы+alloc-params), `cl90f1.h`
  (FERMI_VASPACE_A=0x90f1), `nvos.h` (NV_VASPACE_ALLOCATION_PARAMETERS),
  `nvlimits.h` (NV_PROC_NAME_MAX_LENGTH=100), `gsp_static_config.h`
  (GspStaticConfigInfo), `ctrl2080fb.h` (FB_REGION_INFO; FB_GET_INFO_V2=0x20801303
  + индексы — из полного OGK-заголовка 535.113.01).

Реализация: `driver/gsp/gsp_rm.{c,h}`, очереди `driver/gsp/gsp_rpc.{c,h}`,
оркестратор `tools/gsp_boot_linux.c` (блоки «СЛОЙ 3»/«3B»).
