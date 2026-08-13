<p align="center">
  <img src="logo.png" alt="VIGO Logo" width="450">
</p>

# VIGO : Video Intelligence Guard Outpost

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
```

---

## Decoupled Firmware OTA & Manufacturing Provisioning Framework

To optimize continuous deployment, VIG firmware binaries are completely identical and generic across all hardware units. Unique identities, authentication tokens, and stream encryption keys reside in an isolated, immutable factory data partition (`fct_nvs`) that persists across application updates.

For in-depth explanations, protocol flow charts, and cryptographic analyses, consult the **[VIG Technical & Cryptographic Protocol Handbook](file:///home/juroland/Repositories/juroland/vig/HANDBOOK.md)**.

### Provisioning Parameters (Factory Partition)

The isolated read-only `fct_nvs` partition stores the following device-specific parameters:

* `hardware_id` (string): The unique hardware identifier.
* `device_token` (string): The unique long-lived static bearer token for backend API authentication.
* `dtls_cert` (string): Self-signed X.509 certificate in PEM format for DTLS-SRTP.
* `dtls_key` (binary): Secure ECDSA private key (secp256r1/prime256v1) in binary DER format.

---

### Manufacturing Automation & Production Flashing

The manufacturing workflow utilizes device defaults configuration files (e.g. `configs/jr.defaults`) to extract parameters and keys. This keeps the compiled app firmware entirely generic.

#### 1. Generate DTLS Key & Certificate

Generates a new secure `secp256r1` private key and self-signed certificate, formatting them and saving them inside the device defaults configuration file:

```bash
make generate-keys DEVICE=jr
```

#### 2. Generate Factory Blob

Pulls `CONFIG_VIGO_HARDWARE_ID`, `CONFIG_VIGO_DEVICE_TOKEN`, and the DTLS certificate/key from `configs/jr.defaults` to compile the read-only NVS partition binary (`build/factory_mfg.bin`):

```bash
make generate-factory-blob DEVICE=jr
```

*(Alternatively, you can generate a sandbox factory blob manually using command-line arguments: `make generate-factory-blob HARDWARE_ID=VIGO-DEV-001 DEVICE_TOKEN=my_secure_token`)*

#### 3. Production Flash

Compiles the generic application firmware, generates the factory blob for the specified device defaults config, flashes all system code partitions (bootloader, partition table, active app partition), and writes the factory blob to offset `0x12000` in a single execution step:

```bash
make production-flash DEVICE=jr PORT=/dev/ttyUSB0
```

---

## Firmware Versioning & OTA Releases

The firmware version is the single source of truth for OTA updates. It lives in `version.txt` and follows [SemVer](https://semver.org/) (`MAJOR.MINOR.PATCH`).

### Bumping the Version

Use the Makefile targets to increment the version:

```bash
# Show current version
make version

# Patch bump: 1.2.3 → 1.2.4 (bug fixes, minor changes)
make bump-patch

# Minor bump: 1.2.3 → 1.3.0 (new features, backward-compatible)
make bump-minor

# Major bump: 1.2.3 → 2.0.0 (breaking changes)
make bump-major

# Dev suffix bump: 1.2.3 → 1.2.3-dev.1 (or 1.2.3-dev.1 → 1.2.3-dev.2)
make bump-dev

# Promotion to release: 1.2.3-dev.2 → 1.2.3 (strips suffix)
make bump-release
```

### Building an OTA Release

```bash
make ota-export
# Output: release/vigo-x.y.z.bin
```

### Automated Validation Lifecycle & Rollback Protection

To prevent bricking during OTA deployments, VIG implements an automated validation lifecycle:

1. **Boot Verification**: On boot, the system invokes `run_system_self_test()`.
2. **Self-Test Criteria**:
   * **Camera Check**: Captures a test frame to ensure image sensor is operational.
   * **Network Check**: Waits up to 15 seconds for a Wi-Fi or Ethernet connection.
   * **Backend Check**: Sends a diagnostic heartbeat request to verify API connection.
3. **Commit or Rollback**:
   * **Success**: If all checks pass, the firmware calls `esp_ota_mark_app_valid_cancel_rollback()` to commit the new firmware partition.
   * **Failure/Crash**: If the self-test fails or the system crashes before completion, the bootloader automatically rolls back to the previous working slot and reboots.

---

## Partition Layout

The device uses a dual-OTA layout mapped to the 32 MB flash with an isolated factory provisioning partition:

| Partition | Type | SubType | Offset | Size | Purpose |
| ----------- | ------ | --------- | -------- | ------ | --------- |
| `nvs` | data | nvs | `0x9000` | 24 KB | Standard writable NVS storage (dynamic state) |
| `phy_init` | data | phy | `0xf000` | 4 KB | PHY initialization parameters |
| `otadata` | data | ota | `0x10000` | 8 KB | Tracks which OTA slot is active |
| `fct_nvs` | data | nvs | `0x12000` | 16 KB | Factory provisioning storage (read-only) |
| `ota_0` | app | ota_0 | `0x20000` | 8 MB | OTA firmware partition slot 0 |
| `ota_1` | app | ota_1 | - | 8 MB | OTA firmware partition slot 1 |
| `storage` | data | spiffs | - | 14 MB | Local media storage (SPIFFS) |

---

## Hardware Platform Notes (SD Card, Wi-Fi Co-Processor, USB)

This section documents the non-obvious platform constraints learned the hard way.
Read this before touching `vigo_storage`, `vigo_net`, or the ESP-Hosted/TinyUSB configs.

### Shared SDMMC host controller (two slots, one peripheral)

The ESP32-P4 has a **single** SDMMC SDIO host peripheral with **two slots**, shared
between two different subsystems:

* **Slot 0** — the SD card (4-bit, CLK=43, CMD=44, D0..D3=39..42, VDD via on-chip **LDO channel 4**).
* **Slot 1** — the Wi-Fi co-processor (ESP32-C6 via ESP-Hosted, 4-bit, 40 MHz, CLK=18, CMD=19, D0..D3=14..17, reset = **GPIO 54**).

Consequences:

* Whoever initializes a card on either slot must run the full sequence
  `malloc(sdmmc_card_t)` → `sdmmc_host_init()` → `sdmmc_host_init_slot(slot, …)` →
  `sdmmc_card_init(host, card)`. Calling `sdmmc_card_init()` on an unallocated card
  pointer crashes with `Guru Meditation (Store access fault)` (`memset` into address 0).
* `sdmmc_host_init()` is a shared singleton — the second caller gets the existing
  handle (`SDMMC host controller already created`, harmless).
* The SD card must be **mounted after Wi-Fi is up** in normal mode (see `Device::start()`),
  and in USB mass storage mode ESP-Hosted must never touch the bus (see below).

### USB Mass Storage mode (boot button at startup)

Holding the boot button (**GPIO 52**) at boot enters storage mode: the SD card is
exposed over the OTG **High-Speed** port (`TINYUSB_PORT_HIGH_SPEED_0`, dedicated USB
pads — no GPIO mapping) via TinyUSB MSC.

### ESP-Hosted bring-up: poll *on demand*, not at boot

`esp_hosted` ≥ 3.0 defaults `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=y`:
a C-constructor task starts probing the co-processor over SDIO **before `app_main()`**,
in every mode. That was the source of the `sdmmc_init_ocr: send_op_cond (1) returned
0x107` / `eh_host_port_sdio: sdmmc_card_init failed` error storms:

* In **storage mode** the probing task fights the SD card/USB-MSC over the shared SDMMC host.
* In **normal mode** the bring-up is one-shot: `BRINGUP_FAILED` latches, and all later
  Wi-Fi init returns `-1` for the whole boot.

Fix in use: `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n`, and
`NetworkManager::init_wifi()` drives the link on demand
(`esp_hosted_init()` + `esp_hosted_connect_to_slave()`, both idempotent).

### Co-processor reset polarity (GPIO 54)

`CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y` is board truth here: GPIO 54 is wired to
the C6 `EN` pin, and ESP modules' `EN` lines are **active-low** (LOW = reset, HIGH = run).
The 2.x → 3.0.6 bump flipped shipped configs to `ACTIVE_HIGH`, which makes the port's
reset sequence end at *inactive* == LOW on GPIO 54 == **co-processor held in permanent
reset** → it never answers `send_op_cond` (the exact `0x107` timeouts above).
