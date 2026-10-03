# LoRa-OTA - security design, limitations and review guide

This is **security v2** (v1.17.28). Reviewers: section 6 lists where to look first. Please report findings privately to the
maintainers while the repository is private.

## 1. What is protected, and against whom

**Trust boundary: the circle.** Every node of one operator's fleet is given the same passphrase. Circle members trust each other
completely; everybody else is an outsider.

| Goal | How |
|---|---|
| Outsiders cannot find, pull from or serve to circle nodes | discovery runs on a group channel derived from the circle key; every discovery answer and offer is MAC'd with a circle-derived key; a node without the passphrase is hidden and does not take part at all |
| A puller only completes a session with the real holder of the server identity | session keys need MeshCore's ECDH of the two node identities (X25519 of the Ed25519 keys, as for logins and direct messages) |
| The image is confidential on the air | chunks are encrypted with AES-128-CTR under a per-session key (no airtime cost) |
| Packets cannot be forged or altered | 8-byte truncated HMAC-SHA256 on every LoRa-OTA packet; CRC per chunk; SHA-256 over the whole image, compared with the authenticated manifest |
| Recorded sessions cannot be replayed | both ends contribute a fresh 8-byte nonce to the session keys |
| The admin password is not exposed | it is not used by LoRa-OTA at all (v1 derived the MAC key from it - offline-guessable, removed) |
| The channel stays usable | duty cap, lowest queue priority, yield to other traffic, 4 automatic floods per hour per node |

**Not protected (by design or out of scope):**
* **A circle member serving a bad image.** Images are **not signed** - a deliberate decision so that any node can always be moved
  back to stock firmware. Anyone who knows the passphrase (or controls a circle node) can offer an image to the circle, and a node
  that pulls it installs it. Pulls are always started by the puller (a server cannot push), but `fw autoapply` is on by default.
* **The remote-admin session.** Anyone with a node's MeshCore admin password can run every `fw` command on it, including
  `fw passphrase`, `fw serve force` or a pull from a node of their choice. That is MeshCore's own authentication, unchanged.
* **Physical access.** The circle key is stored in the node's flash file system next to MeshCore's private identity key; SWD or a
  flash dump reveals both. The Bluetooth DFU of the bootloader (stock OTAFIX behaviour) is unauthenticated once a node is in DFU mode.
* **Metadata.** Packet timing, sizes and the fact that two nodes exchange LoRa-OTA traffic are visible.

## 2. Keys
* **Circle key C** = PBKDF2-HMAC-SHA256(passphrase, salt "LoRa-OTA circle v1", 50,000 iterations, 32 bytes). Computed in the
  background (~20 s on the nRF52840). Only C is stored (`/fwota.circle`), never the phrase. The fingerprint shown by `fw passphrase`
  is the first 4 bytes of SHA-256(C).
* **Derived from C:** discovery channel secret = HMAC(C, "lota2-disc") (a MeshCore group channel); list key = HMAC(C, "lota2-list")[0..15];
  offer key = HMAC(C, "lota2-offer")[0..15].
* **Session keys per pull:** HMAC(C, "lota2-sess" | ECDH(puller, server) | session id | puller nonce | server nonce) -> a 16-byte MAC
  key and a 16-byte encryption key.
* **Chunks:** AES-128-CTR, counter block = session | chunk index | block number; ciphertext length = plaintext length.
* **MACs:** HMAC-SHA256 truncated to 8 bytes.

Implementation: `src/helpers/fw_ota/FwOtaTransfer.cpp` - `circleKeys`, `sessionKeys`, `deriveKey`, `macFill`/`macFillKey`/`macOk`,
the CTR helper, `cmdPassphrase`/`passphraseStep`; MeshCore hooks in `examples/simple_repeater/MyMesh.cpp` (ECDH via MeshCore's
identity, the group-channel lookup and receive path).

## 3. Messages
* LIST_REQ / LIST_RSP / discovery / heal notices: on the circle's group channel (MeshCore channel encryption) plus our MAC with the
  list key. LIST_RSP carries the server's full 32-byte public key (needed for ECDH).
* OFFER_REQ: duty, the session it replaces, the puller's public key and nonce, route back, name; MAC with the offer key.
* OFFER_ACK: agreed duty, server public key and nonce, the image manifest (lengths, CRC-32, SHA-256); MAC with the session MAC key.
* CHUNK_REQ / CHUNK / ABORT: session MAC key; chunk data encrypted.

## 4. Known limitations and open questions (please weigh in)
1. **Offline guessing of the passphrase.** One captured discovery answer lets an attacker test passphrase guesses offline at the cost
   of 50,000 PBKDF2 iterations per guess. A short or common phrase can be found. Use a long random phrase (4+ random words).
2. **Fixed salt.** The PBKDF2 salt is the same for every circle, so a precomputed table of common phrases works against all circles.
   A per-circle salt would need to be distributed with the phrase - a usability trade-off.
3. **Truncated 8-byte MACs.** Chosen for airtime; forgery needs ~2^64 attempts per packet over a slow radio channel. Comments welcome.
4. **No anti-rollback.** A node installs whatever image it is told to pull, including an older one.
5. **`fw pull-ble`** opens a 5-minute Bluetooth window (started by an admin command) in which anyone in Bluetooth range could upload a
   slot image. It is checked for internal consistency (board tag, kind, sizes, CRC-32, SHA-256) - not for origin - and is then **served
   to the circle**. It is not installed without `fw apply`.
6. **The fingerprint** (4 bytes of SHA-256(C)) lets anyone who sees it check passphrase guesses offline. Do not post screenshots that show it.
7. **Denial of service.** A jammer or a flood of forged packets can stall a transfer (forged packets cost only a MAC check). The pull
   waits and resumes; it never installs a partial or unverified image.

## 5. Bootloader checks
* An image applied from the slot must carry the RAK3401 LoRa-OTA board tag, the right kind and consistent sizes and CRC-32; the LZMA
  decoder writes only inside the app region.
* Raw Bluetooth/serial app uploads (stock OTAFIX path) are accepted without the tag since rev 3, so stock firmware can be installed;
  Nordic's own package checks are unchanged. The control page and the file system are never written by an app upload.
* No signature check (see section 1).

## 6. Where reviewers should look first
1. **Parsing of received packets** - every `on*` handler in `FwOtaTransfer.cpp` (`onPacket`, `onListReq`, `onListRsp`, `onDiscovery`,
   `onOfferReq`, `onOfferAck`, `onChunkReq`, `onChunk`): lengths, offsets, indices, route lengths, names. These run on packets from the air.
2. **Key derivation and MAC checks** (section 2): MAC computed over the right bytes, checked before any state change, constant-time compare.
3. **CTR counter construction** - uniqueness of (key, counter) across sessions, chunks and resends.
4. **Flash writes**: slot, chunk map, pull state - bounds and wear (a long pull writes the chunk map often).
5. **CLI parsing** (`FwOtaCli.h`, `cmd*` functions): every reply fits 160 bytes; remote commands arrive through MeshCore's admin session.
6. **Bootloader** (`LoRa-OTA-bootloader`: `src/fw_ota/fw_ota_apply.c`, `lib/sdk11/.../dfu_single_bank.c`): header validation, LZMA
   output bounds, behaviour on power loss, the slot upload mode.
7. **Abuse of airtime**: can a node be made to flood or transmit beyond its duty?

Host tests cover much of this (`test/fw_ota_*`, bootloader `tools/fw_ota_hosttest`) - see [TESTING.md](TESTING.md). They were written
by the same people as the code; independent eyes are the point of this review.
