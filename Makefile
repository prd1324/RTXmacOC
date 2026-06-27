# RTXmacOC — Makefile
#
# Только macOS. Зависит лишь от системных фреймворков (IOKit, CoreFoundation).
#
#   make probe   — собрать утилиту разведки pcie_probe
#   make run     — собрать и запустить
#   make dump    — запустить и сохранить лог в docs/hw-dumps/ с датой
#   make clean   — удалить артефакты

CC      ?= clang
CFLAGS  ?= -Wall -Wextra -O2
FRAMEWORKS = -framework IOKit -framework CoreFoundation

PROBE_SRC = pcie_probe.c
PROBE_BIN = pcie_probe

DUMP_DIR  = docs/hw-dumps
DATE     := $(shell date +%Y%m%d)

.PHONY: probe run dump clean mmio-linux

probe: $(PROBE_BIN)

$(PROBE_BIN): $(PROBE_SRC) ada_regs.h
	$(CC) $(CFLAGS) $(PROBE_SRC) $(FRAMEWORKS) -o $(PROBE_BIN)

run: probe
	./$(PROBE_BIN)

dump: probe
	@mkdir -p $(DUMP_DIR)
	./$(PROBE_BIN) | tee "$(DUMP_DIR)/$(DATE)-rtx4070s-pcie_probe.log"

clean:
	rm -f $(PROBE_BIN) tools/nv_mmio_linux

# Проверка PMC_BOOT_0 на реальной карте из Linux live-USB (без macOS, без Windows).
# Собирать и запускать ИМЕННО на Linux: make mmio-linux && sudo ./tools/nv_mmio_linux
mmio-linux:
	cc -Wall -Wextra -O2 tools/nv_mmio_linux.c -o tools/nv_mmio_linux
