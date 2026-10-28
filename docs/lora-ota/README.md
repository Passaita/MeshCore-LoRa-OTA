# LoRa-OTA for RAK3401 MeshCore repeaters

**Update your repeaters without going up the hill. New firmware travels over the mesh itself.**

LoRa-OTA lets a MeshCore repeater fetch a new firmware image from a neighbouring repeater **over LoRa**, check it, and install it.
You don't need a site visit, a cable or Bluetooth range. Once a node has the new version it passes it on, so a whole chain of repeaters
can be updated from your desk.

> **Status: v1.17.28-rc21, public community test.** The firmware has been in daily use since September 2026 on a small fleet
> (bench boards plus outdoor solar relays), and every release since rc18 was rolled out over LoRa without a site visit. It has
> **not** had an independent security review yet. Please read [SECURITY.md](SECURITY.md), start with a node you can reach, and
> report what you find.

## Why this project exists

A repeater is most useful where it is hard to reach: a mountain top, a tall roof, a radio tower, a pole in the middle of farmland.
Updating MeshCore normally means standing next to the node with a phone (Bluetooth, ~10-30 m) or a laptop (USB). For those sites an
update can mean a day's drive, a climb, a lift rental or a landowner's permission. In practice, remote repeaters stay on old firmware,
keep known bugs and drift away from the rest of the network.

LoRa-OTA removes the visit. You send a few commands through MeshCore's existing remote-admin session (from the phone app, anywhere in
the mesh), and the repeater fetches the update by itself, slowly and politely, from any neighbour that already has it:

```
fw peers        who around me has firmware?  (a ~25 s scan)
fw list         -> #1 fw-304 x4 (0hop)        newest version first, held by 4 nodes
fw pull #1      fetch it from the best holder; it installs and restarts by itself
```

## Why you might like it

* **No site visits for updates.** After a one-time Bluetooth setup per node, every update arrives over LoRa.
* **Mesh traffic always comes first.** LoRa-OTA packets queue behind mesh traffic, wait for quiet air, and have a hard airtime cap
  (`fw duty`) that counts the relays' repeats too.
* **A swarm, not a single server.** Every node that holds a version serves it. A pull picks the best holder (link quality, relays,
  load, battery), and moves to another holder if its holder goes silent, keeping every chunk.
* **Update a whole fleet with one command.** `fw pull #1 fleet` lets every LoRa-OTA node that hears the transfer store the new image
  and pass it on hop by hop. Nothing installs by itself: you run `fw apply` on each node when you're ready.
* **Ten nodes cost about as much airtime as one.** Nodes near a transfer of the same image listen along and only ask for what they
  missed.
* **Patient on long, weak chains.** Pulls survive reboots, lost links and silent servers (for days). One-way radio links are
  detected and healed. `fw pull-pause 8h` lets a solar relay on the way recharge overnight.
* **Kind to solar nodes.** Battery-aware holder ranking. A node in a critical battery state stays quiet and catches up later.
* **Private to your network.** Only nodes that share your circle passphrase can see, serve or fetch firmware, and every transfer is
  encrypted with its own session keys.
* **Self-healing extras.** A hardware watchdog restarts a frozen node within 10 minutes. Nodes without a clock chip get the time from
  a circle neighbour that has one.
* **Built on the IoTThinks PowerSaving firmware**, keeping its CPU sleep, RX power saving and AGC reset.

## Who it is for, and who it isn't

**Good fit:** mountain-top and hill-top repeaters; tall roofs, masts and towers; solar infrastructure nodes far apart; a fleet run by
one operator or club, where every node can serve the next.

**Not the target:** a repeater you can walk up to (a phone is quicker); fleets you don't control (LoRa-OTA only works inside one
circle); anyone who needs an update within minutes (a transfer takes hours by design).

## What it costs (honestly)

* **Time, not airtime.** About 2 hours from a direct neighbour at `fw duty 1`, roughly 6 hours at the gentle default `fw duty 0.3`, and
  longer through relays. It runs unattended.
* **A custom bootloader** (an OTAFIX fork, see the **LoRa-OTA-bootloader** repository) and this forked repeater app, installed by
  Bluetooth once per node.
* **No automatic rollback** and **no signed images** (a deliberate choice, so that nodes can always return to stock firmware). The
  circle passphrase is the trust boundary.
* **One board so far:** RAK3401 (nRF52840 + SX1262 with the RAK13302 1 W booster). Other nRF52840 boards are possible in principle, but
  untested.

## Documents

| Document | For |
|---|---|
| [USER-GUIDE.md](USER-GUIDE.md) | Operators: setup, updating, fleet rollout, onboarding, offboarding, recovery |
| [COMMANDS.md](COMMANDS.md) | Every `fw ...` command |
| [HOW-IT-WORKS.md](HOW-IT-WORKS.md) | Image, discovery, pull, swarm, pacing, routes, bootloader |
| [SECURITY.md](SECURITY.md) | Threat model, cryptography, known limitations, **what reviewers should look at** |
| [TESTING.md](TESTING.md) | Host tests, the network simulator, hardware results |
| [ROADMAP.md](ROADMAP.md) | Known issues and planned work |
| [CHANGELOG.md](CHANGELOG.md) | Release history |
| [../fw_pull_porting.md](../fw_pull_porting.md) | Carrying the feature to another MeshCore tree |

## Credits and licence

* **MeshCore** - Scott Powell / rippleradios.com and contributors (MIT).
* **IoTThinks MeshCore fork, branch PowerSaving-v17** - the power-saving base this fork is built on (MIT).
* **OTAFIX bootloader** - oltaco, based on the Adafruit nRF52 bootloader (MIT); Nordic SDK components under Nordic's licence.
* **LoRa-OTA additions** - LoRa-OTA contributors, MIT, like the code they extend.

This is an independent community fork. It is not an official MeshCore, IoTThinks, RAKwireless or Adafruit product.

The LoRa-OTA additions were written by a hobbyist operator with extensive help from an AI coding assistant. That is one more reason
independent review and bench reports are very welcome.
