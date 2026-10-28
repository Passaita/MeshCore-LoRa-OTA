# LoRa-OTA - command reference

Current for **v1.17.28-rc21**. Every `fw ...` command this fork adds. Standard MeshCore commands (`ver`, `get`, `set`, `reboot`,
`start ota`, `clock`, ...) are unchanged and documented by MeshCore.

All commands work on the **serial console** and from the **remote-admin session** (phone app / companion), except `fw stage` (serial
only). Every reply fits 160 bytes. A trailing space is ignored. `fw help` lists the commands on the device; `fw help <cmd>` explains one.

## Circle
* **`fw passphrase <phrase>`** - join this node to your circle. LoRa-OTA works only between nodes with the SAME phrase (8-64 characters).
  The key is derived in the background (~20 s, the node keeps relaying). Only the derived key is stored, never the phrase.
* **`fw passphrase`** - progress, then the circle **fingerprint** (the same on every node of one circle).
* **`fw passphrase off`** - forget it (not during a pull). Without a passphrase LoRa-OTA is off and the node is hidden.

## Finding and pulling firmware
* **`fw peers`** - scan ~25 s for circle nodes offering firmware: two zero-hop requests to the direct neighbours and one discovery flood
  on the circle's private channel. Each server answers a scan once.
* **`fw list [n]`** - the VERSIONS the last scan found, newest first: `#1 fw-304 x3 (0hop) | #2 bl-rev-8 x2 (0hop)` (x = holders,
  hops = the nearest). Two different builds with one number get a short digest `[A1B2]`. Sends nothing.
* **`fw list hosts [n]`** - the same per node: `ID NAME RSSI SNR [Nhop] #1 fw-304 #2 bl-rev-8 | next host`.
* **`fw pull <#n|fw-N|id> [duty x] [fleet] [pause Nh]`** - pull an app image (options in any order).
  * `#n` / `fw-N`: that version from the best holder, with failover between up to 4 holders (chunks kept).
  * `id`: from exactly that node. The full 8-hex id works even when it isn't listed (targeted discovery); 2- or 3-byte prefixes start
    when exactly one node matches. During a running pull, the same command switches the source (same image required).
  * `duty x`: the duty for this pull only. `fleet`: also replace OLDER slot images on every LoRa-OTA node that hears the stream; they
    pass it on hop by hop (never installing). `pause Nh`: set the pull up paused (1-168 h).
  * When verified, the node installs it (`fw autoapply`), unless it already runs that image (then the slot is just filled).
* **`fw blupdate [force] <#n|bl-rev-N|id>`** - pull a BOOTLOADER the same way. The app is kept. `force` allows the same or an older
  revision.
* **`fw pull-status`** - progress (chunks, chunks/min, ETA), `WAITING for server`, `listening: shared stream`, `PAUSED ...`, or on a
  server the pulls it serves (`serving 'fw-304' to 2 pulls: ...`) and counters since boot.
* **`fw pull-pause [Nh]`** - pause: chunks kept, the server's share freed, nothing sent until `fw pull-resume` (or N hours).
* **`fw pull-resume`** - continue at once from the first missing chunk. On a paused passive fill: finish it from the nearest holder
  (no install).
* **`fw pull-abort`** - cancel and clear the pull. On a `fleet` pull the fleet stops too (the cancel is repeated for 30 min); nodes that
  were filling pause and keep their chunks.
* **`fw route [<hops>|auto|off]`** - the route pairs (way there / way back) of the running pull, `*` = in use, and the last holder
  change. `fw route AA,BB` / `AA,BB/CC,DD` type a route (diagnosis only), `auto` = automatic again, `off` = direct.
* **`fw duty [0.1-1.0]`** - the share of the channel a pull may use, the relays' repeats included (1.0 ~ 30 % of the channel; default
  0.3). As a server: the ceiling for pulls from it, shared fairly. A pull runs at `min(puller, server)`.

## Bluetooth into the slot (bootloader rev 5+)
* **`fw pull-ble`** - restart into a Bluetooth upload that writes INTO THE SLOT (`3401_DFU`). Upload the release's `-slot.zip`. The node
  checks it, serves it, and doesn't install it. 5 minutes without a packet: back to the app, slot unchanged.
* **`fw apply`** - install the slot image now (pending + restart). "already installed" if the node runs it.

## Serving
* **`fw serve`** - what this node offers now (app image + bootloader), or why nothing.
* **`fw serve on` / `fw serve off`** - offer what is available / offer nothing (persists).
* **`fw autoapply on|off`** - install a completed pull automatically (default on). Off: the slot is filled and served, `fw apply` later.
* **`fw minbatt [mV|off]`** - battery level below which this node is a LOW holder (pullers take it last); 150 mV lower is CRITICAL (no
  serving, no pulling until it recovers). Shows the battery now.

## Inspecting the node
* **`fw slot`** - what the slot holds: `slot: app fw-304, <size> B, crc <crc>, installed here, offered to neighbours`.
* **`fw bl`** - the running bootloader: revision, size, CRC, whether it is offered.
* **`fw verify`** - recompute the slot's CRC-32 and compare it with its header.
* **`fw ctl`** - the control page (apply state, attempts, last result; `last=0` = the last install succeeded).
* **`fw geo`** - the flash geometry this build was linked for.
* **`fw wdt [clear]`** - the hardware watchdog (10 min), the reason of the last reset, the number of watchdog resets.
* **`fw radio [clear]`** - a record, kept across reboots, of the radio leaving receive mode (> 8 s) or hearing nothing (> 10 min).
* **`fw time [ask]`** - the time to the second and where it came from (RTC chip, or a circle neighbour with one). `ask` = ask now.
* **`fw erase`** - mark the slot empty (refused while a pull uses it). The node then serves only its bootloader.

## Development build only (env `RAK_3401_lora_ota_dev`, `-D FW_OTA_DEV`)
* **`fw fill <kib>`**, **`fw pending`**, **`fw ctl init`**, **`fw serve force`**, **`fw wdt hang`** - bench tests.
* **`fw stage <args>`** - serial only: `tools/fw_ota_stage.py` writes an image into the slot over USB.

## Help
* **`fw help`** - list the `fw` commands. **`fw help <cmd>`** - one command's arguments and what it does.
