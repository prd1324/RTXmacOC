# bench-test-fwsec.md — runbook HW-верификации FWSEC-FRTS (задача 8)

> Цель прогона: на реальной RTX 4070 Super (AD104, `10DE:2783`) загрузить kext
> `RTXProbe`, который автоматически выполнит FWSEC-FRTS, и снять лог по SSH.
> **Метрика слоя 2 (первая):** `mbox0 == 0` И `WPR2 set = 1`. До этого момента
> весь слой 2 — 📄 SRC / 🟡 CI; только этот лог даёт право на 🟢.
>
> Код пути сверен с nova-core построчно (см. `PORTING-MAP.md` → «Аудит против
> nova-core», 2026-06-28). На железе ещё не исполнялся.

---

## 0. Что именно произойдёт при загрузке kext

`RTXProbe::start()` (см. `driver/RTXProbe/RTXProbe.cpp`):
1. матчится на `10DE:2783`, открывает устройство, включает Memory Space + Bus Master;
2. мапит BAR0, читает `NV_PMC_BOOT_0`, декодит Ada AD104;
3. **если чип — Ada**, вызывает `RTXRunFwsecFrts(fPci, bar0)` (`FwsecRun.cpp`):
   - читает VBIOS из ROM-shadow `BAR0 + 0x300000`;
   - находит и парсит дескриптор FWSEC (V3), вычисляет регион FRTS из объёма VRAM;
   - копирует ucode в coherent DMA-буфер, патчит (FRTS-команда + RSA3K-подпись);
   - `reset(GA102) → dma_load → boot(mbox0=0)`;
   - читает `mbox0` и регистры WPR2, логирует результат.

Это **hardware-операция**: сбрасывает GSP-Falcon и создаёт регион WPR2 в VRAM.
Дисплея на стенде нет (i5-12400F без iGPU) — ожидаем чёрный экран, работаем headless
по SSH. FWSEC выполняется на каждом старте драйвера (на загрузке kext) — это и
есть turnkey-прогон.

---

## 1. Стенд (один раз)

Сборка стенда расписана в `docs/testbed.md`/`HANDOFF.md`. Кратко:

1. **Станция установки** — машина с рабочей iGPU (Haswell). На неё ставим
   **macOS Monterey на USB-SSD** (для слоёв 1–2 версия неважна; Tahoe Haswell не тянет).
2. На этом же SSD:
   - включить **Remote Login** (SSH): `Системные настройки → Общий доступ → Удалённый вход`;
   - убедиться, что есть сетевой доступ (Ethernet проще всего, headless).
3. **OpenCore** на USB-SSD настроить под **Alder Lake** (целевая машina i5-12400F,
   Gigabyte B760M DS3H DDR4). EFI переносится вместе с SSD.
4. Перенести USB-SSD в RTX-машину → грузимся headless → заходим по SSH.

> Дешёвая предварительная проверка без macOS: `tools/nv_mmio_linux.c` с Linux
> live-USB или RW-Everything на Windows (так уже сняли `PMC_BOOT_0=0x194000A1`,
> `docs/hw-dumps/20260628-*`). Windows не сносить.

---

## 2. Сборка kext (на станции установки или любом Mac/Xcode)

В kext-таргет `RTXProbe.kext` должны входить **все** исходники пути, а не только
`RTXProbe.cpp`:

```
driver/RTXProbe/RTXProbe.cpp
driver/RTXProbe/FwsecRun.cpp
driver/gsp/falcon.c
driver/gsp/fwsec_patch.c
driver/gsp/fwsec_locate.c
driver/gsp/fb_layout.c
```

Заголовки: `driver/RTXProbe/*.hpp`, `driver/gsp/*.h`, `falcon_regs.h`, `ada_regs.h`.
`Info.plist` — `driver/RTXProbe/Info.plist` (bundle id `org.rtxmacoc.RTXProbe`,
матч `IOPCIPrimaryMatch 0x278310de`).

> CI (`.github/workflows/build-macos.yml`) делает только **compile-check** каждого
> файла (`-c`), а не `.kext`-бандл. Бандл собирается Xcode-таргетом локально.
> Минимальный путь без полного проекта Xcode — собрать объектники как в CI, затем
> слинковать kext (`-fapple-kext -bundle -lkmod ...`) и сложить бандл
> `RTXProbe.kext/Contents/{Info.plist, MacOS/RTXProbe}`. Проще — Xcode «Generic
> Kernel Extension» target со списком файлов выше.

Проверка структуры бандла:
```sh
kextlibs -xml RTXProbe.kext            # зависимости резолвятся
codesign -dv RTXProbe.kext 2>&1 | head # подпись (для self-load не обязательна при SIP relax)
```

---

## 3. Релакс SIP и инжект через OpenCore

В `config.plist` OpenCore:

- **NVRAM → boot-args**: добавить `-v keepsyms=1` (verbose + читаемые символы в панике).
- **SIP**: `csr-active-config` ослабить так, чтобы пускались неподписанные kext и
  разрешались записи. Рабочий вариант для разработки — `csr-active-config = 03080000`
  (`CSR_ALLOW_UNTRUSTED_KEXTS | CSR_ALLOW_UNRESTRICTED_FS | CSR_ALLOW_KERNEL_DEBUGGER`),
  при необходимости полнее — `FF0F0000` (SIP off). Минимально нужно снять проверку
  подписи kext.
- **Kernel → Add**: добавить запись на `RTXProbe.kext` (`BundlePath=RTXProbe.kext`,
  `ExecutablePath=Contents/MacOS/RTXProbe`, `PlistPath=Contents/Info.plist`,
  `Enabled=true`), сам бандл положить в `EFI/OC/Kexts/`.

> Альтернатива для итераций без перепрошивки EFI: при ослабленном SIP грузить kext
> вручную (см. §4).

---

## 4. Загрузка kext и снятие лога по SSH

Заходим по SSH на стенд. Вариант A — kext уже инжектнут OpenCore при загрузке
(ничего делать не надо, лог уже есть). Вариант B — грузим вручную:

```sh
# при ослабленном SIP:
sudo cp -R RTXProbe.kext /tmp/
sudo kmutil load -p /tmp/RTXProbe.kext        # современный загрузчик kext
# (старый путь: sudo kextutil -v /tmp/RTXProbe.kext)
```

Снять лог ядра (IOLog → системный лог). Сразу фильтруем по нашим тегам:

```sh
log show --last 5m --predicate 'process == "kernel"' \
  | grep -E 'RTXProbe|RTXFwsec' | tee ~/fwsec-run.txt
# живой поток (если грузим вручную и хотим смотреть в реальном времени):
log stream --predicate 'process == "kernel"' | grep -E 'RTXProbe|RTXFwsec'
```

Скопировать лог на dev-машину и положить в репозиторий:

```sh
# с dev-машины:
scp user@bench:~/fwsec-run.txt docs/hw-dumps/$(date +%Y%m%d)-rtx4070s-fwsec-frts.log
```

---

## 5. Что ждём в логе (критерий успеха)

Успешный прогон (порядок строк):

```
RTXProbe: matched PCI device 10de:2783
RTXProbe: NV_PMC_BOOT_0 = 0x194000a1
RTXProbe: chip arch=0x19 impl=0x4 chipset=0x194 rev=10.1 -> Ada / AD104
RTXProbe: *** ВИЖУ Ada AD104 ***
RTXProbe: === запуск FWSEC-FRTS (layer 2) ===
RTXFwsec: FRTS region addr=0x<...> size=0x100000
RTXFwsec: FWSEC done, mbox0=0x00000000, WPR2 set=1 [0x<lo>..0x<hi>]
RTXFwsec: *** FWSEC-FRTS OK, WPR2 created ***
RTXProbe: RTXRunFwsecFrts вернул 0x00000000 (OK)
```

**Метрика слоя 2 достигнута, если одновременно:**
- `mbox0 = 0x00000000` (FWSEC отработал без ошибки), и
- `WPR2 set = 1` с непустым диапазоном `[lo..hi]` (регион реально создан).

`size = 0x100000` (1 MiB) — ожидаемый размер FRTS-региона (`frts_size_tu102`, сверено).
Адрес FRTS лежит в верхней части VRAM (около `vidmem - ~1.1 MiB`, зависит от VGA workspace).

---

## 6. Диагностика по симптомам

| Строка лога | Что значит | Куда смотреть |
|---|---|---|
| `PMC_BOOT_0 implausible` | карта не запитана / BAR0 не на регистрах | питание, слот, маппинг BAR0 |
| `failed to compute FRTS region (VRAM size?)` | `NV_USABLE_FB_SIZE_IN_MB` (0x1183a4) = 0 | GFW boot мог не пройти; проверить `nv_gfw_boot_completed` отдельно |
| `FWSEC not located in VBIOS` | не найден образ FWSEC в ROM-shadow | дамп `BAR0+0x300000`: первый word должен быть `0xAA55`, не `NVGI` (см. D3 в `fwsec_locate.c`) |
| `unsupported FWSEC desc version` | дескриптор не V3 | неожиданно для AD104 — снять дамп дескриптора |
| `no matching signature (fuse N)` | fuse-версия не покрыта подписями FWSEC | сверить `signature_versions` дескриптора и fuse-регистр |
| `falcon reset failed` / `dma_load failed` | таймаут скраба/DMA или select_core | прочитать `HWCFG2`, `DMATRFCMD`, `BCR_CTRL` вручную |
| `falcon boot timed out` | Falcon не дошёл до halted за 2с | возможна неверная подпись/BROM; проверить `mbox0`/`mbox1` после |
| `mbox0 != 0` при halted | FWSEC вернул код ошибки | значение `mbox0` = код; также `NV_PBUS_SW_SCRATCH[0xe]` (FRTS err, бит 31:16) |
| `WPR2 set=0` при `mbox0=0` | WPR2 не выставлен | редкость; перечитать `0x1FA824/828`, сверить регион |

Полезные регистры для ручного чтения (через отдельный мини-патч или RW-инструмент):
- `0x1FA824 / 0x1FA828` — WPR2 lo/hi (граница = `(val>>4)<<12`; `hi>>4 != 0` ⇒ задан);
- `0x118234` — GFW boot progress (`& 0xFF == 0xFF` ⇒ завершён);
- `0x1400 + 0xe*4` (`NV_PBUS_SW_SCRATCH[0xe]`) — код ошибки FRTS в битах 31:16.

---

## 7. Гейт честности (AGENTS.md, Правило 0)

- Ставить **🟢** в `IMPLEMENTATION.md`/`HANDOFF.md`/`PORTING-MAP.md` можно **только**
  после появления файла `docs/hw-dumps/<дата>-rtx4070s-fwsec-frts.log` с реальными
  строками `mbox0=0x00000000` и `WPR2 set=1`.
- Перед пометкой 🟢 — явная строка: «Доказательство: docs/hw-dumps/<файл>».
- Пока такого файла нет, статус FWSEC-FRTS — не выше 🟡 CI / 📄 SRC.
- Только после 🟢 переходить к задаче 6 (Booter → GSP-RM). Не раньше.
