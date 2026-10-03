# Porting LoRa firmware-pull to another MeshCore tree

The feature is deliberately small at the seams: one self-contained directory, two linker scripts, and about 300 lines of hooks in six
shared files. It has been carried from the IoTThinks/MeshCore `PowerSaving-v17` base (`ea8f182a`) onto that branch's current tip
(`d9c42964`, 27 commits newer: RXPSv2, deep sleep, RAK3401 BSEC/WB_IO2 fixes) with **no conflicts**, and the result builds
(363,040 B, 79 % of the 448 KiB app region, firmware tag present).

## Automatic
```
python3 tools/port_fwpull.py /path/to/target-checkout --build RAK_3401_repeater_poc
```
It copies the self-contained files, applies `tools/fwpull_hooks.patch` with `git apply --3way`, installs the reference PlatformIO
environments (`tools/fwpull_env.ini` -> `platformio.local.ini`, only if the target has none) and checks the build: tag at image offset
0x200 and size within the app region. Any hook that no longer applies is reported instead of guessed.

## What is where
| Part | Files | Notes |
|---|---|---|
| The feature | `src/helpers/fw_ota/*` (`FwOtaTransfer`, `FwOtaSlots`, `FwOtaCli.h`, `ChunkMap.h`, `FwTag.cpp`) | no upstream file is edited to add them |
| Flash layout + firmware tag | `boards/nrf52840_s140_v6_fwota.ld`, `boards/nrf52_common_fwota.ld` | app 0x26000-0x96000, tag right after the vector table. **Must match the bootloader** (`fw_ota.h`; `check_contract.py` in the bootloader repo verifies it) |
| Hooks in shared code | `examples/simple_repeater/MyMesh.{h,cpp}` (5 places: include, `begin`, `handleCommand`, `onRawDataRecv`, `loop`), `main.cpp` (14 lines: staging + no password echo) | all under `#ifdef FW_OTA` |
| Console clock fix | `CommonCLI.cpp` (+`start ota` comment) | a local console may set the clock backwards (meshcore-dev/MeshCore#1332) |
| `start ota` in slim builds | `NRF52Board.cpp` | writes GPREGRET directly when the SoftDevice is off |
| RAK12002 RTC (optional) | `AutoDiscoverRTCClock.cpp` | `-D RTC_RV3028_ONLY`, +2.3 KB |
| Build environments | `tools/fwpull_env.ini` | slim repeater chain -> `RAK_3401_repeater_poc`; `..._stocktag` = a stock-behaving relay that only carries the tag |

## Constraints worth knowing before porting
* **Size.** The app region is 448 KiB. A full upstream repeater with display, GPS and every sensor (~496 KB for RAK3401) does **not**
  fit; the reference environments strip GPS, I2C sensors, display and the in-app BLE OTA stack (`start ota` still works through the bootloader).
* **Do not build with `NRF52_POWER_MANAGEMENT`** if you rely on the GPREGRET2 marker used by the bench power tests.
* **Bootloader.** Nodes need the fw-pull bootloader; images without the firmware tag are refused by it.
* **Version string:** `FIRMWARE_VERSION` comes from the environment; changing it needs the `-U`/header trick or an edit of the env.

## Manual (if the patch does not apply)
Copy the files in the table, then re-create the hooks by hand from `tools/fwpull_hooks.patch` (each hunk is small and commented), build, and
check `python3 -c "import zipfile;d=zipfile.ZipFile('.pio/build/<env>/firmware.zip').read('firmware.bin');print(d[0x200:0x210].hex())"`
prints `4657504c0100000052414b3334303100` (`FWPL`, version 1, `RAK3401`).
