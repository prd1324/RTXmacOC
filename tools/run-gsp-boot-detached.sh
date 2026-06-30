#!/usr/bin/env bash
#
# run-gsp-boot-detached.sh — полная загрузка GSP-RM (фаза 6) БЕЗ ребута, с возвратом экрана.
# Аналог run-fwsec-detached.sh, но гоняет tools/gsp_boot_linux (фаза 3 задачи 6).
#
# ЗАПУСКАТЬ ОТЦЕПЛЕННО:
#   sudo systemd-run --unit=rtx-booter --collect bash /ABS/PATH/tools/run-gsp-boot-detached.sh
# Результат: tools/gsp-boot.log ; маркер: tools/gsp-boot-DONE
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH   # для zstd (распаковка блоба в харнессе)

DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/gsp-boot.log"
HARNESS_SRC="$DIR/gsp_boot_linux"
HARNESS="/tmp/gsp_boot_linux"
BDF="0000:01:00.0"
AUD="0000:01:00.1"

exec > "$LOG" 2>&1
drv_of() { basename "$(readlink "/sys/bus/pci/devices/$1/driver" 2>/dev/null)" 2>/dev/null; }

restore_gui() {
    echo "-- [restore] вернуть GPU nvidia и поднять gdm --"
    for fn in "$BDF" "$AUD"; do
        [ -e "/sys/bus/pci/devices/$fn" ] || continue
        if [ -e "/sys/bus/pci/devices/$fn/driver" ] && [ "$(drv_of "$fn")" = "vfio-pci" ]; then
            timeout 15 bash -c "echo '$fn' > /sys/bus/pci/devices/$fn/driver/unbind" 2>/dev/null || true
        fi
        echo "" > "/sys/bus/pci/devices/$fn/driver_override" 2>/dev/null || true
    done
    modprobe -r vfio_pci vfio_iommu_type1 vfio 2>/dev/null || true
    modprobe nvidia_drm 2>/dev/null || modprobe nvidia 2>/dev/null || true
    modprobe snd_hda_intel 2>/dev/null || true
    sleep 2
    echo "   card driver now: $(drv_of "$BDF")"
    systemctl start gdm 2>/dev/null || true
    echo "=== DONE $(date -u +%FT%TZ) ==="
    touch "$DIR/gsp-boot-DONE"
}
trap restore_gui EXIT

echo "=== run-gsp-boot-detached $(date -u +%FT%TZ) ==="
echo "kernel: $(uname -r)"
[ "$(id -u)" -eq 0 ] || { echo "ERR: нужен root"; exit 1; }
[ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ] || { echo "ERR: IOMMU не активен"; exit 1; }
[ -f "$HARNESS_SRC" ] || { echo "ERR: нет $HARNESS_SRC"; exit 1; }

echo "-- грейс 6с перед гашением экрана --"
sleep 6

echo "-- stop gdm + gpu-процессы --"
systemctl stop gdm 2>/dev/null || true
systemctl stop nvidia-persistenced 2>/dev/null || true
sleep 2
fuser -k /dev/nvidia* /dev/dri/* 2>/dev/null || true
pkill -9 -x gnome-shell 2>/dev/null || true
pkill -9 -x Xwayland 2>/dev/null || true
sleep 2

echo "-- unbind fbcon (vtcon) --"
for v in /sys/class/vtconsole/vtcon*/bind; do
    [ -e "$v" ] || continue
    if grep -qi "frame buffer" "$(dirname "$v")/name" 2>/dev/null; then
        echo 0 > "$v" 2>/dev/null || true
    fi
done
sleep 1

echo "-- unload nvidia --"
for m in nvidia_drm nvidia_modeset nvidia_uvm nvidia; do timeout 15 modprobe -r "$m" 2>/dev/null || true; done
sleep 1
cur="$(drv_of "$BDF")"
echo "   driver of $BDF after unload: ${cur:-(none)}"
if [ "$cur" = "nvidia" ]; then
    echo "ABORT: nvidia не освободила карту — откат."
    exit 2
fi

modprobe vfio-pci 2>/dev/null || true
for fn in "$BDF" "$AUD"; do
    [ -e "/sys/bus/pci/devices/$fn" ] || continue
    if [ -e "/sys/bus/pci/devices/$fn/driver" ]; then
        timeout 15 bash -c "echo '$fn' > /sys/bus/pci/devices/$fn/driver/unbind" 2>/dev/null || true
    fi
    echo vfio-pci > "/sys/bus/pci/devices/$fn/driver_override" 2>/dev/null || true
    echo "$fn" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
    echo "   $fn -> $(drv_of "$fn")"
done

cp -f "$HARNESS_SRC" "$HARNESS"; chmod +x "$HARNESS"
echo "=== HARNESS ==="
timeout 60 "$HARNESS" "$BDF"
echo "=== harness rc=$? ==="
exit 0
