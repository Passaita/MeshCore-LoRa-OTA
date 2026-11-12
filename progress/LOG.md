# LoRa-OTA - project log

Firmware updates over LoRa for MeshCore repeaters (RAK3401 and Heltec V4).

This is a dated, public record of what the project did and where it stands. It is a summary, not a manual. It shows the order in which ideas were designed, built and tested. Dates are the dates in the project's own working notes. Where a date is only known roughly, it says "around".

*Last updated: 12 November 2026.*

---

## What this project is

Solar repeaters sit on roofs, masts and hills. Updating their firmware usually means a trip, a cable or a phone close by.

LoRa-OTA lets a MeshCore repeater fetch a new firmware **over the LoRa mesh itself**, check it, and install it. No site visit.

* A node **pulls** a firmware image from a neighbour that already has it, piece by piece, at a low share of airtime.
* The image is **checked** end to end before anything is installed.
* A small **custom bootloader** installs it at the next start.
* Every node that holds an image can **serve** it onward, so an update spreads through the mesh.
* Mesh traffic always goes first. The update waits for quiet air.

Hardware: RAK3401 (nRF52840 + SX1262, 1 W booster) and Heltec V4 (ESP32-S3 + SX1262). Base: MeshCore with its power-saving features kept.

---

## Where we stand now (12 Nov 2026)

* **Released:** v1.18.0-rc25 for RAK3401 and Heltec V4, the same source for both. It passed its canary soak on 11 Nov and is published on GitHub as a pre-release.
* **Test fleet:** all RAK3401 nodes run rc25 with bootloader rev 9 (automatic recovery from a boot loop).
* **In progress (RAK3401):** two app slots instead of one. A new app starts on trial. If it does not confirm itself, the previous app comes back on its own. Proven on hardware on 12 Nov.
* **Next:** delta updates. A node fetches only the difference to the version it runs, not the whole image. The decoder is written and tested; the transfer comes next.
* **Smaller app:** about 57 KB less flash from build settings alone (measured, 12 Nov).

---

## Concepts

**Swarm - pull a version, not a host.** A node asks for a version, not for a particular neighbour. It fetches from the best holder in reach. If that holder goes quiet, the pull continues from another holder of the same image. Pullers of the same image share one stream.

**Circle encryption.** Nodes in one owner's "circle" share a passphrase. Outsiders cannot see the update service. Each pull gets its own session key and every chunk is encrypted. No admin password travels over the air.

**Fair airtime.** LoRa-OTA uses only a set share of airtime. Relays are counted, so a pull through three repeaters goes slower on purpose. Several pullers share the budget.

**Fleet fill and push.** A pull also fills the empty slots of nodes that overhear it. A push campaign asks the whole circle to fetch a new version over several days. Filling and installing are separate steps: nothing installs itself unless allowed.

**From one slot to two (A/B).** The first design used one staging slot plus a bootloader that unpacks the image. The next step is two app slots: the new app goes into the free slot and starts on trial. After 3 failed trial boots the bootloader returns to the previous app by itself.

**Delta updates.** Most of a new version is the same as the old one. A patch between two versions measured about 8-35 KB, against about 233 KB for a whole compressed image. Fewer chunks means less airtime and faster updates.

**Size.** A smaller app means fewer chunks per update and room for two slots. Build settings alone saved about 57 KB.

**Safety net.** A hardware watchdog restarts a hung node. A node stuck in a boot loop opens Bluetooth recovery by itself, so a phone near the site can fix it without a button press. Bluetooth and USB recovery always stay available.

---

## Timeline

### September 2026

* **15 Sep - First design.** An early draft with two app slots (A/B) was set aside. The project starts with one staging slot and a custom bootloader that unpacks a compressed image. Simpler, and it fit the flash at the time.
* **16-18 Sep - First custom bootloader.** The first attempt left the bench board unbootable; it was recovered with a debug probe and the bug was found. On 18 Sep a fixed bootloader installed an image from the slot on hardware.
* **19 Sep - First complete firmware update over LoRa.** One board pulled about 1,400 chunks from another in about 19 minutes with zero retries, then installed it and came back on the new version.
* **19 Sep - Hands-off.** Pull and install without anyone touching the node. A pull survives a restart of either side. A node serves only an image it has installed and runs.
* **19 Sep - Through ordinary repeaters.** A pull worked through stock MeshCore repeaters that know nothing about LoRa-OTA.
* **Around 19-20 Sep - Overhearing and a fleet rehearsal.** Chunks go straight into flash; a puller can use chunks sent to someone else. Three boards updated one after another in a rehearsal.
* **20 Sep - Bootloader updates over LoRa, and the name LoRa-OTA.** The bootloader itself can now be updated over the air. Stock boards are brought in over Bluetooth.
* **21 Sep - Remote control and path discovery.** Update commands work from a phone over MeshCore's remote admin. A node finds its way to a server through repeaters by itself, using MeshCore's multi-byte path hashes.
* **24 Sep - Easy way back.** A new bootloader revision allows a normal factory app to be installed again (offboarding).
* **26-27 Sep - Smarter routes.** Routes are scored and replaced when they fail. One-way radio links are handled. First pull through two stock repeaters on 27 Sep.
* **27 Sep - Bluetooth by button.** Three quick presses of reset put a node into Bluetooth update mode, for a frozen node without a cable.
* **27 Sep - First Heltec V4 work.** A Heltec V4 that shut itself down at every start was diagnosed (battery reading) and turned into a working repeater.
* **28 Sep - Airtime limits.** LoRa-OTA gets its own duty-cycle setting. It counts relays and always yields to mesh traffic.
* **29 Sep - Security v2.** Circle passphrase, hidden service, a session key per pull, encrypted chunks, no admin password on air.
* **29 Sep - Bluetooth slot fill.** A phone or laptop can load an image into a node's slot over Bluetooth. The node then serves it over LoRa.
* **29 Sep - First release candidate (rc1).**
* **30 Sep - Swarm idea.** "Pull a version, not a host": the circle as a swarm of holders, like file sharing.
* **30 Sep - Stock-friendly bootloader.** Factory firmware installs over the LoRa-OTA bootloader, and back.

### October 2026

* **1 Oct - Pre-release soak.** A gentler, quieter transfer: send only after a short quiet gap. Soak on three boards.
* **3 Oct - Ready for review.** A clean-up release for community review. First upload of the code to GitHub (private at first).
* **3 Oct - Hardware watchdog.** A hung main loop restarts the node.
* **5 Oct - Remote watchdog setting and fair sharing.** A changed watchdog time applies itself remotely. A slow puller gets a fair share of a holder's airtime.
* **6 Oct - Swarm v1.** Nodes list versions, pull from the best holder and switch holders on failure.
* **6 Oct - Shared stream.** Pullers of the same image share one stream; late joiners fill their gaps afterwards. Built and tested first in a new network simulator.
* **6 Oct - Bootloader rev 8.** The 1 W booster is held off while the bootloader runs. Battery nodes had browned out during updates.
* **8 Oct - Fleet fill.** Every pull also fills empty slots of nodes that hear it. A fleet pull refreshes slots hop by hop and never installs by itself.
* **10-11 Oct - Fleet cancel and better holder choice.** A fleet wave can be cancelled; holder changes are checked against the measured rate.
* **15-16 Oct - Whole test fleet updated.** rc21 rolled out to all nodes on 16 Oct; a 24-hour soak passed with no watchdog resets.
* **25 Oct - External flash idea.** An add-on flash module was evaluated as storage for a rollback copy.
* **Around 29 Oct - Code public on GitHub.**
* **30 Oct - Stricter checks.** A slot counts as valid only after both a CRC and a SHA-256 check. A node keeps serving while it pulls.

### November 2026

* **1 Nov - Heltec V4 port starts.** LoRa-OTA on the ESP32-S3 Heltec V4. A 24-hour soak with power saving on passed on 2 Nov.
* **4 Nov - Push design.** A campaign that asks a whole circle to fetch a new version over several days.
* **5 Nov - Move to MeshCore 1.18.0.** Hardware crypto on, app about 70 KB smaller. The canary showed it crashed at start-up, so it never left the bench.
* **6 Nov - Upgrade tests.** The simulator now also tests an upgrade from the previous release with its saved state.
* **7 Nov - Boot-loop recovery (bootloader rev 9).** The bootloader starts the watchdog before the app. After 3 failed starts in a row it opens Bluetooth recovery by itself. Every normal user install method passed on the canary.
* **7 Nov - Start-up crash fixed (rc24).** Plus a new release check so this class of bug cannot ship again.
* **8 Nov - rc25.** Push campaigns, version names on the MeshCore base, one source for RAK3401 and Heltec V4.
* **10 Nov - Rollback proof of concept.** With an add-on flash module, the bootloader kept a backup of the running app and restored it automatically after 3 failed boots. Passed on the bench board.
* **10 Nov - Delta updates measured.** Patches between versions: about 8 KB and 35 KB, against about 233 KB for a whole image.
* **11 Nov - rc25 passes its canary** on RAK3401 and three Heltec V4s, and is **published on GitHub** as a pre-release with binaries for both boards.
* **11 Nov - Delta updates become priority 1 on the Heltec V4.**
* **12 Nov - Size study.** Optimising for size instead of speed, plus dropping unused USB features: about 57 KB (16 %) smaller, measured. Patches stay the same size.
* **12 Nov - Add-on flash parked.** Rollback will come from two internal slots instead, with no extra hardware.
* **12 Nov - A/B slots proven on hardware.** The bootloader started an app from the second slot on trial. That test app never confirmed itself: after 3 failed trial boots the bootloader went back to the first slot by itself.
* **12 Nov - rc26 on the canary.** The transition build (about 299 KB) runs on the bench board; the first board started fetching it over LoRa.
* **12 Nov - Delta decoder done.** Tested against thousands of damaged patches: none wrote outside its area.

---

## What is next

1. rc26 soak on the canary boards, then the fleet.
2. Bootloader rev 11, one node at a time: two app slots, trial boot, automatic rollback.
3. rc27: the first update installed on trial, then delta updates over LoRa.
4. Delta updates on the Heltec V4.
