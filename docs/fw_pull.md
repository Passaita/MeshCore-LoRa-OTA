# LoRa firmware pull (RAK3401 repeaters)

A repeater can **pull a new firmware image over LoRa from a neighbour**, verify it, and flash itself – no site visit,
no cable. Since v1.17.12 every `fw` command below can be typed on the serial console **or** sent from a phone app / companion
through the node's authenticated remote-admin session (earlier builds accepted them on the serial console only). This page is the
operator's guide. The bootloader half is described in
`docs/FWPULL_BOOTLOADER.md` of the bootloader repository, which also covers **onboarding a node that still runs a stock bootloader**
(two BLE uploads, scripted by `tools/hw_bench/onboard.py`). Related notes here: `fw_pull_porting.md`, `fw_pull_security.md`,
`fw_pull_bootloader_update_design.md`.

Commands that reset the node (`start ota`, `reboot`, `clkreboot`, `poweroff`) answer first and act 3 s later (since v1.17.13); before,
the reset happened inside the command and no reply ever reached the console or the phone, so `start ota` looked like it had done
nothing. **A node still on v1.17.12 does not have this fix** - any of those four commands reset it immediately with no reply, and
`start ota` specifically drops it out of the mesh into the bootloader's BLE-only DFU mode (`3401_DFU`), which looks like the node
"hung" and stopped answering. Update to v1.17.13 or later before relying on those commands remotely. `start ota` now says what to
look for in the DFU app: the bootloader advertises `3401_DFU` from the chip's BLE address + 1.

Status: working end to end on three bench boards (RAK3401 and a RAK4630-core board, 2026-09) with a real 232 KB image, including reboots,
server outages, simultaneous pullers and multi-hop. Needs the **fw-pull bootloader** (branch
`fwota-on-2.3-BP1.4`, based on OTAFIX 2.3) – the stock bootloader cannot apply a pulled image.

## The idea in one minute

```
 node A (has firmware)                          node B (wants it)
 ─────────────────────                          ─────────────────
 slot holds a valid image  ──  auto "serving"
                                            fw list      → finds A
                                            fw pull <A>  → asks for the ADMIN PASSWORD
                              ◄── chunks (160 B each, CRC + SHA-256 checked)
                                            slot now valid → B reboots itself
                                            bootloader unpacks the slot over the app, boots it
                                            B now holds the image too → B serves it on to C
```

* Every node keeps **one staging slot** (flash 0x96000–0xEC000, 344 KiB) that holds the LZMA-compressed image.
* The slot is only ever **read** by the bootloader, so a node can keep serving the image to others and can re-apply it
  after a power cut. There is no A/B and no rollback: test an image on a bench node before you release it.
* **A node offers an image only after it has installed it itself** (auto-serve). Rule: the slot is valid *and* the
  application region holds exactly the slot's image. So a node that has just pulled an image does **not** serve it while
  it is waiting for its apply; it starts serving by itself once the bootloader has applied it and the new app is running
  (about 15–30 s after the reboot). An image the bootloader refused (no tag, bad decode) or whose apply keeps being
  interrupted is never served. `fw serve force` is a bench-only override that ignores this rule (not persistent).

## Everyday use (serial console or remote-admin session)

Over a remote session the password has to be on the same line (`fw pull #n <password>`, `fw blupdate #n <password>`): the mesh
carries one message per command, so there is no second line for the `Admin password:` prompt. The command travels inside MeshCore's
encrypted admin message. `fw stage` is the one command that stays serial-only (it takes over the serial stream for the upload).

| Command | What it does |
|---|---|
| `fw list` | Ask "who offers firmware?" for ~20 s: the zero-hop request is repeated every 1-3 s (random) so that a busy server is still found, and one flooded discovery goes out through the repeaters (since v1.17.14) so servers behind relays answer too, with the path. |
| `fw peers [page]` | Show the answers: `#n  ID  name  fw-version  rssi  snr [Nhop]`, 3 per page. `ID` is the first 4 bytes of the peer's public key (8 hex); `Nhop` = reached through N relays (rssi/snr are then those of the last hop). |
| `fw pull <ID, #n or name>` | Prints the peer's name and version and asks **`Admin password:`**. Type it (not echoed). The pull then runs unattended, along the path the discovery found. The name may be a prefix (case-insensitive). |
| `fw pull <ID or #n> <password>` | One-shot form for scripts. |
| `fw pull-status` | Progress: `downloading … 812/1423 chunks (57%) 1.4c/s ETA 7m40s`, `WAITING for server …`, `COMPLETE … \| serving`. The rate and ETA are measured since the download (or its last resume) started; `ETA measuring` for the first ~20 s. |
| `fw pull-abort` | Cancel a pull. |
| `fw serve off` / `fw serve` | Opt out of / back into auto-serving (persistent). `fw serve` only starts if the image is installed here. |
| `fw autoapply on\|off` | Whether a verified pull is applied automatically (default **on**). Turn off only for tests with synthetic data. |
| `fw slot`, `fw verify`, `fw ctl` | Inspect the slot (VALID / CRC match) and the bootloader control page (`last=0` means the last apply succeeded). |
| `fw help` / `fw help <cmd>` | Since v1.17.16: `fw help` lists every subcommand; `fw help pull` (etc.) gives that command's arguments and what it does, one reply per command. |

Typical session on the receiving repeater:

```
> fw list
scanning for peers that offer firmware - wait ~20 s, then type: fw peers
> fw peers
#1 D03C5A97 RAK3401-SERVER-B fw-20 rssi? snr12.75
> fw pull #1
Pull from 'RAK3401-SERVER-B' D03C5A97 (fw-20, 227614 B). Admin password:
> ********
OK - pulling; it runs unattended and applies itself when verified
```

About 19 minutes later (a 220 KB image at ~200 B/s) the node reboots by itself, the bootloader unpacks the image
(≈13 s) and the node comes back on the new version with its name, keys and settings intact.

## Through repeaters (multi-hop)

Stock MeshCore repeaters forward *direct* packets along a path of 1-byte hashes for any payload type, so a pull can cross
them without any change to those repeaters. They never relay a *flooded* raw packet, though, so discovery uses what they do relay: a
channel message. `fw list` sends one flooded text on a fixed LoRa-OTA discovery channel (`LOTA1 <session> <target>`); every serving
node within 4 relays that hears it answers direct along the reversed path the flood collected, and includes that path, so the
puller learns the route. `fw peers` shows such servers with `Nhop`; `fw pull #n` / `fw pull <name>` then uses the route by itself.

While a pull waits for a silent server (a relay disappeared, the server rebooted), the puller asks the mesh for a new path after
60 s, then every 5 minutes doubling to hourly, and switches as soon as an answer arrives; while downloading it asks every 30 minutes
and switches only to a shorter path (`fw pull-status` shows `route updated: N hop(s)`). The discovery target is then the server's id,
so only that node answers.

Paths use MeshCore's own encoding since v1.17.15: 1-, 2- or 3-byte hashes (the node's `path.hash.mode` + 1, and the discovery
flood never uses less than 2 bytes, so two repeaters that share the first byte of their key cannot misroute a pull), up to 6 relays.

Manual override, for a server that did not answer the discovery:

```
> fw route 0C3A,D041     # 2-byte hashes = first two bytes of each relay's public key, in order from here towards the server (max 6)
> fw route 0C            # 1-byte form (the only one a server before v1.17.15 accepts)
> fw pull 5843D43E       # the server's full 8-hex id (it need not appear in fw list)
```

Requests go out direct along the route and carry the reversed route, so the server answers the same way. A typed route wins over a
discovered one until `fw route off` (automatic again). Measured (1 relay, 5 dBm bench): about 0.4 chunk/s against 1.4 direct, i.e. roughly an hour for a full image;
through a real, busy repeater about 0.3 chunk/s with retries. Multi-hop pulls are meant for the rare node out of direct range.

**Compatibility:** a server on v1.17.14 or older drops a request that carries a 2-byte route and ignores a 2-byte discovery flood; direct
pulls work between all versions, and a typed 1-byte `fw route` still reaches an old server. The T-LoRa Pager companion build 3.4 answers
discovery and understands all hash sizes. `fw list` may be run while a pull is waiting for its server.

## Clock and RTC

The clock resets on every reboot. Since v1.17.12 the node saves its clock right before an automatic apply and restores it
(+30 s, forward-only) at the next boot, so its adverts after an update still carry a newer timestamp than before and phones keep
accepting them; earlier builds came back with the build date and needed a `clock sync` from an admin after every update. Any other
reboot still resets the clock. Build with `-D RTC_RV3028_ONLY` (and without `NO_EXTERNAL_RTC`) to use a
RAK12002 (RV-3028, I2C 0x52): it is read at boot and hourly, written on every `time`/`clock sync`, and the clock never steps backwards on a
resync. Nodes without the module keep the software clock, so one binary fits all. A local console `time <epoch>` may set the clock backwards
(to fix a clock that ended up in the future); remote `clock sync` stays forward-only.

## Several nodes pulling at once

Chunks go into the slot where they belong, in any order, and carry the id of their image instead of a session. A puller therefore takes
**any valid chunk of its image it hears, whoever asked for it**: with two nodes pulling from one server, the second joins the ongoing
stream (it is always the same image - a node has one slot), and asks only for the chunks it still lacks. The server stays stateless and never waits
for acknowledgements, so a node with poor reception only delays itself; it re-asks for its missing chunks (a per-window bitmap) at the end. The server also skips a chunk it sent
to another puller a moment ago. Measured on the bench: two pulls of a 1452-chunk image, the second starting 4 minutes late, both finished within
19 minutes of the first start (sequential would be ~34). This works among nodes that hear the server (or the same relay) directly.

## Security model

* **Authentication = the admin password.** The MAC key of a pull is `SHA-256("fwota-v1|" + password)[0..16]`.
  The server derives the key from *its own* admin password, the puller from the password you type. If they differ,
  the server silently ignores every request (counted as `mac_fail` in the server's `fw pull-status`) and the puller
  reports `no answer: peer gone / wrong admin password?`. So the password you type is **the admin password of the serving node** - the puller's own password is irrelevant, and nodes may all have different passwords (you are already logged in as admin on the puller; the serving node asks you to authorise the transfer). Only the bench form `fw pull *` / `fw pull fw-N` uses the puller's own password.
  The admin password field holds at most 15 characters.
* Every message carries an 8-byte MAC (truncated SHA-256 over key + message). Every chunk also has a CRC-16; the whole
  payload is checked with CRC-32 **and** SHA-256 before the slot is marked valid.
* `fw list` answers are unauthenticated (name, version and size only – the same things a MeshCore advert already shows).
* The bootloader accepts only images carrying the **firmware tag** (`FWPL`, board `RAK3401`) so stock or foreign
  firmware cannot be flashed by accident. This guards against mistakes, not against a determined attacker.
* Known gap: the MAC is only 8 bytes and there is no per-image signature yet (backlog: signed manifests).

## What happens when things go wrong

The pull is designed to run for hours in a hostile RF environment without anybody watching.

| Situation | Behaviour |
|---|---|
| Lost / corrupted chunks | Re-requested (only the missing ones), adaptive window 1–8 chunks. |
| **No answer yet (bad weather, server off, out of reach)** | A new pull keeps trying after its first 12 quick attempts, with a growing delay (up to 60 s), for up to a week of **continuous silence**. There is no limit on attempts, a slow pull that keeps receiving chunks is never cut off, and the request itself survives a reboot. `fw pull-abort` ends it. |
| **Server disappears or reboots** | Puller keeps everything it has, goes to `WAITING`, re-asks with exponential back-off (4 s → 60 s) for up to a week. When the *same* image is offered again it continues from the first missing chunk. |
| **Puller reboots / loses power** | Every received chunk is already in the slot; after boot the puller rebuilds the map of present chunks from flash, keeps the stored key, and carries on by itself. |
| Server now offers a **different version** | The partial download is useless: the pull restarts from scratch with the new image. This is the only thing that interrupts a pull. |
| Wrong password | The serving node tells the puller; the pull aborts within ~10 s with `wrong admin password for the serving node`, nothing is stored. (The notice is unauthenticated - the key is what is wrong - so someone nearby could fake it to abort a pull.) |
| CRC/SHA mismatch at the end | Restarts (twice), then fails without touching the running app. |
| Puller already has the offered image | `already have this image` – nothing is written. |
| Power cut while the bootloader applies the image | The slot is intact, the next boot applies it again (idempotent). Tested with 12 random resets and 8 real power pulls. |

The running app is never overwritten before the complete image has been verified; the only point of no return is the
bootloader's apply step (~13 s).

## Loading an image onto a first node

The first node of a fleet gets the image over USB (the second and later nodes pull it over LoRa):

```
python3 tools/fw_ota_stage.py firmware.bin /dev/serial/by-id/<board> --fw-version <N>
```

It LZMA-compresses the image (dictionary 8 KiB) and writes it into the slot; the node then starts serving it by itself.
`--fw-version` is any number that increases with each release; peers use it (with the payload CRC) to decide whether
two images are the same. The `.bin` must be a **tagged** build (see below) – untagged images are refused.

Check with `fw slot` (VALID) and `fw verify` (MATCH).

## Building an image that the bootloader accepts

* Build the `RAK_3401_repeater_poc` environment. It uses `boards/nrf52840_s140_v6_fwota.ld` (which pulls in
  `boards/nrf52_common_fwota.ld`) to place the 16-byte tag right after the vector table, and to stop the app at
  0x96000 so it never overlaps the slot.
* Do **not** build the app with `NRF52_POWER_MANAGEMENT`: the bench tests rely on a marker in `GPREGRET2`.
* The app has **~84 KiB of flash left** (372,528 of 458,752 bytes, 81.2 % as of v1.17.16-help) since the 2026-09-19 resize (option C).
* Version strings: `FIRMWARE_VERSION` in `examples/simple_repeater/MyMesh.h` (the `-fwpull` suffix comes from the
  build environment).

## Two-board bench test (what was actually run)

See `otafix/tools/hw_bench/LORA_TWO_BOARD_TEST.md` in the bootloader repository. Summary of what was proven:

* Real 221 KB LoRa transfer, 1385/1385 chunks, 0 retries, ~19 min; apply 15.5 s; new version running; settings kept.
* Server rebooted during the pull; server silent for 75 s and then back (resumed at chunk ~150); puller rebooted
  during the pull (resumed by itself); server switched to a different image (restart).
* Wrong admin password rejected with 12 MAC failures on the server and zero data sent.

## Space budget (option C, geometry from 2026-09-19; usage as of v1.17.16-help, 2026-09-22)

| Area | Size | Used | Free |
|---|---|---|---|
| Application region (0x26000–0x96000) | 458,752 B (448 KiB) | 372,528 B (81.2 %) | **86,224 B** |
| Staging slot payload (0x97000–0xEC000) | 348,160 B (340 KiB) | varies with the served image (v1.17.16-help LZMA's to ~238 KB) | varies |
| Bootloader (0xF4000–0xFE000, usable) | 38,912 B | 36,768 B (94.5 %) at rev 2 | **2,144 B** |

The LZMA image is ~64 % of the raw app (a larger dictionary than 8 KiB gains under 2 %). A completely full 448 KiB app
(≈ 291 KiB compressed at 0.65) fits the slot with ~14 % margin; a pull whose manifest exceeds the slot is refused.
The bootloader remains the tight area. Changing this geometry needs the bootloader, the app and the linker script to
agree (`check_contract.py` verifies the constants) and, on deployed nodes, a bootloader update first.

## Troubleshooting

* `fw peers` says `no peers` – the server has no valid slot (`fw slot`), has not installed the image yet (`fw serve` explains), has `fw serve off`, or is out of range.
  Distance matters more than antennas at SF7: a pull needs a link with SNR ≳ −5 dB.
* `rssi?` in the list – this radio wrapper reports no RSSI for duty-cycled receive; use the SNR.
* Pull sits in `offering` for a minute then fails – wrong admin password (check the server's `mac_fail`).
* `fw list` (or `fw pull`, `fw blupdate`, `fw stage`) answers `ERR - busy (download/stage in progress)` – a pull is
  still downloading/verifying, or a serial `fw stage` is running. Check `fw pull-status` first; if it is stuck
  (silent server, bad link, or you just want a fresh start), `fw pull-abort` cancels it and clears the saved state,
  then `fw list` works again immediately. Aborting is safe at any point before COMPLETE - nothing has been applied yet.
* After a pull the node does not come back – wait 30 s. A node without a valid app and with a valid slot re-applies it
  by itself; a node with neither waits in BLE OTA (`3401_DFU`, blue LED blinking) for a manual upload.
* A remote session "hangs" right after a command and the node stops answering – see the `start ota`/`reboot`/`clkreboot`/`poweroff`
  note above; on v1.17.12 those reset the node with no reply, and `start ota` leaves it reachable only over local BLE DFU, not the mesh.
* Never halt the CPU over SWD while a board is in BLE OTA (it crashes the SoftDevice).
