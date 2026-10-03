# LoRa-OTA - command reference

Current for **v1.17.28-rc6**. Every `fw ...` command this fork adds. Standard MeshCore commands (`ver`, `get`, `set`, `reboot`,
`start ota`, `clock`, ...) are unchanged.

All commands work on the **serial console** and in the **remote-admin session** (phone app / companion), except `fw stage`
(serial only). Every reply fits 160 bytes. `fw help` lists the commands on the device; `fw help <cmd>` explains one.

## Circle
* **`fw passphrase <phrase>`** - join this node to your **circle**: LoRa-OTA works only between nodes with the same phrase
  (8-64 characters). The key is derived in the background (~20 s; the node keeps relaying). **`fw passphrase`** alone shows the
  progress, then a **fingerprint** - the same on every node with the same phrase. **`fw passphrase off`** forgets it (not during a
  pull). Only the derived key is stored, never the phrase.
* **Without a passphrase LoRa-OTA is off and hidden:** no serving, no answers to scans, no pulls; `fw list` / `fw pull` / `fw serve`
  say "set 'fw passphrase <phrase>' first".

## Discovery and pulling
* **`fw list`** - scan ~25 s for circle members offering firmware: asks the direct neighbours (twice) and sends one discovery flood
  through the relays on the circle's private channel. Each server answers a scan once; a direct neighbour answers only directly.
* **`fw peers [n]`** - the results, one block per host: `ID NAME RSSI SNR [Nhop] #1 fw-290 #2 bl-rev-7 | next host`. `rssi?` = the
  answer came as a flood. `fw peers n` continues from entry n. During a scan: "still scanning (N s left)".
* **`fw pull <id|#n> [duty x]`** - pull that peer's **app** image. `<id>`: `#n` from `fw peers` (best - uses the route the scan
  found); the full 8-hex id (pulls even if unlisted: a targeted discovery finds it); or 2/3 bytes (`AABB`/`AABBCC`) - the node scans
  and starts when exactly one node matches (two or more are refused and listed; none: it scans again with back-off, 1 min doubling
  to 30 min). Ids must be hex. During a running or waiting pull the same command **switches the source** (same version required;
  chunks kept if the image is identical). When verified, the node installs it (`fw autoapply`) - unless it already runs exactly that
  image: then the slot is just filled and served.
* **`fw blupdate [force] <id|#n>`** - pull that peer's **bootloader**; the node copies it and restarts, the app is kept. `force`
  allows the same or an older revision.
* **`fw pull-status`** - chunks, rate, ETA, retries, MAC failures, hops, time since the server last answered; `WAITING for server`,
  `offering ... attempt N`, `PAUSED ...`; `idle | serving ...` when not pulling.
* **`fw pull-pause [Nh]`** - pause: chunks and state kept, the server's share freed, the node **sends nothing** until
  `fw pull-resume` or for N hours (1-168). A pause survives a reboot (a timed pause then starts its N hours again).
* **`fw pull-resume`** - continue a paused pull from the first missing chunk.
* **`fw pull-abort`** - cancel a waiting / running / paused pull and clear its saved state.
* **`fw duty [0.1-1.0]`** - the share of the channel a pull may use, relays' repeats included; persistent; default **0.3**.
  **1.0 is about 30 % of the channel**, 0.3 about 9 %. As a server: the ceiling for every pull from it (several pullers share it). As
  a puller: what its pulls ask for. A pull runs at `min(pull's duty, server's duty)`. `fw duty` alone shows the setting, the running
  pull and what the node serves. LoRa-OTA packets always queue below mesh traffic and wait for 2 s of quiet (max 20 s).
  **`fw pull <id|#n> duty x`** = the duty for that one pull.
* **`fw route [<hops>|auto|off]`** - a pull keeps up to 4 route **pairs** to its server (way there / way back - radio links are not
  always symmetric) and fails over automatically. `fw route` alone lists them (`*` = in use, answered / unanswered, reply time).
  `fw route AA,BB` = manual override (also mid-pull), `fw route AA,BB/CC,DD` types both directions, `fw route auto` = automatic again,
  `fw route off` = direct. A typed route that stays unanswered (6 requests) falls back to discovery. For diagnosis only.
* While a server is silent, the pull asks again with back-off (4 s doubling to 60 s; after 10 unanswered offers up to 5 min) and
  re-discovers at 1, 6, 16, 36, 76 min, then hourly. A week of silence ends it. Automatic floods are limited to 4 per rolling hour
  per node; a server heals a puller's lost way back at most once an hour while that puller makes no progress.

## Bluetooth into the slot (bootloader rev 5+)
* **`fw pull-ble`** - restart into the bootloader's Bluetooth upload that writes **into the slot** (the app is untouched). Upload the
  release's **`-slot.zip`** to `3401_DFU` with the nRF DFU app. The node checks it (board, kind, sizes, CRC-32, SHA-256), marks the
  slot valid and **serves** it - it does not install it. Nothing uploaded for 5 minutes: back to the app, slot unchanged. An upload
  that is cut off leaves the slot empty. An upload overwrites the old slot image as soon as it starts.
* **`fw apply`** - install the slot image on this node now (pending + restart; the bootloader applies it). If the node already runs
  that image it answers "already installed".

## Serving
* **`fw serve`** - what this node offers now: e.g. `serving: app fw-290 + bootloader rev 7`, `no app (slot empty) + bootloader rev 7`,
  or why nothing is offered.
* **`fw serve on`** - offer what is available: the running bootloader always, the slot image when it is valid and installed here (or
  filled by `fw pull-ble`). **`fw serve off`** - offer nothing (persists). **`fw serve force`** - bench only: ignore the "installed
  here" rule.
* **`fw autoapply [on|off]`** - install a completed, verified pull automatically; default **on**. Off: the slot is filled and served,
  `fw apply` installs later.

## Inspecting the node
* **`fw radio [clear]`** - a record kept across reboots of the radio being out of receive mode for > 8 s and of nothing received for
  > 10 min, plus the time since the last packet. Read it before rebooting a node that seems deaf.
* **`fw bl`** - the running bootloader: revision, size, CRC, whether it is offered.
* **`fw slot`** - the slot header: empty / VALID / INVALID.
* **`fw verify`** - recompute the CRC-32 of the slot payload.
* **`fw ctl`** - the control page (apply state, boot attempts, fail count, last result; `last=0` = the last install succeeded).
  `fw ctl init` writes a fresh idle page.
* **`fw geo`** - the flash geometry this build was linked for.

## Bench / development only
* **`fw fill <kib>`** - erase the slot and write a test pattern with a valid header.
* **`fw erase`** - wipe the slot header (slot reads as empty).
* **`fw pending`** - mark "apply on next boot" for whatever is in the slot.
* **`fw stage <args>`** - serial only: `tools/fw_ota_stage.py` writes an image into the slot over USB.

## Help
* **`fw help`** - the command list; **`fw help <cmd>`** - one command.

Known quirk (fixed in the next version): a trailing space after a command (`fw pull-status `, added by phone keyboard autofill) is
read as a peer name and answered `ERR - unknown peer`.
