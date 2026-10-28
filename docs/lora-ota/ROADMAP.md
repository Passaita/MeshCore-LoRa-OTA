# LoRa-OTA - roadmap and known issues

## Next: v1.17.28-rc22 on MeshCore 1.18
* Port LoRa-OTA onto the IoTThinks **PowerSaving-v18** branch (MeshCore 1.18.0) once it has a release build. v18 is a fresh port, so
  our changes are re-applied, not merged.
* Clock handling moves onto IoTThinks' "keep time across reset" (one mechanism instead of two).
* `ver` names the image number (`fw-NNN`); small status-text clean-ups.

## Known issues
* **No automatic rollback.** A new app that runs but misbehaves stays until the next update (over LoRa or Bluetooth).
* **Weak batteries and Bluetooth.** Near 3.3 V some boards brown out when Bluetooth DFU starts (`start ota`, `fw pull-ble`). Do Bluetooth
  steps on USB power or a charged battery.
* **No low-voltage shutdown yet.** Waiting for MeshCore's configurable power-management stage; LoRa-OTA will build on it.
* **Remote-admin replies over weak relays** can arrive late or out of order (a MeshCore/companion effect, not LoRa-OTA's). Check the
  result with a read-only command.

## Later
* **Radio health watch:** detect a radio chip that was reset by a voltage dip and reinitialise it.
* **External SPI flash (RAK15001)** for a rollback copy of the previous app. Under evaluation: the module shares the radio's SPI bus on
  the RAK3401, so it needs a careful hardware check first.
* Other nRF52840 boards (RAK4631 class).

## Decided against
* **Signed images / a signature check in the bootloader** - nodes must always be able to return to stock firmware.
* **Persisted route memory** across pulls - routes go stale between rare updates, and it costs flash writes.
* **Changes to the MeshCore protocol or its forwarding** - LoRa-OTA uses only its own packets on top of MeshCore and listens to traffic
  it would hear anyway.
