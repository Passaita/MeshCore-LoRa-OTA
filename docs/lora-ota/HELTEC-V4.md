# LoRa-OTA on the Heltec V4

Current for **v1.18.0-rc25 (fw-319)**. The Heltec V4 (ESP32-S3, 16 MB flash, SX1262) runs the same LoRa-OTA code as the RAK3401. This
page covers what is different and the steps once per node. Variant: `heltec_v4` (16 MB flash, 2 MB PSRAM).

## The same as on the RAK3401
* The commands: `fw peers`, `fw list`, `fw pull #1`, `fw pull-status`, `fw apply`, `fw push`, `fw serve`, `fw duty`, `fw passphrase`
  ... ([COMMANDS.md](COMMANDS.md)). Same circle passphrase, same release names and fw numbers.
* Downloads survive restarts and power cuts, neighbours listen along, `fw push` fills a whole circle.
* The node keeps its name, identity, contacts, radio settings and passphrase through every update.

## A mixed circle (RAK3401 and Heltec V4 together) is fine
Every image carries the name of its board (`RAK3401` or `HELTECV4`). A node lists, stores and installs only images for its own board:
`fw list` on a Heltec shows only Heltec holders, a RAK never stores a Heltec image, and a pull aimed at the other board's node stops at
once with "its firmware is for another board (...)". Both boards relay each other's transfers. A mixed circle needs a holder of each
board; a scan may need a second `fw peers` when two holders answer at the same moment.

## Different on the Heltec V4
| | RAK3401 | Heltec V4 |
|---|---|---|
| Image size | 1457 chunks (rc25) | 2584 chunks (about 1.8x the airtime) |
| Who installs | the LoRa-OTA bootloader | the app itself, into the other app partition, at the next start (a few seconds) |
| A new version that misbehaves | stays (fixed by the next version) | **rolls back by itself**: it must run 2 min AND hear a packet within 3 h, otherwise the board returns to the previous version (`fw slot` says "rolled back") |
| Seeding a new version into the first node | Bluetooth (`fw pull-ble` + nRF DFU app) | **Wi-Fi** (`fw pull-ble`, alias `fw pull-wifi`): an open access point, see below |
| Bootloader updates (`fw blupdate`, `fw bl`) | yes | not needed (answers "not on this board") |
| Recovery | Bluetooth / USB drive | USB (`esptool`), always possible: the chip's own ROM loader cannot be overwritten |

## Release files
| File | Use |
|---|---|
| `LoRa-OTA-HELTECV4-app-<ver>-first-install.bin` | **first LoRa-OTA install by USB**: `esptool` at `0x0`. It ends before `0xC90000`, so the node's identity and settings stay |
| `LoRa-OTA-HELTECV4-app-<ver>-slot.lota` | **Wi-Fi slot fill** (`fw pull-wifi`, upload at http://192.168.4.1): the node keeps running, then serves the new version |
| `LoRa-OTA-HELTECV4-app-<ver>.lota` | the LoRa-OTA image (same bytes as `-slot.lota`) |
| `...-usb-slot-0x610000.bin` | slot fill by USB: `esptool` at `0x610000` |
| `...-firmware.bin`, `-seeder.bin`, `-bootloader.bin`, `-partitions.bin`, `-boot_app0.bin` | the parts of the first install (app0 at `0x10000`) |

## Getting started (once per node)
1. **First install by USB** (a laptop with `esptool`): download mode = hold **PRG**, tap **RST**, release PRG; write the release's
   `LoRa-OTA-HELTECV4-app-<ver>-first-install.bin` at `0x0`; tap **RST**. The node keeps the identity and settings it had (they live
   past the end of this image). This also writes the LoRa-OTA partition table, once.
2. **Set the admin password and the circle**: `password ...`, `fw passphrase <your circle phrase>`; compare the fingerprint
   (`fw passphrase`) with your other nodes.
3. **Read back** `get repeat` (must be **on** for a repeater) and `get tx`. A fresh install uses the IoTThinks default tx 17 (about
   28 dBm at the antenna on the V4); a board that ran another MeshCore-based firmware keeps its old settings file, so check both.
4. `fw autoapply` is **off** on a new node: completed pulls wait in the slot for `fw apply`.

## Seeding a new version by Wi-Fi (no laptop)
1. Logged in as admin (phone app, remote admin): `fw pull-ble` (or `fw pull-wifi` - the same command).
2. The node restarts into its small seeder: an **open** Wi-Fi network `LoRa-OTA-xxxxxx` (no password: only a logged-in admin can start
   it, and it closes by itself).
3. Join it with the phone, open **http://192.168.4.1**, choose the release's `LoRa-OTA-HELTECV4-app-<ver>-slot.lota` and upload it (a
   few seconds). From a laptop: joining this network takes the laptop off your home Wi-Fi, so keep the file on the laptop's own disk.
4. The node goes back to its normal firmware by itself (after the upload, or after 5 min without one), checks the image and starts
   offering it to the circle. Nothing is installed yet: `fw apply` installs it on this node, `fw push` offers it to the others.

## Power saving and the USB console
With power saving on (`powersaving on`), the ESP32 sleeps up to 30 s between packets. LoRa-OTA keeps the node awake only while it has
work of its own (a pull, a window to send, an answer), so transfers keep their speed and a neighbour that only overhears a transfer
keeps sleeping. **The USB console switches off in sleep**, and a laptop may then switch that USB port off: manage a sleeping node over
LoRa (phone app / remote admin). For a USB session: `powersaving off` (over LoRa), then unplug and replug the cable.

Battery readings on the V4 are approximate; don't base battery-saving settings on them alone.

## Building
`pio run -e heltec_v4_lora_ota` (the repeater) and `pio run -e heltec_v4_lora_ota_seeder` (the Wi-Fi seeder); partition table
`boards/lora_ota_esp32s3_16MB.csv`. See [TESTING.md](TESTING.md) for the reproducible build.
