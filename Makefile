# Can be overridden via command line: make flash PORT=/dev/ttyUSB1
PORT ?= /dev/ttyUSB0

.PHONY: all build flash monitor clean format lint test-host test-browser

all: build

build:
	idf.py build

flash:
	idf.py -p $(PORT) flash

monitor:
	idf.py -p $(PORT) monitor

clean:
	idf.py fullclean

# Tools

menuconfig:
	idf.py menuconfig

save-config:
	idf.py save-defconfig

format:
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-format -i

lint:
	# Run clang-tidy using the generated compile_commands.json
	find main components -iname '*.hpp' -o -iname '*.cpp' | xargs clang-tidy -p build/
