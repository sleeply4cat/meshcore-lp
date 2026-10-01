# Third-party components

| Component | Location | License |
|---|---|---|
| MeshCore 1.17.1 (core, helpers, simple_repeater) | `src/meshcore/`, `src/repeater/` (modified, see docs/MESHCORE_PATCHES.md) | MIT, (c) Scott Powell / rippleradios.com |
| nrfx v2.11.0 (MDK, HAL headers) | `third_party/nrfx` (git submodule) | BSD-3-Clause, (c) Nordic Semiconductor |
| TinyUSB 0.18.0 (CDC device) | `third_party/tinyusb` (git submodule) | MIT, (c) Ha Thach |
| CMSIS-Core (Cortex-M4) headers | `third_party/cmsis` | Apache-2.0, (c) Arm Limited |
| Arduino Cryptography Library (AES, SHA, Ed25519 helpers) | `third_party/crypto` | MIT, (c) Southern Storm Software |
| orlp ed25519 | `third_party/ed25519` | zlib, (c) Orson Peters |
| uf2conv.py / uf2families.json | `tools/` | MIT, (c) Microsoft |

Not included: the Nordic S140 SoftDevice (needed on the chip, and as a hex file only to build the
`repeater-sd.zip` package) - get it from the Adafruit nRF52 bootloader repository under Nordic's license.
