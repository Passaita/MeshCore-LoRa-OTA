# LoRa-OTA - user guide

Current for **app v1.17.28-rc6** and **bootloader rev 7**. All commands are typed in the repeater's CLI: the serial console, or the
MeshCore phone app's remote-admin session after an admin login. Full command list: [COMMANDS.md](COMMANDS.md).

## 1. What a node needs

| Piece | How to check | Needed for |
|---|---|---|
| LoRa-OTA bootloader, rev 7 recommended | `fw bl` -> `bootloader rev 7, 39000 B, crc ...` | applying LoRa pulls; rev 5+ for `fw pull-ble` |
| LoRa-OTA app v1.17.28 | `ver` -> `v1.17.28-... LoRa-OTA on v1.17.1@d9c42964` | everything below |
| The circle passphrase | `fw passphrase` -> `circle set, fingerprint XXXXXXXX` | finding, serving and pulling |

v1.17.28 does not talk LoRa-OTA with v1.17.27 or older (security v2 changed the protocol); move such nodes by Bluetooth once.

Release files (per release):

| File | Use |
|---|---|
| `LoRa-OTA-app-<ver>-firmware.zip` | app for the nRF DFU phone app after `start ota` (installs it) |
| `LoRa-OTA-app-<ver>-slot.zip` | for `fw pull-ble`: puts the image in the SLOT (served, not installed) |
| `LoRa-OTA-RAK3401-app-<ver>.lota` | the LoRa-OTA image itself (what nodes serve from their slot) |
| `LoRa-OTA-bootloader-revN-bootloader_only.zip` | bootloader update by Bluetooth |

## 2. Your circle - once per node

LoRa-OTA works only between nodes that were given the **same passphrase**. On every node of your fleet:
```
fw passphrase <your phrase>     8-64 characters, e.g. four random words. ~20 s in the background; the node keeps relaying.
fw passphrase                   -> "circle set, fingerprint XXXXXXXX" - the same fingerprint on every node = the same circle
```
* Without a passphrase a node is **hidden**: it serves nothing, answers no scan and cannot pull. Normal mesh relaying is unaffected.
* The phrase is never stored, only a key derived from it. Keep the phrase safe: a new node needs it to join.
* A different fingerprint means a typo - enter the phrase again.
* `fw passphrase off` removes it (not during a pull).

## 3. Updating a node over LoRa

On the node you want to update (remote-admin session):
```
fw list                 wait ~25 s
fw peers                -> e.g. "1A2B3C4D REPEATER-A -71 11.5 #1 fw-290 #2 bl-rev-7"
fw pull #1              pick the app image you want
fw pull-status          progress at any time; the pull runs unattended
```
* When the image is complete and verified the node installs it and restarts (`fw autoapply`, default on). It keeps its name, radio
  settings, identity, passwords and passphrase, and from then on serves that version to others.
* **If it already runs exactly that image**, the pull only fills its slot so it can serve it - no re-flash.
* `fw pull <8-hex id>` works when the server is not listed: the node finds it by itself (slower).
* **How long:** at the default `fw duty 0.3` roughly 6-9 h from a direct neighbour and 12-15 h through one relay; busy daytime
  channels take longer. `fw pull #1 duty 1` is about three times faster if the server allows it (see section 7).
* Prefer `fw autoapply off` on a node you want to install by hand later: the slot is filled and served, `fw apply` installs it.

### Seeding the first node of a new version (Bluetooth into the slot)
You need one node that has the new image before anyone can pull it. With bootloader rev 5+:
1. `fw pull-ble` - the node restarts into a Bluetooth upload mode that writes **into the slot** (`3401_DFU`).
2. In the nRF DFU phone app upload the release's **`-slot.zip`** (not the app zip).
3. The node returns to its old version with the new image in its slot and **serves it**. `fw apply` installs it on this node too.

**Staged rollout (recommended):** seed the new version into the slot of the node closest to you, but do not install it there yet.
Let the remote nodes pull it while both ends still run the same version, then install it on the seeding node last. A version
mismatch in the middle of a transfer is the worst case for speed and reliability.

### Pausing, resuming, aborting
* `fw pull-pause` - stops sending, keeps every chunk. `fw pull-resume` continues from the first missing chunk.
* `fw pull-pause 3h` - resumes by itself after 3 hours (1-168) - e.g. to let solar relays on the way recharge overnight.
* `fw pull-abort` - drops the pull and its saved state. Safe at any time before the install: the running app is never touched until then.

### The server went away in the middle of a pull
`fw pull-status` shows `WAITING for server ...`: the pull keeps its chunks and keeps asking, less and less often (for up to a week).
To continue from another node that offers the **same** version: `fw list`, `fw peers`, `fw pull #n` - the pull switches source and
keeps its chunks if the image is identical.

## 4. Updating the bootloader over LoRa
`fw list`, `fw peers`, then `fw blupdate #n` on the `bl-rev-N` entry. The new bootloader is staged in the slot, copied by the MBR,
and the node restarts into its app with settings kept. Over LoRa **the app is not touched** - this is the way to update the
bootloader of a remote node. (A bootloader update by Bluetooth or USB *does* erase the app - see section 6.)

## 5. Onboarding a node that runs stock firmware (one visit)
Bluetooth, nRF DFU phone app, two uploads in this order:
1. `start ota` on the stock node, then upload `LoRa-OTA-bootloader-rev7-bootloader_only.zip` to `3401_DFU`. The node comes back in
   Bluetooth DFU **without an app** - expected.
2. Right away upload `LoRa-OTA-app-<ver>-firmware.zip` to `3401_DFU`. It restarts into the LoRa-OTA app.
3. In the CLI: `ver`, `fw bl`, then the repeater basics for your site, then `fw passphrase <your phrase>` and compare the fingerprint.
4. `clock sync` if the node has an RTC module (RAK12002): it sets the clock and writes it into the RTC, which then keeps time through power cuts.
5. The slot starts empty. `fw list` + `fw pull #n` fills it (for the version it already runs this is a slot fill, no re-flash), so the
   node can serve that version to others.

Tips: if an upload stalls, turn "Request high MTU" off in the DFU app (20-byte packets work everywhere, ~4-5 min for the app). The
LoRa-OTA bootloader leaves Bluetooth DFU after 5 minutes without a packet: a node with a valid app restarts it, a node without one
waits in DFU for the next try.

## 6. Moving a node back to stock firmware (offboarding)
* **Keep the LoRa-OTA bootloader, run stock firmware:** `start ota`, upload the stock app zip (rev 6+: up to 792 KiB, enough for
  the stock RAK3401 repeater). The node keeps its identity and settings. To return: on the stock app `start ota`, upload the LoRa-OTA
  app zip.
* **Fully stock:** `start ota`, upload the stock OTAFIX bootloader package (`..._s140_6.1.1.zip`), then the stock app zip.
* Stock firmware cannot `fw pull`: later updates need a visit again.

Bootloader updates by **Bluetooth or USB** store the new bootloader at the start of the app region, so the app is erased: always
upload the app zip right after. Over LoRa (`fw blupdate`) the app is kept.

## 7. How fast may a pull be? - `fw duty`
A transfer shares the channel with the mesh. `fw duty` (0.1-1.0, default **0.3**) sets the share of the channel a pull may use,
the relays' repeats included: **1.0 is about 30 % of the channel, 0.3 about 9 %**. In addition LoRa-OTA packets always queue
behind mesh traffic and wait for 2 s of quiet before sending (up to 20 s).
* A pull runs at `min(the puller's duty, the server's duty)`, agreed when it starts. A server sets its ceiling once (`fw duty 0.2`).
* `fw pull #n duty x` uses x for that one pull.
* Relays on solar pay for every repeated chunk: prefer daytime pulls through them, or pause at night.

## 8. Which firmware is a node running?
`ver` shows our version, the upstream base and the build time, e.g. `v1.17.28-rc6 LoRa-OTA on v1.17.1@d9c42964 (Build: ...)`.
LoRa-OTA images are named by an internal number (`fw-290`) in `fw peers`; `fw serve` shows what this node offers.

## 9. Troubleshooting
* **`fw peers` says "no peers"** - the server has `fw serve off`, an empty slot (`fw serve` explains), a different passphrase
  (compare fingerprints) or is out of reach. Through weak relays one scan can miss a server: `fw pull <8-hex id>` keeps looking by itself.
* **`fw list` / `fw pull` answer "set 'fw passphrase <phrase>' first"** - the node has no circle yet.
* **`fw pull-status` shows `offering ... no answer yet`** - the server is out of reach or in another circle; the pull keeps trying.
* **A pull through relays gets no answers but the server's status shows `offers=` rising** - its answers are lost on the way back
  (radio links are not always symmetric). The server heals this by itself with a flooded answer (`heals=` on the server, "way back
  healed" on the puller). `fw route` shows the route pairs in use.
* **A node seems deaf** (answers on USB, but its packet counters stop) - run `fw radio` BEFORE rebooting it and keep the reply; then reboot.
* **Pulls start fast and get slower, fast again after rebooting the server** - MeshCore's own airtime budget (`get af`). Where no
  legal duty limit applies, `set af 0` (100 %) on servers; LoRa-OTA's own `fw duty` still keeps transfers gentle.
* **Replies over a remote session arrive late or under the next command** - over weak relays a reply can take ~50 s. Check the effect with a read-only command.
* **`ERR - unknown peer` after `fw pull-status `** - a trailing space (phone keyboard autofill) - remove it. Fixed in the next version.

## 10. Recovery (board not responding)
The bootloader runs before the app, so a hung app never blocks it.
* **Double-click RST** -> USB DFU (a "RAK3401" UF2 drive appears; serial DFU works). One press returns to the app.
* **Triple-click RST** (bootloader rev 4+) -> Bluetooth DFU (`3401_DFU`) without the app - for a frozen node on battery without USB.
* **No valid app** -> the bootloader advertises `3401_DFU` by itself: upload the app zip.
* **ST-Link (SWD) as a last resort:** back up the whole flash first (`flash read_bank 0 backup.bin 0 0x100000` - it contains the
  node's private identity key: keep it private), then `program <bootloader>.hex verify` **without** a mass erase. A mass erase wipes
  the node's identity and settings - only when nothing else works.

## Safety notes
* A pull can be interrupted at any point before the install; the running app is only replaced by a fully verified image.
* There is no automatic rollback to the previous app. Try every new version on a node you can reach before rolling it out.
* Images are not signed (by decision, so nodes can always return to stock firmware). Anyone who knows your circle passphrase can
  serve your nodes an image. Treat the passphrase like an admin password. See [SECURITY.md](SECURITY.md).
