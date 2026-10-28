# MeshCore LoRa-OTA: update your repeaters without going up the hill

**New firmware travels over the mesh itself, from the repeater next door. No climbing, no cable, no site visit.**

LoRa-OTA is a community fork of the MeshCore repeater firmware for the **RAK3401**. A repeater fetches a new firmware image from a
neighbouring repeater **over LoRa**, checks it and installs it by itself. Once it has the new version, it passes it on, so a whole
chain of repeaters can be updated from your desk.

> **Current release: [v1.17.28-rc21](https://github.com/Passaita/MeshCore-LoRa-OTA/releases/tag/v1.17.28-rc21)** (fw-304), public
> community test. It needs the **[LoRa-OTA bootloader](https://github.com/Passaita/LoRa-OTA-bootloader)** (rev 8), installed once per
> node by Bluetooth.

## The problem you probably know

Your repeater sits on a roof, a mast or a hilltop. It works fine until a new MeshCore firmware comes out, or a setting needs a fix that
only a new build can bring. Then someone has to drive out, climb up, open the box and stand next to it with a phone. With five or ten
repeaters that is a weekend, and in winter it often just doesn't happen. Nodes stay on old firmware for months, keep known bugs and
drift away from the rest of the network.

## What LoRa-OTA does about it

Your repeaters pass new firmware to each other **over the LoRa radio they already use for the mesh**. Put the new version on one node
(Bluetooth from your kitchen table is fine). The others can then fetch it from that node, or from any neighbour that already has it,
one hop at a time, across the whole network.

From your phone's remote-admin screen, an update looks like this:

```
fw peers        who around me has firmware? (takes about 25 seconds)
fw list         -> #1 fw-304 x4 (0hop)       the newest version, held by 4 nodes
fw pull #1      fetch it
```

That's all. The node downloads the image in the background, keeps relaying mesh traffic while it does, checks the whole image, installs
it and restarts. It keeps its **name, identity, contacts, radio settings and passwords**. After that it offers the new version to its own
neighbours.

## Why you'll like it

* **No more site visits for updates.** After a one-time setup, every update arrives over LoRa.
* **It stays out of your mesh's way.** Firmware chunks always wait behind normal mesh traffic, and have a hard limit on how much
  airtime they may use (`fw duty`), the relays' repeats included. Your repeater relays messages first and updates second.
* **A swarm, not a single server.** Every node that holds a version serves it. A download picks the best holder and moves to another
  one if its holder goes quiet, keeping every piece it already has.
* **Update the whole fleet in one go.** `fw pull #1 fleet` on one node lets every LoRa-OTA node in earshot quietly store the new version
  too, and pass it on. Nothing installs by itself: you decide when each node switches (`fw apply`), and you can keep your most important
  relays for last.
* **Ten nodes cost about as much airtime as one.** Nodes near a running transfer of the same version just listen along and only ask for
  the pieces they missed.
* **Patient on weak links.** Downloads survive reboots, lost links and silent servers, for days if needed. One-way radio links are
  detected and worked around. `fw pull-pause 8h` lets a solar relay on the way recharge overnight.
* **Kind to solar nodes.** A node with a low battery is asked last and stays quiet when it is critical. It catches up when the sun is
  back.
* **Private to your network.** Only nodes that share your *circle passphrase* can see, serve or fetch firmware, and every transfer is
  encrypted. Strangers' nodes can't push anything onto yours.
* **Self-healing.** A hardware watchdog restarts a frozen node within 10 minutes. Nodes without a clock chip get the time from a
  neighbour that has one.
* **The same power saving as the regular firmware.** LoRa-OTA is built on the IoTThinks PowerSaving firmware, so it keeps CPU sleep,
  RX power saving and the AGC reset.

## Does it really work?

* The firmware has been in daily use since September 2026 on a small test fleet: bench boards plus outdoor solar relays.
* Every release since rc18 was rolled out with **one Bluetooth seed and everything else over LoRa**, without a site visit. rc21 then ran
  a 24-hour soak test on all nodes with no faults.
* Before every release, the update logic runs through a network simulator built from the real MeshCore and LoRa-OTA code (208 checks).
  Deliberately broken versions (22 "mutants") are checked to make sure the tests catch them. The bootloader's install step has been
  power-cut-tested at every single flash write.
* The source in this repository rebuilds the published release binary **byte for byte** (see [Building](#building)).
* An update from a direct neighbour takes about 2 hours at full LoRa-OTA speed (`fw duty 1`), roughly 6 hours at the gentle default,
  and longer through relays. You don't have to watch it.

Details: [docs/lora-ota/TESTING.md](docs/lora-ota/TESTING.md).

## Who it is for

**Good fit:** hill-top and mountain-top repeaters; tall roofs, masts and towers; solar infrastructure nodes far apart; a fleet run by
one operator or club, where every node can serve the next.

**Not the target:** a repeater you can walk up to (a phone is quicker); fleets you don't control (LoRa-OTA only works inside one
circle); anyone who needs an update within minutes (a transfer takes hours by design).

## What you need

* A **RAK3401** repeater (WisBlock, nRF52840 + SX1262, with the RAK13302 1 W booster). Other boards have not been tested yet.
* A phone with the **nRF Device Firmware Update** app (or a laptop with Bluetooth), **once per node**, within Bluetooth range.
* About **15 minutes per node** for that one-time setup.

## Getting started (once per node)

1. **Install the LoRa-OTA bootloader.** Send `start ota` from the phone app. The node shows up as `3401_DFU` in the nRF DFU app.
   Upload `LoRa-OTA-bootloader-rev8_s140_6.1.1-ONBOARD.zip` from the
   [bootloader release](https://github.com/Passaita/LoRa-OTA-bootloader/releases/tag/bootloader-rev8).
2. **Install the app right after.** Upload `LoRa-OTA-app-v1.17.28-rc21-firmware.zip` from the
   [app release](https://github.com/Passaita/MeshCore-LoRa-OTA/releases/tag/v1.17.28-rc21) (the *firmware* zip, not the *slot* zip).
   The node is off the air between step 1 and step 2, so do them back to back.
3. **Join your circle.** In the remote-admin session: `fw passphrase <your phrase>` (8-64 characters, four random words work well).
   Use the same phrase on every node and keep it somewhere safe. `fw passphrase` then shows a fingerprint, which must match on all your
   nodes.
4. **Check the radio settings.** Apply your usual repeater settings and read them back. A fresh install already has power saving, RX
   power saving and the AGC reset switched on.

That's it. From now on this node updates over LoRa. Full walk-through: [USER-GUIDE.md, section 2](docs/lora-ota/USER-GUIDE.md).

## Rolling out a new version (the safe way)

1. **Seed one node by Bluetooth without installing:** `fw pull-ble`, then upload the release's **`-slot.zip`**. The node keeps running
   its current firmware and offers the new one.
2. **Let one node you can reach easily pull and install it** (`fw peers`, `fw list`, `fw pull #1`, or `fw pull #1 fleet` to fill
   everyone's slot along the way). Watch it for a day.
3. **Install it on the others** with `fw apply` (if their slot is already filled) or `fw pull #1`. Do the serving nodes last.

## Is it safe? The honest part

* The running firmware is **not touched until the complete image has been verified** (CRC-32 and SHA-256). You can cancel a download at
  any time before then (`fw pull-abort`) with no risk.
* A power cut during the install is safe: the next start simply finishes the install.
* There is **no automatic rollback**. If a new version runs but has a bug, you fix it with the next version (over LoRa again) or by
  Bluetooth. This is why you roll out one node first.
* Images are **not signed**. This is a deliberate choice so that moving back to stock firmware stays easy. Your circle passphrase is
  the trust boundary, so keep it private.
* LoRa-OTA has **not had an independent security review yet**. Reviewers are very welcome: start with
  [docs/lora-ota/SECURITY.md](docs/lora-ota/SECURITY.md) and report findings through GitHub's private vulnerability reporting
  (Security tab).
* This is a **community project**, not an official MeshCore, IoTThinks or RAKwireless release. It is built and tested carefully, but on
  a small fleet. Start with a node you can reach.

## Changed your mind?

Going back is easy and keeps the node's name, identity and settings: `start ota`, then upload the stock repeater zip with the nRF DFU
app (on 1 W boards use IoTThinks / EasySkyMesh PowerSaving 17.1.4 or newer). The LoRa-OTA bootloader runs stock firmware fine. If you
come back later, even your passphrase is still there. Details: [USER-GUIDE.md, section 6](docs/lora-ota/USER-GUIDE.md).

## Documentation

| You want to... | Read |
|---|---|
| set up a node, update a fleet, fix a problem, or recover a board | [USER-GUIDE.md](docs/lora-ota/USER-GUIDE.md) |
| look up one command | [COMMANDS.md](docs/lora-ota/COMMANDS.md) |
| understand the image format, discovery, swarm, pacing and routes | [HOW-IT-WORKS.md](docs/lora-ota/HOW-IT-WORKS.md) |
| review the threat model and cryptography | [SECURITY.md](docs/lora-ota/SECURITY.md) |
| run the tests and the network simulator | [TESTING.md](docs/lora-ota/TESTING.md) |
| see what changed between versions | [CHANGELOG.md](docs/lora-ota/CHANGELOG.md) |
| know what is planned and what is known not to work yet | [ROADMAP.md](docs/lora-ota/ROADMAP.md) |
| carry the feature to another MeshCore tree | [fw_pull_porting.md](docs/fw_pull_porting.md) |
| install or rebuild the bootloader | the [LoRa-OTA-bootloader](https://github.com/Passaita/LoRa-OTA-bootloader) repository |

## Building

* App: `pio run -e RAK_3401_lora_ota` (the environment is in `platformio.local.ini`; `RAK_3401_lora_ota_dev` is the bench build with
  extra test commands).
* Reproducible: `SOURCE_DATE_EPOCH=1792083074 pio run -e RAK_3401_lora_ota` rebuilds the published **v1.17.28-rc21** binary byte for
  byte. Compare it with `SHA256SUMS.txt` in the release.
* Tests: host tests in `test/fw_ota_*`, the network simulator `bash test/fw_ota_sim_test.sh`. See
  [TESTING.md](docs/lora-ota/TESTING.md).
* The LoRa-OTA code lives in `src/helpers/fw_ota/`; the repeater hooks are in `examples/simple_repeater/`.

## Credits and licence

* **[MeshCore](https://github.com/meshcore-dev/MeshCore)**: Scott Powell / rippleradios.com and contributors (MIT). For MeshCore itself,
  its clients, flasher and community, see the upstream repository and [docs.meshcore.io](https://docs.meshcore.io).
* **[IoTThinks MeshCore fork](https://github.com/IoTThinks/MeshCore), branch PowerSaving-v17**: the power-saving base this fork is
  built on (MIT).
* **[OTAFIX bootloader](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX)**: oltaco, based on the Adafruit nRF52
  bootloader (MIT). The LoRa-OTA bootloader fork is in its [own repository](https://github.com/Passaita/LoRa-OTA-bootloader).
* **LoRa-OTA additions**: LoRa-OTA contributors, MIT like the code they extend ([license.txt](license.txt)).

This is an independent community fork, not an official MeshCore, IoTThinks, RAKwireless or Adafruit product. The LoRa-OTA additions
were written by a hobbyist operator with extensive help from an AI coding assistant. That is one more reason independent review and
bench reports are very welcome.
