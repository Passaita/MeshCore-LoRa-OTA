# LoRa-OTA - testing

## Host tests (no hardware)
Run from the repository root. They need `g++` (with AddressSanitizer / UBSan) and `python3`. The shell tests extract the **real**
functions from `src/helpers/fw_ota/FwOtaTransfer.cpp` on every run (not copies), so they test the code that is built.

| Test | Command | What it covers | Checks (rc6) |
|---|---|---|---|
| Routing | `bash test/fw_ota_route_test.sh` | route table, route pairs, heals, collisions, typed routes, peers table, packet padding | 324 |
| Duty and pacing | `bash test/fw_ota_duty_test.sh` | duty parsing and agreement, pacing, simulated pulls, back-off, scan/answer budget, discovery answers, pause, rc6 yield | 698 |
| Crypto | `bash test/fw_ota_crypto_test.sh` | every LoRa-OTA payload through MeshCore's real channel encryption, 656 single-bit corruptions rejected, PBKDF2 vectors (RFC 6070 inputs), session keys, other circle hidden, AES-CTR; a re-introduced padding bug (mutant) must be caught | 480 |
| BLE slot fill | `bash test/fw_ota_slotfill_test.sh <release .lota>` | `fw pull-ble`: good and bad uploads into the slot | 15 |
| Chunk map | `g++ -std=c++11 -I src/helpers/fw_ota test/fw_ota_chunkmap_test.cpp && ./a.out` | the received-chunk bitmap | |
| Bootloader images | see the header of `test/fw_ota_blimage_test.cpp` | bootloader images served over LoRa (needs real bootloader builds) | |

Bootloader repository (`LoRa-OTA-bootloader/tools/fw_ota_hosttest`): `make test` runs the **unmodified** `fw_ota_apply.c` on a
simulated nRF52840 flash with a power cut injected at every flash operation (5153 checks); `make mutants` re-introduces 23 known
bugs, each of which must be caught. `tools/check_vectors.py` checks a bootloader build's interrupt vectors before it is flashed.

## Building
* App: `pio run -e RAK_3401_repeater_poc` (environment in `platformio.local.ini`). With `SOURCE_DATE_EPOCH` set, two builds are
  byte-identical; the published rc6 binary was rebuilt from this repository byte for byte.
* Bootloader: `make BOARD=wiscore_rak3401 FW_OTA_BL_REV=7` with the ARM GCC toolchain on `PATH` (see that repository).

## Hardware results (summary)
On RAK3401 repeaters with the RAK13302 1 W booster, 910.525 MHz / 62.5 kHz / SF7 / CR5:

| Test | Result |
|---|---|
| Direct pull, old pacing (now `fw duty 1.0`) | 1615 chunks in 1 h 41 min, 0 retries, 0 MAC failures, installed itself |
| Direct pull at `fw duty 0.3` (rc6) | ~3-4 chunks/min, 0 MAC failures (6-9 h per image) |
| Pull through one relay at `fw duty 0.3` (rc6) | ~1.8 chunks/min (~14 h), serving node transmitting 4.4 % of the time (rc5: 9.7 %), LoRa-OTA packets heard -69 % |
| Pull of the image a node already runs | slot filled and served, no restart |
| Pulls through 2 relays, one-way links | completed after the route-pair and heal fixes; a weak receive antenna on one node was the main cause of stalls |
| Bootloader over LoRa (`fw blupdate`) | app kept, settings kept |
| Onboarding from stock OTAFIX, offboarding to stock bootloader + stock app, and back | identity and settings kept |
| `fw pull-ble` | good uploads served; cut uploads, wrong zips and timeouts handled |
| Power cut during apply | covered by the host test; bench power pulls during apply recovered |
| RTC (RAK12002) | time kept through reboot and power cut after one `clock sync` |

A 48-hour soak of rc6 on several nodes (including an outdoor solar node) is in progress.
