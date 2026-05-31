# Can be overridden via command line: make flash PORT=/dev/ttyUSB1
PORT ?= /dev/ttyACM0

ifeq ($(IDF_PATH),)
    IDF_PATH := $(HOME)/.espressif/v6.0.1/esp-idf
endif

VSCODE_CLANG_FORMAT=$(ls -d $HOME/.vscode/extensions/ms-vscode.cpptools-*-linux-x64/LLVM/bin/clang-format | tail -n 1)

# Automatically wrap all make recipes inside the ESP-IDF shell environment
SHELL := /bin/bash
.SHELLFLAGS := -c 'source $(IDF_PATH)/export.sh && eval "$$0"'

DEVICE_ARG :=
ifneq ($(DEVICE),)
    DEVICE_ARG := -DDEVICE=$(DEVICE)
endif

.PHONY: all build flash monitor clean format lint test-host test-browser deps test-backend-build test-backend-flash test-backend-run generate-keys check-config-sync

all: build

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

build: check-config-sync
	idf.py $(DEVICE_ARG) build

flash: check-config-sync
	idf.py $(DEVICE_ARG) -p $(PORT) flash

monitor: check-config-sync
	idf.py $(DEVICE_ARG) -p $(PORT) monitor

clean:
	idf.py fullclean

menuconfig:
	idf.py $(DEVICE_ARG) menuconfig

save-config:
	idf.py $(DEVICE_ARG) save-defconfig

format:
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-format -i

build_clang/compile_commands.json:
	idf.py -B build_clang -DIDF_TOOLCHAIN=clang reconfigure

lint: build_clang/compile_commands.json
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-tidy -p build_clang/

test-backend-build:
	cd tests/integration/test_backend && idf.py set-target esp32p4 && idf.py build

test-backend-flash: test-backend-build
	cd tests/integration/test_backend && idf.py -p $(PORT) flash

test-backend-run:
	cd tests/integration/test_backend && uv run pytest pytest_backend_integration.py --target esp32p4 --port $(PORT)