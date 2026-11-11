# LoRa-OTA - command reference

Current for **v1.18.0-rc25** (RAK3401 and Heltec V4; differences are marked). Every `fw ...` command this fork adds. Standard MeshCore commands (`ver`, `get`, `set`, `reboot`,
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
* **`fw list [n]`** - the VERSIONS the last scan found, newest first: `#1 fw-318 x3 (0hop) | #2 bl-rev-8 x2 (0hop)` (x = holders,
  hops = the nearest). Two different builds with one number get a short digest `[A1B2]`. Sends nothing.
* **`fw list hosts [n]`** - the same per node: `ID NAME RSSI SNR [Nhop] #1 fw-318 #2 bl-rev-8 | next host`. A node lists only images for its own board.
* **`fw pull <#n|fw-N|id> [duty x] [fleet] [pause Nh]`** - pull an app image (options in any order).
  * `#n` / `fw-N`: that version from the best holder, with failover between up to 4 holders (chunks kept).
  * `id`: from exactly that node. The full 8-hex id works even when it isn't listed (targeted discovery); 2- or 3-byte prefixes start
    when exactly one node matches. During a running pull, the same command switches the source (same image required).
  * `duty x`: the duty for this pull only. `fleet`: also replace OLDER slot images on every LoRa-OTA node that hears the stream; they
    pass it on hop by hop (never installing); since rc25 it starts the same 7-day campaign as `fw push` once this node holds the
    image. `pause Nh` / `pause Nm`: set the pull up paused (1 min-168 h).
  * When verified, the image waits in the slot and is served; it is installed by `fw apply`, or at once if `fw autoapply` is on.
    A node that already runs that image just fills its slot.
* **`fw blupdate [force] <#n|bl-rev-N|id>`** - RAK3401: pull a BOOTLOADER the same way (Heltec V4: "not on this board"). The app is kept. `force` allows the same or an older
  revision.
* **`fw pull-status`** - progress (chunks, chunks/min, ETA), `WAITING for server`, `listening: shared stream`, `PAUSED ...`, or on a
  server the pulls it serves (`serving 'fw-318' to 2 pulls: ...`) and counters since boot.
* **`fw pull-pause [Nh|Nm|NhMm]`** - pause: chunks kept, the server's share freed, nothing sent until `fw pull-resume` (or N hours).
* **`fw pull-resume [duty x]`** - continue at once from the first missing chunk; `duty x` changes this pull's share (also while it runs). On a paused passive fill: finish it from the nearest holder
  (no install).
* **`fw pull-abort`** - cancel and clear the pull. On a `fleet` pull the fleet stops too (the cancel is repeated for 30 min); nodes that
  were filling pause and keep their chunks.
* **`fw route [<hops>|auto|off]`** - the route pairs (way there / way back) of the running pull, `*` = in use, and the last holder
  change. `fw route AA,BB` / `AA,BB/CC,DD` type a route (diagnosis only), `auto` = automatic again, `off` = direct.
* **`fw duty [0.1-1.0]`** - the share of the channel a pull may use, the relays' repeats included (1.0 ~ 30 % of the channel; default
  0.3). As a server: the ceiling for pulls from it, shared fairly. A pull runs at `min(puller, server)`.

## Pushing an image to the circle (rc25)
* **`fw push`** - on a node whose slot image is offered (`fw slot`, `fw serve`): every circle node of the same board that runs and
  holds something older fills its **slot** with that image, for 7 days; late nodes still find it. Nothing installs: use `fw apply` on
  each node. Running it again invites again (harmless). Refused during a pull.
* **`fw push-status`** - on the origin: `push fw-N 6d23h left: 5 complete, 1 filling, 1 stuck, 0 refused`. On a member: its part
  (filled / filling n% / waiting / stuck / refused: newer here).
* **`fw push-cancel`** - origin only: nobody joins any more, fills pause, filled slots stay, nothing installs.

## Seeding the slot by Bluetooth (RAK3401, bootloader rev 5+) or Wi-Fi (Heltec V4)
* **`fw pull-ble`** - restart into a Bluetooth upload that writes INTO THE SLOT (`3401_DFU`). Upload the release's `-slot.zip`. The node
  checks it, serves it, and doesn't install it. 5 minutes without a packet: back to the app, slot unchanged.
  **Heltec V4:** `fw pull-ble` = `fw pull-wifi`: the node restarts into its small seeder, an open Wi-Fi network `LoRa-OTA-xxxxxx`;
  upload the release's `-slot.lota` at http://192.168.4.1. Back to the app after the upload or after 5 minutes without one.
* **`fw apply`** - install the slot image now (pending + restart; on the Heltec V4 the app installs it at the next start, then a
  probation: 2 min running and a packet heard within 3 h, else back to the previous version). "already installed" if the node runs it.

## Serving
* **`fw serve`** - what this node offers now (app image + bootloader), or why nothing.
* **`fw serve on` / `fw serve off`** - offer what is available / offer nothing (persists).
* **`fw autoapply on|off`** - install a completed pull automatically. Default **off on a new node** (rc22+); a node that already
  belonged to a circle keeps its setting. Off: the slot is filled and served, `fw apply` later.
* **`fw minbatt [mV|off]`** - battery level below which this node is a LOW holder (pullers take it last); 150 mV lower is CRITICAL (no
  serving, no pulling until it recovers). Shows the battery now.

## Inspecting the node
* **`fw slot`** - what the slot holds: `slot: app fw-318, <size> B, crc <crc>, installed here, offered to neighbours`.
* **`fw bl`** - RAK3401: the running bootloader: revision, size, CRC, whether it is offered.
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
