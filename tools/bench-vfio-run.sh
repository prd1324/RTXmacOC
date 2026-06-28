#!/usr/bin/env bash
#
# bench-vfio-run.sh — прогон FWSEC-FRTS на стенде ОДНОЙ командой, без аргументов.
# (см. docs/bench-test-fwsec-linux.md)
#
# Запуск из папки на флешке, куда положены этот скрипт и бинарь fwsec_run_linux:
#   sudo bash bench-vfio-run.sh
#
# Скрипт сам:
#   * находит BDF карты NVIDIA (не нужен lspci);
#   * пишет лог рядом с собой на флешку (монтировать вручную не нужно — ты ведь уже
#     запускаешь его с флешки, значит раздел смонтирован);
#   * копирует бинарь в /tmp и делает исполняемым (обход noexec на FAT/exFAT);
#   * проверяет IOMMU, привязывает обе функции карты к vfio-pci, запускает харнесс;
#   * дублирует весь вывод в лог-файл, который переживёт гашение экрана после vfio.
#
# Опционально можно передать BDF первым аргументом: sudo bash bench-vfio-run.sh 0000:01:00.0
set -u

# Папка, где лежит сам скрипт (это и есть флешка, если запускать оттуда).
SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
DIR="$(dirname "$SELF")"
LOG="$DIR/fwsec-frts.log"
SRC_HARNESS="$DIR/fwsec_run_linux"
RUN_HARNESS="/tmp/fwsec_run_linux"

# Авто-детект BDF NVIDIA, если не задан аргументом.
detect_bdf() {
    for d in /sys/bus/pci/devices/*; do
        [ -r "$d/vendor" ] || continue
        [ "$(cat "$d/vendor")" = "0x10de" ] || continue
        local c; c="$(cat "$d/class" 2>/dev/null)"
        case "$c" in
            0x0300*) basename "$d"; return 0 ;;   # VGA — приоритет
        esac
    done
    # запасной проход: любой дисплейный класс 0x03xxxx
    for d in /sys/bus/pci/devices/*; do
        [ -r "$d/vendor" ] || continue
        [ "$(cat "$d/vendor")" = "0x10de" ] || continue
        case "$(cat "$d/class" 2>/dev/null)" in
            0x03*) basename "$d"; return 0 ;;
        esac
    done
    return 1
}

{
    echo "=== bench-vfio-run $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    echo "kernel: $(uname -r)"

    if [ "$(id -u)" -ne 0 ]; then
        echo "ERR: нужен root. Запусти: sudo bash $0"
        exit 1
    fi

    BDF="${1:-}"
    if [ -z "$BDF" ]; then
        BDF="$(detect_bdf)" || { echo "ERR: карта NVIDIA не найдена для авто-детекта."; exit 1; }
        echo "BDF (авто-детект): $BDF"
    else
        echo "BDF (задан): $BDF"
    fi

    # IOMMU активен?
    if [ -z "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]; then
        echo "ERR: IOMMU не активен. В BIOS включи VT-d; в GRUB на загрузке добавь 'intel_iommu=on'."
        exit 1
    fi
    echo "IOMMU: активен ($(ls /sys/kernel/iommu_groups | wc -l) групп)"

    # Подготовить бинарь (обход noexec на флешке).
    if [ ! -f "$SRC_HARNESS" ]; then
        echo "ERR: не найден бинарь $SRC_HARNESS (положи fwsec_run_linux рядом со скриптом)."
        exit 1
    fi
    cp -f "$SRC_HARNESS" "$RUN_HARNESS"
    chmod +x "$RUN_HARNESS"

    modprobe vfio-pci 2>/dev/null || true

    # Привязать обе функции карты (видео .0 + аудио .1) к vfio-pci.
    base="${BDF%.*}"
    for fn in "${base}.0" "${base}.1"; do
        [ -e "/sys/bus/pci/devices/$fn" ] || continue
        echo "--- $fn ---"
        if [ -e "/sys/bus/pci/devices/$fn/driver" ]; then
            cur="$(basename "$(readlink "/sys/bus/pci/devices/$fn/driver")")"
            echo "  драйвер: $cur -> unbind"
            echo "$fn" > "/sys/bus/pci/devices/$fn/driver/unbind" 2>/dev/null || true
        fi
        echo vfio-pci > "/sys/bus/pci/devices/$fn/driver_override" 2>/dev/null || true
        echo "$fn" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
        if [ -e "/sys/bus/pci/devices/$fn/driver" ]; then
            echo "  теперь: $(basename "$(readlink "/sys/bus/pci/devices/$fn/driver")")"
        else
            echo "  теперь: none"
        fi
    done

    echo "=== запуск харнесса (экран может погаснуть — норма, лог пишется в файл) ==="
    "$RUN_HARNESS" "$BDF"
    echo "=== харнесс завершился, код=$? ==="
} 2>&1 | tee "$LOG"

sync
echo
echo "Готово. Лог: $LOG"
echo "Перезагрузись в Windows и пришли этот файл."
