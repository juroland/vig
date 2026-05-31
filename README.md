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

## Device Provisioning & Configuration

VIG requires specific environment configurations and cryptographic credentials to publish secure live video via WebRTC (WHIP). 

For in-depth educational explanations, protocol flow charts, and cryptographic analyses (including why UDP/DTLS dictate this design, public key structures, and trust-anchors), consult the **[VIG Technical & Cryptographic Protocol Handbook](file:///home/juroland/Repositories/juroland/vig/HANDBOOK.md)**.

### Configuration Parameters Overview

1. **`CONFIG_VIGO_DEVICE_TOKEN` (Device Authentication)**:
   The unique long-lived static bearer token configured on the device. It authenticates control plane HTTPS signaling/heartbeats (e.g. `Bearer <TOKEN>`) to dynamically obtain single-use session tokens from the backend.
2. **`CONFIG_VIGO_DTLS_CERT_PEM` & `CONFIG_VIGO_DTLS_KEY_PEM` (Stream Encryption)**:
   The device-specific self-signed certificate and ECDSA private key. These secure the P2P UDP video stream via **DTLS-SRTP** on-the-fly, utilizing SDP fingerprint matching without backend database storage.

---

### Generating Unique Device Keys

We provide an automated helper to generate a unique cryptographically secure `secp256r1` (prime256v1) elliptic curve private key and self-signed certificate, format them correctly, and inject them into a specific device defaults profile:

```bash
# Generate and inject unique keys for a specific device profile (e.g. configs/jr.defaults)
make generate-keys DEVICE=jr

# Then build the firmware for that specific device
make build DEVICE=jr
```

This updates the respective `<device_name>.defaults` file with the custom `CONFIG_VIGO_DTLS_CERT_PEM` and `CONFIG_VIGO_DTLS_KEY_PEM` configuration options, completely overriding the insecure shared fallbacks.
