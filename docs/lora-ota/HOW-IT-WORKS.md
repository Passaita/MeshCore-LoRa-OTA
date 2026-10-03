# LoRa-OTA - how it works

Code: `src/helpers/fw_ota/` (transfer, CLI, slot handling, radio diagnostics), small hooks in `examples/simple_repeater/` and
`src/helpers/CommonCLI.*`, linker scripts `boards/*fwota.ld`. Bootloader: the **LoRa-OTA-bootloader** repository
(`src/fw_ota/`, `docs/FWPULL_BOOTLOADER.md`).

## 1. Flash layout (nRF52840, SoftDevice S140 6.1.1)

| Region | Address | Size | Contents |
|---|---|---|---|
| MBR + SoftDevice | 0x00000 - 0x26000 | 152 KiB | Nordic, unchanged |
| Application | 0x26000 - 0x96000 | 448 KiB | the running repeater app (LoRa-OTA builds are linked for this size) |
| **Slot** | 0x96000 - 0xEC000 | 344 KiB | header + LZMA-compressed image (app or bootloader) - what the node serves and applies |
| Control page | 0xEC000 | 4 KiB | apply state, boot attempts, fail count, last result |
| File system | 0xED000 - 0xF4000 | 28 KiB | MeshCore identity and settings, the circle key, pull state |
| Bootloader | 0xF4000 - | | LoRa-OTA bootloader (OTAFIX fork) |

The application cannot overwrite itself, and two 448 KiB copies do not fit. So the app only **downloads** the compressed image into the
slot; the bootloader does the risky part (decompress over the app region, record it, boot it). The image stays in the slot afterwards,
so the node can serve it, and an apply interrupted by a power cut is simply redone at the next boot.

## 2. The image
A `.lota` image = a header (board tag, kind app/bootloader, version number, lengths, CRC-32, SHA-256) + the LZMA-compressed payload
(~240 KB for the app). The bootloader accepts only images whose board tag matches; the app checks the SHA-256 of the complete payload
against the **authenticated** manifest before it marks anything pending.

## 3. Discovery
* `fw list` sends a LIST_REQ zero-hop to the direct neighbours (twice) and one discovery flood on the circle's private group channel
  (secret derived from the circle key). Answers (LIST_RSP) carry the server's id and full public key, the image kind, version, sizes,
  name and the route the answer took, MAC'd with the circle's list key.
* Each server answers a given scan once. A direct neighbour answers only directly (no flood). A pull's targeted discovery
  (`@<id>`) is answered directly and flooded, and only for the kind asked (app or bootloader).
* A flooded answer teaches the puller a way back as well as a way there (radio links are often one-way).

## 4. The pull
1. **Offer** (OFFER_REQ): the puller sends its public key, a fresh nonce, the duty it asks for, the route back and the session it replaces.
2. **Manifest** (OFFER_ACK): the server answers with its nonce, the agreed duty (`min` of both) and the image manifest. Both ends now
   derive the session keys (see [SECURITY.md](SECURITY.md)).
3. **Windows:** the puller requests chunks in windows (1-8 chunks, adaptive when direct; a fixed batch of 5 through relays);
   the server sends them encrypted. Missing chunks are asked for again. A chunk map in flash lets a pull survive reboots of either end.
4. **Verify and install:** CRC per chunk, CRC-32 and SHA-256 over the whole image, then `fw pending` + restart (unless
   `fw autoapply off`, or the node already runs that image).

## 5. Pacing - leaving the channel to the mesh
* **Duty:** chunk spacing = airtime x (relays + 1) / channel share, where the channel share is `fw duty x 30 %` (rc6: 1.0 = 30 %,
  default 0.3 = 9 %). The relays' repeats are counted, so a longer route goes slower rather than filling the channel.
* **Quiet gap:** after each batch the server waits max(5 s, 2.5 s x (relays + 1)), so replies and other traffic get through every hop.
* **Yield (rc6):** both ends send a LoRa-OTA packet only after 2 s without any other packet heard (at most 20 s of waiting).
* LoRa-OTA packets are queued at the lowest priority, behind all mesh traffic.
* **Flood budget:** at most 4 automatic floods (discoveries, flooded answers, heals) per rolling hour per node.

## 6. Routes and failure handling
* Up to 4 route **pairs** (way there / way back) per pull, scored by answers and reply time; 2 unanswered requests on a route -> the
  next best; all failing -> a new discovery.
* **Heal:** a server that sees a puller's requests but no progress sends one flooded answer so the puller learns a working way back
  (at most once per hour per puller without progress).
* **Waiting:** a silent server is asked again with back-off (to 5 min), re-discovered at 1, 6, 16, 36, 76 min, then hourly; a week
  of silence ends the pull. `fw pull-pause` stops all sending.

## 7. Bootloader side (summary)
* Applies a pending slot image: checks the header (board tag, kind, sizes, CRC), decompresses LZMA over the app region, verifies,
  records the result in the control page, boots. Power cuts at any flash step are covered by the host test (every flash operation
  interrupted in turn).
* A bootloader image in the slot is copied by the MBR's own bootloader-update mechanism; the app is kept.
* Bluetooth DFU (OTAFIX) is unchanged, plus: an upload mode that writes **into the slot** (`fw pull-ble`, rev 5), a 5-minute inactivity
  timeout, triple-click RST for Bluetooth DFU (rev 4), stock apps up to 792 KiB (rev 6), and a slot that holds exactly the uploaded app
  is kept (rev 7).
