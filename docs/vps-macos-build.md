# Сборка драйверов на VPS (macOS в KVM)

Цель: собрать `RTXProbe` (kext) и `RTXProbeDext` (dext) на реальной macOS,
запущенной в KVM на Linux-VPS. Это закрывает шаги 4–5 Недели 1 (компиляция).
RTX тут нет — прогон на железе остаётся на домашнем ПК.

## Проверенные характеристики стенда (VPS Xipher)

| Параметр | Значение | Годность |
|---|---|---|
| /dev/kvm | есть | ✓ аппаратное ускорение |
| vmx (vt-x) | проброшен | ✓ |
| Диск | 151 ГБ свободно | ✓ (нужно ~70) |
| RAM | 9.7 ГБ | ⚠ гостю 8, впритык, но ок |
| vCPU | 5 | ✓ |
| AVX2 | **нет** (Xeon E5 v2, Ivy Bridge) | ⛔ → потолок macOS **Monterey 12** |

> Не пытаться ставить Ventura/Sonoma — без AVX2 ядро macOS 13+ паникует на бутах.
> Monterey (12) содержит Xcode 13/14 с DriverKit + PCIDriverKit — этого достаточно.

> Правовая заметка: EULA macOS разрешает запуск только на Apple-железе. Запуск в
> VM для разработки — серая зона. Используешь под свою ответственность.

---

## 1. Зависимости (на VPS, Ubuntu 22.04)

```sh
sudo apt-get update
sudo apt-get install -y qemu-system-x86 qemu-utils qemu-kvm \
    libvirt-daemon-system bridge-utils python3 python3-pip git
sudo usermod -aG kvm "$USER"   # перелогиниться/новый ssh после этого
```

## 2. OSX-KVM + образ Monterey

```sh
git clone --depth 1 https://github.com/kholia/OSX-KVM.git
cd OSX-KVM
./fetch-macOS-v2.py        # в меню выбрать: Monterey  (НЕ Ventura+)
qemu-img convert BaseSystem.dmg -O raw BaseSystem.img
qemu-img create -f qcow2 mac_hdd_ng.img 64G
```

## 3. Настроить ресурсы гостя

В файле `OpenCore-Boot.sh` выставить:

```sh
ALLOCATED_RAM="8192"     # 8 ГБ гостю
CPU_SOCKETS="1"
CPU_CORES="4"
CPU_THREADS="4"
```

## 4. Запуск headless через VNC (безопасно — только localhost)

OSX-KVM по умолчанию открывает GUI-дисплей. На headless-VPS вместо этого вешаем
VNC **на 127.0.0.1** (не наружу!). В конце команды запуска QEMU в `OpenCore-Boot.sh`
заменить опцию дисплея на:

```
-display none -vnc 127.0.0.1:0
```

Запустить:

```sh
./OpenCore-Boot.sh
```

VNC теперь слушает порт 5900 только локально на VPS.

## 5. Подключиться с домашнего ПК (Windows) по SSH-туннелю

На Windows (PowerShell), прокинуть локальный порт к VNC на VPS:

```powershell
ssh -L 5900:127.0.0.1:5900 root@<IP_VPS> -N
```

Затем VNC-клиентом (TigerVNC/RealVNC) подключиться к `127.0.0.1:5900`.
Так VNC не торчит в интернет — трафик идёт внутри SSH.

## 6. Установка macOS Monterey (через VNC)

1. В OpenCore boot-picker выбрать **macOS Base System**.
2. Disk Utility → стереть `QEMU HARDDISK` (64 ГБ) → APFS, имя `macOS`.
3. Выйти → Reinstall macOS Monterey → ставить на этот том.
4. Несколько перезагрузок (на каждой выбирать загрузку с диска `macOS`). KVM
   ускоряет — без AVX2 всё равно работает, Monterey его не требует.

## 7. Xcode

- Поставить полный **Xcode 14.2** (последний для Monterey): App Store (нужен Apple ID)
  или `.xip` с developer.apple.com/download/all (Xcode 14.2).
- После установки:
  ```sh
  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
  sudo xcodebuild -license accept
  xcodebuild -showsdks | grep -iE 'macos|driverkit'   # должны быть оба SDK
  ```

## 8. Забрать исходники проекта в гостя

В macOS-госте (Terminal):

```sh
git clone <URL_твоего_репо> RTXmacOC
cd RTXmacOC
```
(или `scp -r` с домашней машины через тот же SSH-хост.)

## 9. Сборка драйверов

Исходники готовы (`driver/RTXProbe`, `driver/RTXProbeDext`, общий `ada_regs.h`).
Нужно один раз завести Xcode-таргеты:

**kext (RTXProbe):**
1. Xcode → New Project → macOS → (Other) **Generic Kernel Extension**.
2. Добавить в таргет `RTXProbe.cpp`, `RTXProbe.hpp`, подключить `../../ada_regs.h`,
   заменить Info.plist на наш (с `IOPCIPrimaryMatch 0x278310de`).
3. Сборка:
   ```sh
   xcodebuild -scheme RTXProbe -configuration Debug build
   ```

**dext (RTXProbeDext):**
1. Xcode → New Project → **DriverKit Driver**.
2. Добавить `RTXProbeDext.iig`, `RTXProbeDext.cpp`, `ada_regs.h`, наш Info.plist
   и entitlements.
3. Сборка:
   ```sh
   xcodebuild -scheme RTXProbeDext -configuration Debug build
   ```

Цель шага — `BUILD SUCCEEDED` для обоих. Логи сложить в `docs/hw-dumps/`.

## Что это даёт и чего нет

- ✓ Подтверждаем, что драйверы реально компилируются Apple-тулчейном (шаги 4–5).
- ✓ Можно итеративно править код драйвера и сразу пересобирать.
- ⛔ Нельзя загрузить драйвер и прочитать RTX — карты в VPS нет. Это только дома.

## Возможные грабли

- **Гость паникует/циклит на буте:** почти всегда — попытка поставить macOS новее
  Monterey (AVX2). Ставить строго Monterey/Big Sur.
- **Мало RAM:** если 8 ГБ гостю не хватает и VPS душит — уменьшить до 6144, сборка
  медленнее, но идёт.
- **VNC не коннектится:** проверь SSH-туннель и что QEMU реально вешает `-vnc 127.0.0.1:0`.
