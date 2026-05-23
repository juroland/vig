# Can be overridden via command line: make flash PORT=/dev/ttyUSB1
PORT ?= /dev/ttyACM1

ifeq ($(IDF_PATH),)
    IDF_PATH := $(HOME)/.espressif/v6.0.1/esp-idf
endif

VSCODE_CLANG_FORMAT=$(ls -d $HOME/.vscode/extensions/ms-vscode.cpptools-*-linux-x64/LLVM/bin/clang-format | tail -n 1)

# Automatically wrap all make recipes inside the ESP-IDF shell environment
SHELL := /bin/bash
.SHELLFLAGS := -c 'source $(IDF_PATH)/export.sh && eval "$$0"'

.PHONY: all build flash monitor clean format lint test-host test-browser deps

all: build

build:
	idf.py build

flash:
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor

clean:
	idf.py fullclean

menuconfig:
	idf.py menuconfig

save-config:
	idf.py save-defconfig

format:
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-format -i

lint:
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-tidy -p build/