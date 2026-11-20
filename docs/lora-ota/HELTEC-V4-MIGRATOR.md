# Heltec V4: onboarding from stock MeshCore without USB (the migrator)

A Heltec V4 running **stock MeshCore** can move to LoRa-OTA **over Wi-Fi from a phone**, standing near the node. You don't need a
laptop, a USB cable or access to the board, so a roof node stays on the roof. The node keeps its name, identity, contacts and settings.

Two files from the release (the migrator ships with the Heltec V4 release; until then build it, see the end):

| File | Use |
|---|---|
| `LoRa-OTA-HELTECV4-app-<ver>-migrator.bin` | the **migrator**: a small app that stock MeshCore's own Wi-Fi update page installs |
| `LoRa-OTA-HELTECV4-app-<ver>-migrate.bin` | the **bundle**: the LoRa-OTA app and the Wi-Fi seeder in one file, uploaded on the migrator's page |

## Why a migrator is needed
Stock MeshCore and LoRa-OTA use different partition tables on the 16 MB flash (stock: two 6.25 MB app partitions; LoRa-OTA: two 3 MB
app partitions, the update slot and the Wi-Fi seeder). Stock's own Wi-Fi update (`start ota`) can only replace an app inside the stock
layout. The migrator is an app that stock's updater accepts. Once running, it rewrites the layout itself. The node's files (identity,
settings, contacts) are at the same place in both layouts and are never written.

## Steps (about 5 minutes next to the node)
1. **Stock MeshCore**, logged in as admin (phone app / remote admin): `start ota`. The node opens the Wi-Fi network `MeshCore-OTA`.
   Join it, open **http://192.168.4.1/update** and upload `...-migrator.bin`. The node restarts into the migrator.
2. **The migrator** opens an **open** Wi-Fi network `LoRa-OTA-MIGRATE-xxxxxx` (the last six digits of the node's MAC). Join it, open
   **http://192.168.4.1**. The page shows the layout the node runs from and that stock MeshCore is kept.
3. Upload `...-migrate.bin` (about 1.5 MB, 15 s). The migrator checks both images: ESP32-S3 header, checksum, SHA-256, and that the app
   is a LoRa-OTA build for the Heltec V4. Anything else is refused and nothing changes.
4. Tick **"I'm next to the node and its power is stable"**, press **Migrate**. The node writes the new partition table and restarts
   into LoRa-OTA **on probation**. The app confirms itself after it has run 2 minutes and heard a packet.
5. Check over LoRa or the app: `ver` (the LoRa-OTA version), `fw slot` (`slot empty` on a new node), `get repeat`. Then join your
   circle as on any LoRa-OTA node: `fw passphrase <your phrase>` and compare the fingerprint ([HELTEC-V4.md](HELTEC-V4.md), Getting
   started, steps 2-4).

**Cancel** on the migrator's page, or 15 minutes with no activity, takes the node back to stock MeshCore unchanged.

The migrator's Wi-Fi is open, like the seeder's. Only an admin can start it (`start ota` needs the admin login). It is up only for
those few minutes, and it accepts only a LoRa-OTA Heltec V4 app.

## What can go wrong, and what happens then
| Event | Result |
|---|---|
| Wrong file, damaged upload, upload cut off | refused / "waiting for the bundle": upload again. Stock MeshCore is untouched |
| Power cut or restart before **Migrate** | the node comes back to the migrator; finished files are recognised by their contents |
| Power cut during the migration (after Migrate) | the migrator finishes it at the next start (it keeps a progress record) |
| The LoRa-OTA app fails its first start (no 2 min run + packet heard, or a restart on probation) | the chip goes back by itself: to **stock MeshCore** (usual case) or to the migrator's page after 2 more tries |
| **Power cut during the partition-table write itself** (2-3 flash operations, a few milliseconds) | **the node needs USB** (`esptool`). This is the only such case, and the reason for the checkbox: migrate on stable power |

If a node came back to stock MeshCore after a failed first start, it already has the LoRa-OTA layout and seeder. To retry, use
`start ota` and upload the plain LoRa-OTA app (`...-firmware.bin`); no migrator is needed.

## How it was tested
* **Host test** (`test/migrator_host_test.cpp`; the same engine code as on the board, on a simulated 16 MB NOR flash with an ESP-IDF 4.4
  bootloader model). It injects a **power cut at every flash operation** (337-355 per case), for both partitions stock may run from and
  for a good and a failing LoRa-OTA app. Every cut ends in LoRa-OTA, stock MeshCore or the migrator page, with nvs and the node's files
  unchanged, except cuts inside the table write (at most 3 operations). Refusals: unknown partition table, untagged app, migrator too
  big. Five **mutants** (known mistakes put back on purpose) are each caught.
* **Bench** (a Heltec V4 on USB, stock MeshCore v1.17.1 with the Wi-Fi updater, the v1.18.0-rc25 app and seeder):
  * the full migration (layout, app and seeder verified byte for byte, probation confirmed, identity kept);
  * `fw pull-wifi` into the seeder after it;
  * a restart in the middle of the upload and right after it;
  * a restart during probation, which rolled back to stock MeshCore;
  * the retry from stock with the plain app.
* **Found on the bench and fixed:** ESP-IDF's flash driver refuses writes anywhere in the partition the migrator runs from, and that
  partition covers the seeder's new place. The migrator now writes through a driver copy without that check and does its own range
  guarding (never the bootloader, table, nvs, the node's files, its own code or the kept stock app). The host test models the driver's
  rule, with a mutant for it.

## Building
`pio run -e heltec_v4_lora_ota_migrator` (environment in `tools/fwpull_env.ini`, built for the stock layout `default_16MB.csv`). The
bundle: `tools/make_migrate_bundle.py <app firmware.bin> <seeder firmware.bin> <fw number> <out>-migrate.bin` (checks both images, the
board tag and the sizes). Host test: `g++ -O2 -std=c++17 -I. -o mtest test/migrator_host_test.cpp
examples/lora_ota_migrator/MigrateEngine.cpp`, then `./mtest new_table.bin stock_table.bin` (both tables from `gen_esp32part.py`:
`boards/lora_ota_esp32s3_16MB.csv` and the framework's `default_16MB.csv`).
