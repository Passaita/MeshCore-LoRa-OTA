# LoRa-OTA - roadmap and known issues

## Next
* **Better scans in busy areas:** a scan carries the list of holders it already heard, so only the missing ones answer, and answers are
  spread over a longer window (fewer collisions when many holders sit behind one relay). Studied in the simulator.
* **Reaching nodes a fleet wave misses:** a node that hears holders only through relays should still join a fill.
* **Bootloader boot-loop recovery (RAK3401):** a build that crashes at start is counted by the bootloader; after 3 failed starts it opens
  Bluetooth DFU for 30 min, so a phone near the site can install a good app without pressing buttons. Tested on the bench, not yet
  released.

## Known issues
* **No automatic rollback on the RAK3401.** A new app that runs but misbehaves stays until the next update (over LoRa or Bluetooth).
  The Heltec V4 rolls back by itself (probation).
* **A pull follows its server's new image.** If the serving node restarts and then offers a different image, a running pull starts over
  on that image instead of looking for another holder of the old one. Avoid restarting a server that is serving.
* **Weak batteries and Bluetooth.** Near 3.3 V some boards brown out when Bluetooth DFU starts (`start ota`, `fw pull-ble`). Do Bluetooth
  steps on USB power or a charged battery.
* **Mixed circles:** a scan may need a second `fw peers` when holders of both boards answer at the same moment.
* **Remote-admin replies over weak relays** can arrive late or out of order (a MeshCore/companion effect, not LoRa-OTA's). Check the
  result with a read-only command.

## Later
* **External SPI flash (RAK15001) on the RAK3401:** a backup of the running app before every install and a rollback after failed
  starts, a small library of known-good images. A proof of concept runs on the bench; the module shares the radio's SPI bus on the
  RAK3401, so the radio pauses briefly while it is used.
* **Delta updates:** send only the difference between two versions (a few percent of a full image for small fixes). Needs room for
  the rebuilt image: external flash on the RAK3401, the spare flash on the Heltec V4.
* Other boards (RAK4631 class, other nRF52840 and ESP32-S3 boards).

## Decided against
* **Signed images / a signature check in the bootloader** - nodes must always be able to return to stock firmware.
* **Persisted route memory** across pulls - routes go stale between rare updates, and it costs flash writes.
* **Changes to the MeshCore protocol or its forwarding** - LoRa-OTA uses only its own packets on top of MeshCore and listens to traffic
  it would hear anyway.
