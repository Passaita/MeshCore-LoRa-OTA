# LoRa-OTA - changelog

Image numbers (`fw-NNN`) are what `fw peers` shows. Bootloader revisions are listed in the LoRa-OTA-bootloader repository.

| Version | Date | Changes |
|---|---|---|
| **v1.17.28-rc21** | 2026-10-15 | Base: IoTThinks PowerSaving-v17 `b7cc0e6d`. An install keeps the clock (older versions added 30 s per install). Time from the swarm: a node without an RTC takes the time from a circle neighbour that has one (`fw time`); a clock that is ahead is held, never stepped back. Fresh installs start with power saving on, RX power saving balanced and the AGC reset at 60 s. Text fixes. fw-304. |
| v1.17.28-rc20 | 2026-10-11 | A holder move is checked against the measured rate and moved back if slower (`fw route` shows the last change); a session abort is repeated; clearer replies. fw-303. |
| v1.17.28-rc19 | 2026-10-10 | Fleet cancel (`fw pull-abort` stops a fleet pull's fillers, repeated for 30 min); passive fills pause instead of asking for too much; gaps only behind the stream; low-battery Bluetooth warning; fw_ota built for size (31 KB smaller). fw-302. |
| v1.17.28-rc18 | 2026-10-08 | **Fleet fill:** every pull fills empty slots of the nodes that hear it; `fw pull ... fleet` replaces older slot images fleet-wide (hop-by-hop gossip, 48 h window, never installs). Adopt (a stuck pull switches to a nearer holder), local-first gap repair, battery rest, `fw pull ... pause Nh`, `fw slot`. **`fw peers` (scan) and `fw list` (versions) swapped names.** fw-301. |
| v1.17.28-rc16 | 2026-10-07 | Truthful status texts from the first hardware runs of the shared stream; simulator text audit. fw-300. |
| v1.17.28-rc15 | 2026-10-06 | **Shared image stream:** pullers of one image share one stream (listeners, late joiners, gaps after the end from the best holder); expanding search with suppressed answers. Built and tested in the new network simulator first. fw-299. |
| v1.17.28-rc12-14 | 2026-10-06 | **Swarm v1:** pull a VERSION from the best holder (fair share, relay airtime, link, battery), failover between up to 4 holders, announce, `fw minbatt`; live `fw duty` on the puller. fw-296..298. |
| v1.17.28-rc10-11 | 2026-10-05 | Fair duty share between several pulls of one server; a watchdog timeout change applies itself remotely; clearer status texts. fw-294, fw-295. |
| v1.17.28-rc8-9 | 2026-10-03/04 | **Hardware watchdog** (`fw wdt`): a hung node restarts itself within 10 minutes. fw-292, fw-293. |
| v1.17.28-rc7 | 2026-10-03 | Review clean-up: trailing space ignored, clearer texts, old password code removed, constant-time MAC check, bench commands only in the dev build, one release env `RAK_3401_lora_ota`; OFFER_NAK bound to the session. fw-291. |
| v1.17.28-rc6 | 2026-10-01 | Duty rescale: `fw duty 1.0` = the previous 0.3, so the default 0.3 is ~3x gentler. Both ends yield: a LoRa-OTA packet is sent only after 2 s without other traffic (max 20 s wait). fw-290. |
| v1.17.28-rc5 | 2026-09-30 | The quiet gap after each batch grows with the relays (5 s up to 1 relay, 7.5 s at 2, 10 s at 3). fw-289. |
| v1.17.28-rc4 | 2026-09-30 | Serve what is available (the bootloader always; the slot image when installed; empty slot = bootloader only). `fw serve` = status, `fw serve on`. No zero-hop offers once the server is known to be behind relays. `ans Nm ago` in `fw pull-status`. fw-288. |
| v1.17.28-rc3 | 2026-09-30 | A pull of the image a node already runs fills the slot without re-flashing; no flooded answer to a scan from a direct neighbour; a pull's targeted discovery answered directly and flooded; only the kind asked; `fw pull-pause [Nh]` / `fw pull-resume`. |
| v1.17.28-rc2 | 2026-09-29 | Quieter `fw list` (answer first, two direct requests, each scan answered once per server): a scan went from ~27 packets to ~7. |
| v1.17.28-rc1 | 2026-09-29 | **Security v2:** circle passphrase, hidden without it, per-pull session keys, encrypted chunks, no admin password. **`fw pull-ble`** + **`fw apply`**. Quieter stalls (back-off to 5 min; heals once per hour per puller). Hex-checked ids. **Not compatible with v1.17.27 and older.** |
| v1.17.27 | 2026-09-28 | Duty counts the relays (spacing = airtime x (relays+1) / duty), fixed batches of 5 through relays, 5 s quiet gap, one flood budget (4 per rolling hour), default duty 0.3. |
| v1.17.26 | 2026-09-28 | Heal cap, bootloader serve counters, a typed route falls back to discovery after 6 misses, re-discovery back-off. |
| v1.17.25 | 2026-09-28 | LoRa-OTA duty cycle: `fw duty`, `fw pull ... duty x`, agreed per pull; OTA packets queued below mesh traffic. |
| v1.17.24 | 2026-09-27 | Fix: flooded discovery answers were dropped by the receiver (AES padding in the MAC check). |
| v1.17.23 | 2026-09-27 | One-way links: discovery answered by a flood, route pairs (way there / way back), the server heals a broken way back; `fw peers` per host; `fw radio`. |
| v1.17.21-22 | 2026-09-26 | Automatic multi-hop routing (4 scored routes, failover, flood fallback); `fw route`; typed routes with several relays. |
| v1.17.20 | 2026-09-26 | Fix: an unknown `fw` sub-command could freeze the node (reply buffer overflow). |
| v1.17.17-19 | 2026-09-24/25 | Rebased on IoTThinks PowerSaving-v17 `d9c42964`; `ver` shows version + base + build time; switch the source of a running pull, keeping the chunks. |
| v1.17.12-16 | 2026-09-21/22 | `fw` commands over the remote-admin session; discovery through stock repeaters; 2/3-byte path hashes; `fw help`. |
| v1.17.2-11 | 2026-09-19/20 | First end-to-end LoRa pull, reboot survival, auto-serve, `fw list` / `fw pull`, bootloader over LoRa (`fw blupdate`). |
| v1.17.1 PoC | 2026-09-18 | Custom bootloader + tagged app, first proof of concept. |
