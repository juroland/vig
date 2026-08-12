# Can be overridden via command line: make flash PORT=/dev/ttyUSB1
PORT ?= /dev/ttyACM0

export IDF_CCACHE_ENABLE := 1

ifeq ($(IDF_PATH),)
    IDF_PATH := $(HOME)/.espressif/v6.0.1/esp-idf
endif

VSCODE_CLANG_FORMAT=$(ls -d $HOME/.vscode/extensions/ms-vscode.cpptools-*-linux-x64/LLVM/bin/clang-format | tail -n 1)

# Automatically wrap all make recipes inside the ESP-IDF shell environment
SHELL := /bin/bash
.SHELLFLAGS := -c 'source $(IDF_PATH)/export.sh >/dev/null 2>&1 && eval "$$0"'

DEVICE_ARG :=
ifneq ($(DEVICE),)
    DEVICE_ARG := -DDEVICE=$(DEVICE)
endif

.PHONY: all build flash monitor clean format lint test-host test-browser deps test-backend-build test-backend-flash test-backend-run generate-keys check-config-sync bump-patch bump-minor bump-major bump-dev bump-release ota-export version generate-factory-blob production-flash

all: build

# ─── Version Management ─────────────────────────────────────────────────────
# The canonical version lives in version.txt and is injected into the firmware
# at build time via ESP-IDF's PROJECT_VER → VIGO_VERSION compile definition.
#
# Use semver format: MAJOR.MINOR.PATCH (e.g. 1.2.3)
# Pre-release suffixes like -dev.N are stripped on bump.

VERSION := $(shell cat version.txt | tr -d '[:space:]')

version:
	@echo "$(VERSION)"

bump-patch:
	@OLD=$$(cat version.txt | tr -d '[:space:]'); \
	BASE=$$(echo "$$OLD" | cut -d'-' -f1); \
	MAJOR=$$(echo "$$BASE" | cut -d'.' -f1); \
	MINOR=$$(echo "$$BASE" | cut -d'.' -f2); \
	PATCH=$$(echo "$$BASE" | cut -d'.' -f3); \
	PATCH=$$((PATCH + 1)); \
	NEW="$$MAJOR.$$MINOR.$$PATCH"; \
	echo "$$NEW" > version.txt; \
	echo "Bumped version: $$OLD → $$NEW"

bump-minor:
	@OLD=$$(cat version.txt | tr -d '[:space:]'); \
	BASE=$$(echo "$$OLD" | cut -d'-' -f1); \
	MAJOR=$$(echo "$$BASE" | cut -d'.' -f1); \
	MINOR=$$(echo "$$BASE" | cut -d'.' -f2); \
	MINOR=$$((MINOR + 1)); \
	NEW="$$MAJOR.$$MINOR.0"; \
	echo "$$NEW" > version.txt; \
	echo "Bumped version: $$OLD → $$NEW"

bump-major:
	@OLD=$$(cat version.txt | tr -d '[:space:]'); \
	BASE=$$(echo "$$OLD" | cut -d'-' -f1); \
	MAJOR=$$(echo "$$BASE" | cut -d'.' -f1); \
	MAJOR=$$((MAJOR + 1)); \
	NEW="$$MAJOR.0.0"; \
	echo "$$NEW" > version.txt; \
	echo "Bumped version: $$OLD → $$NEW"

bump-dev:
	@OLD=$$(cat version.txt | tr -d '[:space:]'); \
	if [[ "$$OLD" =~ (.*)-dev\.([0-9]+) ]]; then \
		BASE="$${BASH_REMATCH[1]}"; \
		DEV="$${BASH_REMATCH[2]}"; \
		DEV=$$((DEV + 1)); \
		NEW="$$BASE-dev.$$DEV"; \
	else \
		NEW="$$OLD-dev.1"; \
	fi; \
	echo "$$NEW" > version.txt; \
	echo "Bumped dev version: $$OLD → $$NEW"

bump-release:
	@OLD=$$(cat version.txt | tr -d '[:space:]'); \
	BASE=$$(echo "$$OLD" | cut -d'-' -f1); \
	echo "$$BASE" > version.txt; \
	echo "Promoted to release: $$OLD → $$BASE"

# ─── OTA Firmware Export ─────────────────────────────────────────────────────
# Build the firmware and copy the binary to release/ for OTA upload to VisionLink.

ota-export: build
	@mkdir -p release
	@cp build/vigo.bin release/vigo-$(VERSION).bin
	@echo ""
	@echo "═══════════════════════════════════════════════════════"
	@echo "  OTA firmware exported: release/vigo-$(VERSION).bin"
	@echo "  Version:  $(VERSION)"
	@echo "  Size:     $$(du -h release/vigo-$(VERSION).bin | cut -f1)"
	@echo "  SHA-256:  $$(sha256sum release/vigo-$(VERSION).bin | cut -d' ' -f1)"
	@echo "═══════════════════════════════════════════════════════"

# ─── Device Key Generation ───────────────────────────────────────────────────

generate-keys:
ifeq ($(DEVICE),)
	@echo "Error: Please specify a DEVICE, e.g., 'make generate-keys DEVICE=jr'"
	@exit 1
else
	python3 tools/generate_device_keys.py configs/$(DEVICE).defaults
endif

check-config-sync:
ifneq ($(DEVICE),)
	@if [ -f configs/$(DEVICE).defaults ] && [ -f configs/sdkconfig.$(DEVICE) ] && [ configs/$(DEVICE).defaults -nt configs/sdkconfig.$(DEVICE) ]; then \
		echo "Detected changes in configs/$(DEVICE).defaults. Removing stale configs/sdkconfig.$(DEVICE)..."; \
		rm -f configs/sdkconfig.$(DEVICE); \
	fi
	@if [ -f sdkconfig.defaults ] && [ -f configs/sdkconfig.$(DEVICE) ] && [ sdkconfig.defaults -nt configs/sdkconfig.$(DEVICE) ]; then \
		echo "Detected changes in sdkconfig.defaults. Removing stale configs/sdkconfig.$(DEVICE)..."; \
		rm -f configs/sdkconfig.$(DEVICE); \
	fi
endif

# ─── Build & Flash ───────────────────────────────────────────────────────────

build: check-config-sync
	idf.py $(DEVICE_ARG) build

flash: check-config-sync
	idf.py $(DEVICE_ARG) -p $(PORT) flash

HARDWARE_ID ?= VIGO-DEV-001
DEVICE_TOKEN ?= setup_token_value_placeholder

generate-factory-blob:
	@mkdir -p build
ifeq ($(DEVICE),)
	python3 tools/generate_factory_data.py \
		--bin-out build/factory_mfg.bin \
		--csv-out configs/factory_mfg_template.csv \
		--hardware-id "$(HARDWARE_ID)" \
		--device-token "$(DEVICE_TOKEN)" \
		--size "0x4000" \
		--idf-path "$(IDF_PATH)"
else
	python3 tools/generate_factory_data.py \
		--bin-out build/factory_mfg.bin \
		--csv-out configs/factory_mfg_template.csv \
		--defaults-file configs/$(DEVICE).defaults \
		--size "0x4000" \
		--idf-path "$(IDF_PATH)"
endif

production-flash: build generate-factory-blob
	idf.py $(DEVICE_ARG) -p $(PORT) flash
	esptool.py --chip esp32p4 --port $(PORT) --baud 921600 --before default_reset --after hard_reset write_flash 0x12000 build/factory_mfg.bin

monitor: check-config-sync
	idf.py $(DEVICE_ARG) -p $(PORT) monitor

clean:
	idf.py fullclean

menuconfig:
	idf.py $(DEVICE_ARG) menuconfig

save-config:
	idf.py $(DEVICE_ARG) save-defconfig

# ─── Code Quality ────────────────────────────────────────────────────────────

format:
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-format -i

build_clang/compile_commands.json: check-config-sync
	rm -rf build_clang
	idf.py $(DEVICE_ARG) -B build_clang -DIDF_TOOLCHAIN=clang reconfigure

lint: build_clang/compile_commands.json
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-tidy -p build_clang/

# ─── Tests ───────────────────────────────────────────────────────────────────

test-backend-build:
	cd tests/integration/test_backend && idf.py set-target esp32p4 && idf.py build

test-backend-flash: test-backend-build
	cd tests/integration/test_backend && idf.py -p $(PORT) flash

test-backend-run:
	cd tests/integration/test_backend && uv run pytest pytest_backend_integration.py --target esp32p4 --port $(PORT)

test-unit-build:
	cd tests/unit && idf.py set-target esp32p4 && idf.py build

test-unit-flash:
	cd tests/unit && idf.py -p $(PORT) flash

test-unit-monitor:
	cd tests/unit && idf.py -p $(PORT) monitor