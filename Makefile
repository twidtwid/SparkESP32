# SparkESP32 developer targets. Firmware targets need ESP-IDF v5.5.x exported
# (. ~/esp/esp-idf/export.sh); `make check` needs only python3 (+ ruff/shellcheck
# if installed).
VERSION := $(shell head -n1 VERSION)
DIST    := dist
PORT    ?=
PY      ?= python3

# Every directory under firmware/boards/ is a board (see firmware/boards/README.md).
# The C6 AMOLED is the default and the reference board.
BOARDS  := $(shell find firmware/boards -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | sort)
BOARD   ?= waveshare-esp32c6-amoled-2.16
BUILD    = firmware/build/$(BOARD)
# The board decides the chip. Passing it keeps idf.py from guessing from a stray sdkconfig.
# Build one board at a time: every board shares firmware/managed_components, which the
# component manager re-syncs to that board's dependencies.lock.
TARGET   = $(shell sed -n 's/^CONFIG_IDF_TARGET="\{0,1\}\([a-z0-9]*\)"\{0,1\}$$/\1/p' \
              firmware/boards/$(BOARD)/sdkconfig.defaults 2>/dev/null)
IDF      = cd firmware && IDF_TARGET=$(TARGET) idf.py -B build/$(BOARD) -DBOARD=$(BOARD) \
              -DSDKCONFIG=build/$(BOARD)/sdkconfig

.PHONY: boards build flash monitor bundle dist check clean

boards:
	@printf '%s\n' $(BOARDS)

build:
	@[ -n "$(TARGET)" ] || { echo "unknown BOARD '$(BOARD)'; one of: $(BOARDS)"; exit 1; }
	$(IDF) build

# split images only: keeps the board's saved settings (see install.sh)
flash: build
	$(IDF) $(if $(PORT),-p $(PORT)) flash

monitor:
	$(IDF) $(if $(PORT),-p $(PORT)) monitor

# Release bundle for one board, what install.sh and the GitHub release consume:
#   dist/sparkesp32-<board>-firmware-X/       split images + flash_args + SHA256SUMS
#   dist/sparkesp32-<board>-firmware-X.tar.gz that directory, tarred (release asset)
#   dist/sparkesp32-<board>-X-merged.bin      one image at 0x0 for web flashers; fresh
#                                             boards only, it erases saved settings
PKG = sparkesp32-$(BOARD)-firmware-$(VERSION)
SHA := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo shasum -a 256)

bundle: build
	rm -rf $(DIST)/$(PKG) && mkdir -p $(DIST)/$(PKG)
	cp $(BUILD)/bootloader/bootloader.bin $(BUILD)/partition_table/partition-table.bin \
	   $(BUILD)/sparkesp32.bin $(DIST)/$(PKG)/
	sed 's|[^ ]*/||' $(BUILD)/flash_args > $(DIST)/$(PKG)/flash_args   # paths: the bundle's own files
	cd $(DIST)/$(PKG) && $(SHA) bootloader.bin partition-table.bin sparkesp32.bin flash_args > SHA256SUMS
	cd $(DIST)/$(PKG) && python -m esptool --chip $(TARGET) merge_bin \
	   -o ../sparkesp32-$(BOARD)-$(VERSION)-merged.bin @flash_args
	tar -C $(DIST) -czf $(DIST)/$(PKG).tar.gz $(PKG)

# Every board's bundle, plus dist/SHA256SUMS over the tarballs and merged images.
dist:
	rm -rf $(DIST) && mkdir -p $(DIST)
	@for b in $(BOARDS); do $(MAKE) --no-print-directory bundle BOARD=$$b || exit 1; done
	cd $(DIST) && $(SHA) *.tar.gz *-merged.bin > SHA256SUMS
	@ls -l $(DIST)

check:
	$(PY) -m py_compile agent/sparkesp32_agent.py tools/sparklink.py tools/provision.py tools/devctl.py \
	   tools/check_boards.py tools/new_board.py
	$(PY) tests/test_agent.py
	$(PY) tests/test_boards.py
	$(PY) tools/check_boards.py
	@mkdir -p tests/.build
	$(CC) -std=c11 -Wall -Wextra -Werror -Itests/stubs -Ifirmware/main \
	   tests/test_parser.c firmware/main/spark_data.c -o tests/.build/test_parser
	tests/.build/test_parser
	$(CC) -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
	   tests/test_states.c firmware/main/link.c firmware/main/status.c -o tests/.build/test_states
	tests/.build/test_states
	@grep -q '^__version__ = "$(VERSION)"$$' agent/sparkesp32_agent.py \
	   || { echo "agent __version__ != VERSION ($(VERSION))"; exit 1; }
	@if command -v ruff >/dev/null; then ruff check agent tools tests; \
	 elif command -v uvx >/dev/null; then uvx ruff check agent tools tests; \
	 else echo "(ruff not installed: skipped)"; fi
	@if command -v shellcheck >/dev/null; then shellcheck install.sh; else echo "(shellcheck not installed: skipped)"; fi
	@echo "check: ok"

clean:
	rm -rf firmware/build firmware/managed_components $(DIST)
