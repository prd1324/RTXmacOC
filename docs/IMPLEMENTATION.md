# IMPLEMENTATION.md — состояние реализации (источник правды)

> Этот файл фиксирует, **что уже реализовано, где и как**, и **что делать дальше**,
> чтобы не сбиться с пути корректной реализации. Обновляется после каждого
> значимого шага. Парный документ — [PORTING-MAP.md](./PORTING-MAP.md) (соответствие
> наш код ↔ исходники nova-core/open-gpu-kernel-modules).
>
> Правило: **все регистры/структуры/последовательности портируются из публичных
> исходников и сверяются** (см. AGENTS.md). Ничего не выдумывать.

---

## Карта слоёв и статус

| Слой | Что | Статус | Файлы |
|---|---|---|---|
| 1 | PCIe bring-up, чтение `PMC_BOOT_0` | ✅ готово, CI зелёный | `pcie_probe.c`, `ada_regs.h`, `driver/RTXProbe/`, `driver/RTXProbeDext/` |
| 2 | GSP bring-up | 🔨 в работе | `tools/vbios_dump.c`, `falcon_regs.h`, `driver/gsp/falcon.*` |
| 3–6 | память/каналы/дисплей/Metal | ⏳ | — |

---

## Слой 1 — реализовано ✅

### `pcie_probe.c`
User-space разведка PCIe через IOKit: vendor/device/subsystem/class/BAR из
IORegistry. Без драйвера и без отключения SIP. Собирается в CI.

### `ada_regs.h`
Определения `NV_PMC_BOOT_0` (offset 0x0) и декод чипа:
- `architecture[28:24]`, `implementation[23:20]`, `chipset=(raw>>20)&0x1ff`.
- Сверено с nova-core: AD104 → chipset `0x194`, arch `0x19` (Ada).
- Имена арх/чипа, проверка правдоподобности значения.

### `driver/RTXProbe/` (kext) и `driver/RTXProbeDext/` (dext)
Матчатся на `10DE:2783` (`IOPCIPrimaryMatch`), мапят BAR0, читают `PMC_BOOT_0`,
декодят и логируют «Ada AD104». kext — основной канал (OpenCore), dext — через
PCIDriverKit `MemoryRead32`. Сборка проверяется в CI (kext обязателен).

**Метрика слоя 1 (чтение chip ID) — выполнена в коде; на железе ждёт macOS на ПК.**

---

## Слой 2 — реализовано частично 🔨

Полный план — [gsp-bringup-notes.md](./gsp-bringup-notes.md). Ниже — что уже есть.

### Шаг 1 — VBIOS reader / FWSEC locator ✅ — `tools/vbios_dump.c`
Алгоритм (портирован из nova-core `vbios.rs`):
1. Читает VBIOS (файл-дамп или Linux sysfs `rom`).
2. Идёт по образам PCI ROM (PciRomHeader `0xAA55` → PCIR/NPDS; размер из NPDE при
   наличии, иначе PCIR; выравнивание 512).
3. Находит **PciAt** (code_type `0x00`) и **FWSEC** (`0xE0`).
4. В PciAt ищет **BIT** (`FF B8 'B' 'I' 'T' 00`), берёт токен **FALCON_DATA (0x70)**,
   читает 4-байтный указатель → `falcon_data_off = ptr - sizeof(PciAt)`.
5. В FWSEC-секции по этому смещению парсит **PmuLookupTable**, ищет запись
   **app id 0x85 (FWSEC PROD)** → `falcon_ucode_off = entry.data - sizeof(PciAt)`.
6. Читает дескриптор: **`FalconUCodeDescV3`** (44 байта) или V2 (60). Печатает
   `imem_load`, `dmem_load`, `signature_count`, `engine_id_mask`, `ucode_id`,
   смещение ucode. `--extract` пишет блоб ucode (imem||dmem) в файл.

Все смещения/структуры — из `vbios.rs` и `firmware.rs` (см. PORTING-MAP).

### Шаг 2 — движок Falcon ✅ — `falcon_regs.h` + `driver/gsp/falcon.{h,c}`
`falcon_regs.h`: регистры PFALCON/PFALCON2, **база GSP `0x110000`/`0x111000`**
(подтверждено `falcon/gsp.rs`), биты CPUCTL/HWCFG2/DMACTL/ENGINE/DMATRFCMD/FBIF,
WPR2-границы, GFW boot, `NV_PGSP_QUEUE_HEAD`.

`driver/gsp/falcon.{h,c}` — поверх абстрактного MMIO+DMA (`nv_mmio_t`):
- `nv_falcon_reset_ga102`: reset_ready(≤150us, необяз.) → reset движка → ожидание
  скраба → `select_core`(Falcon) → `FALCON_RM = PMC_BOOT_0`. (falcon.rs + ga102.rs)
- `nv_falcon_select_core_ga102`: BCR_CTRL core_select=Falcon, ждать valid.
- `nv_falcon_program_brom_ga102`: BROM_PARAADDR/ENGIDMASK/CURR_UCODE_ID, MOD_SEL=RSA3K.
- `nv_falcon_dma_reset` + `nv_falcon_dma_wr` + `nv_falcon_dma_load_ga102`:
  FBIF→coherent sysmem/physical, цикл `DMATRFCMD` (256-байт блоки), IMEM(sec)+DMEM,
  BOOTVEC. (falcon.rs dma_wr/dma_load)
- `nv_falcon_start`/`wait_halted`/`boot`/mailbox.
- `nv_wpr2_is_set`, `nv_gfw_boot_completed` — проверки результата.

Компилируется в CI (portable C).

### Что в слое 2 ещё НЕ сделано (точные следующие шаги)
1. **Патч FWSEC-образа** — `driver/gsp/fwsec_patch.{h,c}` (порт `fwsec.rs::new_fwsec`):
   - найти `FalconAppifHdrV1` по `imem_load_size + interface_offset`;
   - перебрать записи `FalconAppifV1`, найти `DMEMMAPPER (id=0x4)`;
   - в `FalconAppifDmemmapperV3` выставить `init_cmd = FRTS (0x15)`;
   - по `cmd_in_buffer_offset` записать `FrtsCmd` (ReadVbios + FrtsRegion, type FB=2,
     addr/size = frts_addr/size >> 12);
   - подпись: по `signature_count`/`signature_versions` и fuse-версии выбрать
     индекс, скопировать `Bcrt30Rsa3kSignature` (384б) в `imem_load + pkc_data_offset`.
2. **Kernel DMA в kext** — выделить coherent-буфер (`IOBufferMemoryDescriptor`),
   скопировать пропатченный ucode, получить физ. адрес → `nv_falcon_dma_load_ga102`.
3. **Запуск FWSEC-FRTS** — `reset → dma_load → boot(mbox0=0) → mbox0==0 &&
   nv_wpr2_is_set()`. ← создан WPR2.
4. **Booter Loader** (порт `firmware/booter.rs`) → грузит GSP-RM в WPR2, старт RISC-V.
5. **GSP-RM + очереди RPC** (`open-gpu-kernel-modules` `message_queue_priv.h`,
   nouveau `r535.c`) → первый RPC. ← **метрика слоя 2**.

---

## Инварианты (не нарушать, иначе «сходим с пути»)

1. Регистры/структуры/смещения — только из источников, со сверкой. Непроверенное —
   `TODO: verify`.
2. Прошивки (FWSEC из VBIOS, Booter, GSP-RM) — грузить как есть, не менять (кроме
   санкционированного патча FWSEC: init_cmd + signature, как в `fwsec.rs`).
3. Код примитивов — на абстрактном интерфейсе (`nv_mmio_t`), чтобы работал в
   kext/dext/тесте. Платформенная привязка (DMA-аллокатор, маппинг BAR) — снаружи.
4. Каждый шаг — проверяем headless (регистры, mailbox, WPR2), без монитора.
5. Целевой чип — Ada AD104 (`10DE:2783`), путь GA102-HAL. Hopper/Blackwell (FSP) —
   не наш кейс сейчас.

## Где что искать
- Стратегия: `docs/ARCHITECTURE.md`
- План слоя 2: `docs/gsp-bringup-notes.md`
- Соответствие исходникам: `docs/PORTING-MAP.md`
- Правила: `AGENTS.md`
