# LoRa-OTA - user guide

Current for **app v1.17.28-rc21 (fw-304)** and **bootloader rev 8**. All commands are typed in the repeater's CLI: the serial console,
or the MeshCore phone app's remote-admin session after an admin login. Full command list: [COMMANDS.md](COMMANDS.md).

## 1. What a node needs

| Piece | How to check | Needed for |
|---|---|---|
| LoRa-OTA bootloader, rev 8 recommended | `fw bl` -> `bootloader rev 8, 39000 B, crc E0BC335E` | applying LoRa pulls; rev 5+ for `fw pull-ble` |
| LoRa-OTA app v1.17.28 | `ver` -> `v1.17.28-rc21 LoRa-OTA on v1.17.1@b7cc0e6d (Build: ...)` | everything below |
| The circle passphrase | `fw passphrase` -> `circle set, fingerprint XXXXXXXX` | finding, serving and pulling |

v1.17.28 does not talk LoRa-OTA with v1.17.27 or older (security v2 changed the protocol). Move such nodes by Bluetooth once.
rc12 and newer cannot read the answers of rc11 and older: update older nodes by Bluetooth, or seed the new version on a node that keeps
its old firmware (section 4).

Release files:

| File | Use |
|---|---|
| `LoRa-OTA-app-<ver>-firmware.zip` | app for the nRF DFU phone app after `start ota` (installs it) |
| `LoRa-OTA-app-<ver>-slot.zip` | for `fw pull-ble`: puts the image in the SLOT (served, not installed) |
| `LoRa-OTA-RAK3401-app-<ver>.lota` | the LoRa-OTA image itself (what nodes serve from their slot) |
| `LoRa-OTA-bootloader-rev8_s140_6.1.1-ONBOARD.zip` | bootloader by Bluetooth, from the stock OTAFIX bootloader |
| `LoRa-OTA-bootloader-rev8-bootloader_only.zip` | bootloader by Bluetooth, from an older LoRa-OTA bootloader |

## 2. Getting started: onboarding a node (one Bluetooth visit)
Use the nRF DFU phone app (or a Linux laptop with Bluetooth) and make two uploads, one right after the other:
1. `start ota` on the node, then upload the bootloader zip to `3401_DFU`. The node comes back in Bluetooth DFU **without an app**.
   That's expected.
2. Right away upload `LoRa-OTA-app-<ver>-firmware.zip` (the *firmware* zip, not the *slot* zip). The node restarts into the LoRa-OTA app.
   It keeps its name, identity and settings. It is off the air between the two uploads.
3. Join your circle (section 3), then check `ver` and `fw bl`.
4. Check your radio settings and read each one back. A fresh rc21 install already starts with `powersaving on`, RX power saving
   `balanced` and the receiver (AGC) reset at 60 s. Set TX power and `set af` for your site.
5. `clock sync` if the node has an RTC module (RAK12002). It then keeps time through power cuts.
6. The slot starts empty. It fills with the next update, or right away with `fw pull-ble` + the `-slot.zip`.

Tips: if an upload stalls, turn "Request high MTU" off in the DFU app (20-byte packets work everywhere, ~4-5 min for the app). The
LoRa-OTA bootloader leaves Bluetooth DFU after 5 minutes without a packet. A node with a valid app restarts it; a node without one
waits in DFU for the next try. With bootloader rev 4+ a triple-click on RST (~0.2-0.4 s apart) also enters Bluetooth DFU.

## 3. Your circle - once per node

LoRa-OTA works only between nodes that were given the **same passphrase**. On every node of your fleet:
```
fw passphrase <your phrase>     8-64 characters, e.g. four random words. ~20 s in the background; the node keeps relaying.
fw passphrase                   -> "circle set, fingerprint XXXXXXXX" - the same fingerprint on every node = the same circle
```
* Without a passphrase a node is **hidden**: it serves nothing, answers no scan and cannot pull. Normal mesh relaying is unaffected.
* The phrase is never stored, only a key derived from it. Keep the phrase safe: a new node needs it to join.
* A different fingerprint means a typo. Enter the phrase again.
* `fw passphrase off` removes it (not during a pull).

## 4. Updating nodes over LoRa

### One node
On the node you want to update (remote-admin session):
```
fw peers                scan, wait ~25 s
fw list                 -> "#1 fw-304 x3 (0hop) | #2 bl-rev-8 x3 (0hop)" - versions, newest first, how many hold it, nearest hops
fw pull #1              pull that version from the best holder
fw pull-status          progress at any time; the pull runs unattended
```
* The pull picks the holder with the best expected speed (its free duty share, the airtime of its relays, the link, its battery). It
  remembers up to 4 holders and moves to the next if its holder goes silent, keeping every chunk.
* When the image is complete and verified, the node installs it and restarts (`fw autoapply`, default on). It keeps its name, radio
  settings, identity, passwords and passphrase, and serves that version to others from then on.
* **If it already runs exactly that image**, the pull only fills its slot so it can serve it. No re-flash.
* `fw pull <8-hex id>` pulls from that one node, and finds it by itself when it isn't listed.
* **How long:** about 2 hours from a direct neighbour at `fw duty 1`, roughly 6 hours at the default 0.3, longer through relays
  (section 8).

### The whole fleet (fleet fill)
On one node that should install the new version: `fw peers`, `fw list`, then **`fw pull #1 fleet`**. That node installs as usual. Every
other LoRa-OTA node that hears the transfer stores the new version in its slot and passes it to its own neighbours over the next hours
(one hop at a time, quietly). **Nothing else installs by itself.** Check a node with `fw slot`, then `fw apply` it when you're ready.
Without `fleet`, a pull still fills nodes with an EMPTY slot, but never replaces an older image. `fw pull-abort` on a fleet pull stops
the fleet too.

### Shared stream
Nodes in radio range of a running transfer of the SAME version don't ask for their own copy. They listen and store it
(`fw pull-status`: "listening: shared stream"), then ask only for what they missed. Ten nodes near one server cost about as much
airtime as one.

### Seeding the first node of a new version (Bluetooth into the slot)
Someone has to hold the new image before anyone can pull it. With bootloader rev 5+:
1. `fw pull-ble` - the node restarts into a Bluetooth upload mode that writes **into the slot** (`3401_DFU`).
2. In the nRF DFU phone app, upload the release's **`-slot.zip`** (not the app zip).
3. The node returns to its old version with the new image in its slot and **serves it**. `fw apply` installs it on this node too.

**Staged rollout (recommended):** seed the new version into the slot of the node closest to you, but don't install it there yet.
Let one node you can reach easily pull and install it and watch it for a day. Then install it on the others, with the serving
nodes last.

### Pausing, resuming, aborting
* `fw pull-pause` - stops sending, keeps every chunk. `fw pull-resume` continues from the first missing chunk.
* `fw pull-pause 3h` - resumes by itself after 3 hours (1-168), e.g. to let solar relays on the way recharge overnight.
  `fw pull #1 pause 8h` sets a pull up already paused.
* `fw pull-abort` - drops the pull and its saved state. Safe at any time before the install: the running app is never touched until then.

## 5. Updating the bootloader over LoRa
`fw peers`, `fw list`, then `fw blupdate #n` on the `bl-rev-N` entry. The new bootloader is staged in the slot and copied by the MBR,
and the node restarts into its app with settings kept. Over LoRa **the app isn't touched**: this is how to update the bootloader of a
remote node. (A bootloader update by Bluetooth or USB *does* erase the app. See section 6.)

## 6. Moving a node back to stock firmware (offboarding)
* **Keep the LoRa-OTA bootloader, run stock firmware:** `start ota`, then upload the stock app zip (rev 6+: up to 792 KiB, enough for
  the stock RAK3401 repeater). The node keeps its identity and settings. To come back: on the stock app `start ota`, upload the
  LoRa-OTA app zip. Even the passphrase is still there.
* **Fully stock:** `start ota`, upload the stock OTAFIX bootloader package (`..._s140_6.1.1.zip`), then the stock app zip.
* On 1 W boards without a GPS module use stock PowerSaving 17.1.4 or newer. Some builds from late August 2026 switch off the power
  amplifier's supply when no GPS answers (MeshCore #3275). LoRa-OTA builds aren't affected (a release test guards it).
* Stock firmware can't `fw pull`: later updates need a visit again.

Bootloader updates by **Bluetooth or USB** store the new bootloader at the start of the app region, so the app is erased: always
upload the app zip right after. Over LoRa (`fw blupdate`) the app is kept.

## 7. Power and battery
* `fw minbatt <mV>` sets the battery level below which this node is a LOW holder (pullers take it last). 150 mV below that it is
  CRITICAL and stops serving (it still answers, so pullers know why). Examples: LiPo 3500, LiFePO4 3150.
* Recommended for solar nodes: `powersaving on`, `set radio.rxps balanced`, `set agc.reset.interval 60`, a TX power no higher than
  you need, and a duty cap on busy relays in winter (`set dutycycle ...`).
* A RAK19007-type base whose solar input rises above 5.5 V once the battery is full can freeze (MeshCore #2856). Regulate the solar
  input to 5 V or less. The watchdog (section 9) restarts a frozen node within 10 minutes.

## 8. How fast may a pull be? - `fw duty`
A transfer shares the channel with the mesh. `fw duty` (0.1-1.0, default **0.3**) sets the share of the channel a pull may use,
including the relays' repeats: **1.0 is about 30 % of the channel, 0.3 about 9 %**. On top of that, LoRa-OTA packets always queue
behind mesh traffic and wait for 2 s of quiet before sending (up to 20 s).
* A pull runs at `min(the puller's duty, the server's duty)`. A server sets its ceiling once (`fw duty 0.2`). Several pulls from one
  server share its duty fairly.
* `fw pull #n duty x` uses x for that one pull.
* MeshCore's own airtime budget (`get af`) also limits a server. Where no legal duty limit applies, `set af 0` on servers.
* Relays on solar pay for every repeated chunk. Prefer daytime pulls through them, or pause at night.

## 9. Which firmware, which time, why did it restart?
* `ver` shows our version, the upstream base and the build time. `fw list` names images by an internal number (`fw-304`);
  `fw serve` and `fw slot` show what this node offers and holds.
* `fw wdt` - the hardware watchdog (10 min) and the reason of the last reset. Repeated watchdog resets point at power or a bug.
* `fw time` - the time and where it came from. A node without an RTC asks a circle neighbour that has one, after every boot and every
  6 h. A clock that runs ahead is held until real time catches up (MeshCore never steps a clock back).
* `fw radio` - a record of the radio leaving receive mode or hearing nothing for a long time. Read it BEFORE rebooting a deaf node.

## 10. Troubleshooting
* **`fw list` is empty / "no peers"** - run `fw peers` first (it is the scan). Otherwise: the server has `fw serve off`, an empty slot
  (`fw serve` explains), a different passphrase (compare fingerprints), or is out of reach. Through weak relays one scan can miss a
  server. Scan again, or `fw pull <8-hex id>` keeps looking by itself.
* **"set 'fw passphrase <phrase>' first"** - the node has no circle yet.
* **`fw pull-status` shows `offering ... no answer yet`** - the server is out of reach or in another circle. The pull keeps trying.
* **A pull through relays gets no answers, but the server's status shows `offers=` rising** - its answers are lost on the way back
  (radio links aren't always symmetric). The server heals this by itself with a flooded answer (`heals=` on the server, "way back
  healed" on the puller). `fw route` shows the route pairs in use.
* **Pulls start fast and get slower, then fast again after rebooting the server** - MeshCore's airtime budget (`get af`), see section 8.
* **Replies over a remote session arrive late or under the next command** - over weak relays a reply can take a minute. Check the
  effect with a read-only command.
* **The board doesn't stay in Bluetooth DFU** - use the zip that matches the mode: after `fw pull-ble` only the `-slot.zip`, after
  `start ota` the `-firmware.zip` or a bootloader zip. A weak battery (~3.3 V) can brown out when Bluetooth starts: try on USB power
  or after the panel has charged.

## 11. Recovery (board not responding)
The bootloader runs before the app, so a hung app never blocks it.
* **Double-click RST** -> USB DFU (a "RAK3401" UF2 drive appears; serial DFU works). One press returns to the app.
* **Triple-click RST** (rev 4+) -> Bluetooth DFU (`3401_DFU`) without the app, for a frozen node on battery without USB.
* **No valid app** -> the bootloader advertises `3401_DFU` by itself: upload the app zip.
* **ST-Link (SWD) as a last resort:** back up the whole flash first (`flash read_bank 0 backup.bin 0 0x100000`). It contains the
  node's private identity key, so keep it private. Then `program <bootloader>.hex verify` **without** a mass erase. A mass erase
  wipes the node's identity and settings, so use it only when nothing else works.

## Safety notes
* A pull can be interrupted at any point before the install. The running app is only replaced by a fully verified image, and an
  install cut by a power failure is redone at the next boot.
* There is no automatic rollback to the previous app. Try every new version on a node you can reach before rolling it out.
* Images aren't signed (by decision, so nodes can always return to stock firmware). Anyone who knows your circle passphrase can
  serve your nodes an image. Treat the passphrase like an admin password. See [SECURITY.md](SECURITY.md).
