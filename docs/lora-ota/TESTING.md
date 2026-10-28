# LoRa-OTA - testing

## Host tests (no hardware)
Run from the repository root. They need `g++` (with AddressSanitizer / UBSan) and `python3`. The shell tests extract the **real**
functions from `src/helpers/fw_ota/` on every run (not copies), so they test the code that is built.

| Test | Command | What it covers |
|---|---|---|
| Routing | `bash test/fw_ota_route_test.sh` | route table, route pairs, heals, collisions, typed routes, peers table, packet padding, holder changes |
| Duty and pacing | `bash test/fw_ota_duty_test.sh` | duty parsing and agreement, fair share, pacing, simulated pulls, back-off, scan/answer budget, discovery answers, pause, yield |
| Crypto | `bash test/fw_ota_crypto_test.sh` | every LoRa-OTA payload through MeshCore's real channel encryption, single-bit corruptions rejected, PBKDF2 vectors, session keys, other circle hidden, AES-CTR; a re-introduced padding bug (mutant) must be caught |
| CLI | `bash test/fw_ota_cli_test.sh` | argument clean-up (trailing spaces, options in any order) |
| Watchdog | `bash test/fw_ota_wdt_test.sh` | watchdog arming, timeout change applied remotely, reset reasons |
| Replies | `bash test/fw_ota_reply_test.sh` and `python3 tools/check_replies.py` | every status reply fits 160 bytes and says what the node is doing |
| PA supply rail | `bash test/fw_ota_pa_rail_test.sh [firmware.elf]` | the release build never switches off the 1 W booster's supply and links no GPS probe (MeshCore #3275) |
| BLE slot fill | `bash test/fw_ota_slotfill_test.sh <release .lota>` | `fw pull-ble`: good and bad uploads into the slot |
| Chunk map | `g++ -std=c++11 -I src/helpers/fw_ota test/fw_ota_chunkmap_test.cpp && ./a.out` | the received-chunk bitmap |
| Bootloader images | see the header of `test/fw_ota_blimage_test.cpp` | bootloader images served over LoRa (needs real bootloader builds) |

## Network simulator
`bash test/fw_ota_sim_test.sh` compiles the **real** MeshCore core and the **real** LoRa-OTA sources into one PC program that runs many
virtual nodes on a simulated LoRa channel (loss, collisions, one-way links, relays, power cuts, batteries, clocks), then runs the
scenarios in `test/sim/scenarios.cpp`: swarm failover, shared stream, fleet fill and cancel, gap repair, long weak chains, time from the
swarm, text audit of every reply. `SIM_CRYPTO=<path to the rweather Crypto library>` if PlatformIO hasn't fetched it into `.pio/libdeps`
yet (run a build once). rc21: **208 checks, 0 failed.** Mutants (known bugs put back on purpose) must each make a scenario fail; 22
are caught. Gap-fill and stall changes are also compared against the previous release over 8 random seeds before they ship.

Bootloader repository (`LoRa-OTA-bootloader/tools/fw_ota_hosttest`): `make test` runs the **unmodified** `fw_ota_apply.c` on a
simulated nRF52840 flash with a power cut injected at every flash operation (5153 checks); `make mutants` re-introduces known bugs,
each of which must be caught. `tools/check_vectors.py` checks a bootloader build's interrupt vectors before it is flashed.

## Building
* App: `pio run -e RAK_3401_lora_ota` (environment in `platformio.local.ini`; `RAK_3401_lora_ota_dev` = the bench build). With
  `SOURCE_DATE_EPOCH=1792083074` this repository rebuilds the published **v1.17.28-rc21** binary byte for byte.
* Bootloader: see the LoRa-OTA-bootloader README (rev 8 rebuilds byte for byte as well).

## Hardware results (summary)
On RAK3401 repeaters with the RAK13302 1 W booster (915 MHz ISM band, 62.5 kHz / SF7 / CR5), bench boards and outdoor solar relays:

| Test | Result |
|---|---|
| Direct pull at `fw duty 1` | ~15 chunks/min, ~2 h per app image (1660 chunks), 0 MAC failures, installed itself |
| Pull through relays, overnight | completed unattended in ~5-6 h; the node installed itself |
| Releases rc18-rc21 rolled out | one Bluetooth slot fill per release, everything else over LoRa (pulls, fleet fills, `fw apply`); no site visit |
| Swarm tests (rc19) | one node updates; whole swarm stores without installing; whole swarm updated - all passed |
| rc21 soak | 24 h on all nodes, 0 watchdog resets; time from the swarm kept a clockless relay within a minute of real time |
| Pull of the image a node already runs | slot filled and served, no restart |
| One-way links | detected and healed (route pairs, flooded answers) |
| Bootloader over LoRa (`fw blupdate`) | app kept, settings kept |
| Onboarding from stock OTAFIX, offboarding to stock bootloader + stock app, and back | identity, settings and passphrase kept |
| `fw pull-ble` | good uploads served; cut uploads, wrong zips and timeouts handled |
| Power cut during apply | covered by the host test; bench power pulls during apply recovered |
| Watchdog | a frozen test build restarted itself within the timeout; a timeout change applied itself remotely |
