# LoRa-OTA for RAK3401 MeshCore repeaters

**Firmware updates for repeaters you cannot easily reach - delivered over the mesh itself.**

LoRa-OTA lets a MeshCore repeater pull a new firmware image from a neighbouring repeater **over LoRa**, verify it, and install
it - with no site visit, no cable and no Bluetooth range needed. Once a node has the new version it can pass it on to the next
one, so a whole chain of repeaters can be updated from one place.

> **Status: release candidate (v1.17.28-rc6), private review.** It works on real hardware and has been field-tested on a small
> fleet, but it has **not** been independently reviewed yet. Please read [SECURITY.md](SECURITY.md) and treat it as experimental.

## Why this project exists

A repeater is only useful where it is hard to get to: on a mountain top, a tall roof, a radio tower or a pole in the middle of
farmland. Updating MeshCore normally means standing next to the node with a phone (Bluetooth, ~10-30 m) or a laptop (USB).
For those sites a firmware update can mean a day's drive, a climb, a lift rental or a landowner's permission - so in practice
remote repeaters stay on old firmware, keep known bugs, and drift away from the rest of the network.

LoRa-OTA removes the visit. The operator sends a few commands through MeshCore's existing remote-admin session (from the phone
app, from anywhere in the mesh), and the repeater fetches the update by itself, slowly and politely, from a neighbour that
already has it.

## Who benefits - and who does not

**Good fit:**
* **Mountain-top and hill-top repeaters** - hours of hiking or a seasonal road between visits.
* **Tall roofs, masts and towers** - out of Bluetooth reach from the ground, access needs a key holder, a climb or a lift.
* **Rural and remote infrastructure nodes** - solar-powered repeaters along valleys, highways or farmland, far apart.
* **A small fleet run by one operator or club** - a chain of repeaters where every node can serve the next one.

**Not the target:**
* **A typical city repeater** that you can walk up to: a phone and the standard Bluetooth update are simpler and faster.
* **Mixed fleets you do not control**: LoRa-OTA only works inside one operator's "circle" (a shared passphrase).
* **Busy urban channels**: a transfer takes hours by design, to leave the channel to normal traffic.

## What it costs (honestly)

* **Time, not airtime.** A pull is paced to leave the channel to the mesh: at the default `fw duty 0.3` a direct neighbour transfer
  takes roughly **6-9 hours**, one relay in between roughly **12-15 hours**. `fw duty 1.0` is about three times faster.
* **A custom bootloader** (an OTAFIX fork, one Bluetooth visit to install) and this forked repeater app.
* **One Bluetooth visit per node, once** - after that, updates are over LoRa. Moving back to stock firmware stays possible (see the user guide).
* **No signed images** (a deliberate decision so nodes can always return to stock firmware) - trust comes from the circle passphrase. See [SECURITY.md](SECURITY.md).

## How it works, in short

1. Every node of the operator's fleet is given the same **circle passphrase** (`fw passphrase ...`). Nodes without it are invisible to LoRa-OTA.
2. A repeater that runs a version keeps a compressed copy of it in a **staging slot** in flash and offers it to its circle.
3. On the node to be updated: `fw list` (find who offers what), `fw peers`, `fw pull #n`. The pull runs unattended for hours,
   survives reboots and lost links, fails over between routes, and waits for a server that went quiet.
4. When every chunk has arrived and the SHA-256 of the whole image matches the authenticated manifest, the node marks it pending and
   restarts; the **bootloader** decompresses the slot over the application and boots it. The node keeps its identity and settings.
5. The freshly updated node now serves the same image to the next repeater.

Transfers are encrypted (AES-128-CTR) with keys that only the two circle members involved can derive, and every packet carries a MAC.
Details: [HOW-IT-WORKS.md](HOW-IT-WORKS.md).

## Documents

| Document | For |
|---|---|
| [USER-GUIDE.md](USER-GUIDE.md) | Operators: setup, updating, onboarding a stock node, offboarding, recovery |
| [COMMANDS.md](COMMANDS.md) | Every `fw ...` command |
| [HOW-IT-WORKS.md](HOW-IT-WORKS.md) | Flash layout, discovery, pacing, routes, apply |
| [SECURITY.md](SECURITY.md) | Threat model, cryptography, known limitations, **what reviewers should look at** |
| [TESTING.md](TESTING.md) | Host tests (how to run them) and hardware results |
| [ROADMAP.md](ROADMAP.md) | Known issues and planned work |
| [CHANGELOG.md](CHANGELOG.md) | Release history |
| [../fw_pull_porting.md](../fw_pull_porting.md) | Carrying the feature to another MeshCore tree |

The bootloader half lives in a separate repository (**LoRa-OTA-bootloader**, a fork of the OTAFIX nRF52 bootloader).

## Hardware

Built and tested on the **RAKwireless RAK3401** (nRF52840 + SX1262, with the RAK13302 1 W booster), SoftDevice S140 6.1.1. Other
nRF52840 MeshCore boards are possible in principle (the feature is one directory plus small hooks) but are not tested.

## Credits and licence

* **MeshCore** - Scott Powell / rippleradios.com and contributors (MIT).
* **IoTThinks MeshCore fork, branch PowerSaving-v17** - the power-saving base this fork is built on (MIT).
* **OTAFIX bootloader** - oltaco, based on the Adafruit nRF52 bootloader (MIT); Nordic SDK components under Nordic's licence.
* **LoRa-OTA additions** - LoRa-OTA contributors, MIT, like the code they extend.

This is an independent fork. It is not an official MeshCore, IoTThinks, RAKwireless or Adafruit product.

The LoRa-OTA additions were written by a hobbyist operator with extensive help from an AI coding assistant. That is one more reason
an independent review is asked for before public release.
