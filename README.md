# VIG : Video IoT Guardian

Edge vision and networking firmware targeting the Waveshare ESP32-P4. Built with modern C++23, strict static analysis, and a hardware abstraction layer (HAL) over ESP-IDF.

## Prerequisites

* **ESP-IDF:** v6.0.1 (or newer) with the `riscv32-esp-elf` toolchain.
* **Host Tools:** `make`, `clang-format`, `clang-tidy`, `Node.js` (for frontend UI testing).

## Quick Start

The build system is wrapped in a top-level `Makefile` to simplify ESP-IDF commands and automate static analysis.

```bash
# 1. Load the ESP-IDF environment
. $IDF_PATH/export.sh

# 2. Build the firmware (automatically generates compile_commands.json)
make build

# 3. Flash to the device and open the serial monitor
make flash monitor PORT=/dev/ttyUSB0
