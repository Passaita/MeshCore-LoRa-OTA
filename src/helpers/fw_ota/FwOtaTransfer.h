/*
 * fw_ota - LoRa firmware transfer between nodes (server / puller).
 *
 * Transport: PAYLOAD_TYPE_RAW_CUSTOM, zero-hop (direct RF neighbour), with our
 * own 8-byte keyed-hash MAC (shared fw_ota key). One node "serves" a staged
 * slot; another "pulls" it into one of its own slots and verifies SHA-256.
 *
 * Both roles live in one build. Drives itself from MyMesh::loop() (tick) and
 * MyMesh::onRawDataRecv() (onPacket). Non-blocking.
 *
 * Guarded by -D FW_OTA.
 */
#pragma once

#include <Arduino.h>
#include <Mesh.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif
#include <helpers/IdentityStore.h>   // brings in the FILESYSTEM / File types

#include "FwOtaSlots.h"
#include "ChunkMap.h"
#include "BlImage.h"

namespace fw_ota {

// ---- wire protocol --------------------------------------------------------
static const uint8_t  XFER_MAGIC   = 0xF7;
// 160 B fills a CHUNK message to 180 B - close to MAX_PACKET_PAYLOAD (184) as
// this framing allows, vs. 128 B/chunk wasting ~36 B (~20%) of every packet.
static const uint16_t XFER_CHUNK    = 160;         // payload bytes per chunk
static const uint8_t  XFER_WINDOW   = 8;           // hard ceiling on chunks per burst
static const uint8_t  XFER_WIN_MIN  = 1;
static const uint8_t  XFER_WIN_INIT = 4;           // starting guess; adapts from here
static const uint32_t XFER_MAC_LEN  = 8;

enum XferMsg : uint8_t {
  MSG_OFFER_REQ = 1,   // puller -> server : {name}
  MSG_OFFER_ACK = 2,   // server -> puller : manifest
  MSG_OFFER_NAK = 3,   // server -> puller : {reason}
  MSG_CHUNK_REQ = 4,   // puller -> server : {start_idx, count}
  MSG_CHUNK     = 5,    // server -> puller : {idx, data[], }
  MSG_ABORT     = 7,
  MSG_LIST_REQ  = 8,   // anyone -> all  : "who offers firmware?" (public discovery key, no secret)
  MSG_LIST_RSP  = 9,   // server -> asker: {id4, fw_version, payload_len, name16}
  // 10 = MSG_BAD_AUTH of v1.17.27 and older ("wrong admin password"): no longer sent or handled
  MSG_OFFER_REQ2 = 11, // (v1.17.25) puller -> server : {duty, replaced session, route back, name} - OFFER_REQ + the duty the pull asks for
  MSG_CHUNK_IMG = 13,  // (rc15) server -> all pullers of an image: {image tag4, idx, clen, req seq, data (image key)} - the SHARED stream
  MSG_PACE      = 12,  // (rc11, C29) server -> puller : {session, request seq, duty} - its share changed (fair share), from its next window
  MSG_FLEET_ANN = 14,  // (rc17) puller -> along its route (heard by every node near a hop): {tag, mode, manifest, server id} (image key MAC)
  MSG_GAP_REQ   = 15,  // (rc17) zero-hop: {tag, seq, blocks of missing chunks} - the neighbours that hold them repair (image key MAC)
  MSG_CLAIM     = 16,  // (study C/D) "I miss the most: I pull next": {tag, have, id} (image key MAC); zero-hop, or flooded as LOTA1V
  MSG_SUMM      = 17,  // (study E) zero-hop trickle summary: {tag, have, n} (image key MAC)
  MSG_TIME_REQ  = 18,  // (rc21, C57) zero-hop, node without an RTC: {nonce4} (list key MAC) on the discovery channel as LOTA1Q
  MSG_TIME_RSP  = 19,  // (rc21, C57) zero-hop, node with a valid RTC: {nonce4, time4, id4} (list key MAC) as LOTA1T
};
// (rc17, owner 2026-10-08, SWARM-DESIGN 9.2/9.3) fleet fill: mode 1 (every pull) = nodes with an EMPTY slot fill from the stream they hear;
// mode 2 ('fw pull ... fleet') = nodes also REPLACE an older slot image. Never an older/equal image, never installed (fw apply per node).
static const uint8_t FLEET_FILL_EMPTY = 1, FLEET_REPLACE = 2, FLEET_HOLDER = 0x40;   // (study) holder flag
static const uint8_t FLEET_CANCEL = 0x20;          // (rc19, C42) the requester aborted its fleet pull: listeners pause (older firmware drops the mode)
static const uint8_t GAP_BLK_CHUNKS = 64;          // a GAP_REQ block: [block index][64-bit map of its chunks]
static const uint8_t GAP_MAX_BLKS   = 4;           // blocks per request
static const uint8_t GAP_REP_CAP    = 64;          // (study) array size; g_tune.gap_max = chunks asked (and repaired) per round

// (algorithm study, owner 2026-10-08) the gap-fill strategies compared in the simulator. A = rc17 as built (local repair, then each node
// alone along the path); C = owner's inventory (collectors stay quiet; when the stream ends the node missing the most claims and pulls
// next, everyone listens); D = A's local repair, then C's election instead of solo pulls; E = trickle gossip (zero-hop summaries tell
// neighbours who has more; repair hop by hop; the path only after a long wait). A release keeps the winner as constants.
enum SwarmAlgo : uint8_t { ALGO_A = 0, ALGO_C = 1, ALGO_D = 2, ALGO_E = 3 };
// rc18 defaults = the study's winner G (docs/GAPFILL-ALGO-STUDY-20261008.md): trickle gossip, holder + source spread for fleet pulls,
// adopt, newq, battery rest, 48 h gossip window. A/C/D stay selectable for the simulator study.
struct SwarmTune {
  uint8_t  algo = ALGO_E;
  uint8_t  gap_max = 24;                 // chunks per gap round (<= GAP_REP_CAP)
  uint32_t gap_round_ms = 20000;         // a gap round waits this + 2 s per chunk asked
  uint8_t  gap_dry_max = 2;              // rounds without progress before the next step
  uint32_t rep_snr_ms = 40;              // repair delay per 0.25 dB below 10 dB SNR (best link first)
  uint32_t elect_span_ms = 120000;       // C/D: claim delay = have/n x this (fewest chunks first)
  uint32_t elect_settle_ms = 45000;      // C/D: no better claim heard this long after ours -> we pull
  uint32_t elect_defer_ms = 1200000;     // C/D: deferred to another claim; its stream not heard this long -> a new inventory
  uint8_t  elect_rounds = 3;             // C/D: inventory rounds before "waiting for a manual wave"
  bool     claim_flood = true;           // C/D: the claim floods (one puller swarm-wide) or goes zero-hop (one per area)
  uint32_t tr_imin_ms = 60000, tr_imax_ms = 1800000;   // E: trickle interval
  uint8_t  tr_k = 2;                     // E: summaries heard per interval that silence ours
  uint32_t e_fallback_ms = 2700000;      // E: no progress this long -> along the path
  uint32_t gossip_life_ms = 172800000;   // E: a holder gossips this long after the last neighbour that missed chunks (48 h: a node
                                         //   that was CRITICAL during the stream gets the image when the sun is back)
  bool     batt_rest = true;             // all: a CRITICAL node sends no requests / claims / summaries until it recovers
  bool     req_adopt = true;             // all: a pull by id adopts the image after the first OFFER_ACK (holder search, moves, failover)
  uint32_t newq_ms = 900000;             // all: a collector waits for the requester only while the stream brought it a NEW chunk this
                                         //   recently (0 = rc17: as long as the requester pulls)
  bool     e_spread = true;              // E: a holder's summary is the image's announce (zero-hop): older neighbours fill hop by hop
  bool     src_spread = true;            // E: the stream's server gossips as a holder from the first FLEET offer (not only after a completion)
  bool     move_eval = true;             // (rc20, C48) a move is checked against the measured rate (false = rc19)
};
#ifdef FW_OTA_STUDY
extern SwarmTune g_tune;                 // the simulator varies the parameters
#else
static constexpr SwarmTune g_tune{};     // (rc19) the firmware: constants - the other algorithms' branches are compiled out
#endif

// ---- duty (v1.17.25, A10): the share of airtime LoRa-OTA may use, in percent (0.1-1.0 on the CLI) ----------------------------------------
// Negotiated ONCE per start / restart of a pull: OFFER_REQ2 carries the duty the pull asks for, the OFFER_ACK the agreed one
// (= min(asked, what the server gives)); both ends then work at the same pace (chunk spacing = chunk airtime / duty), so the puller never
// times out on chunks the server is still pacing out. OTA packets are always queued below all mesh traffic (OTA_TX_PRIORITY).
static const uint8_t  DUTY_DEFAULT_PCT = 30;   // (v1.17.27, user: 0.3 of the channel, relays included; v1.17.25/26: 0.5 of the server's airtime)
static const uint8_t  DUTY_MIN_PCT     = 10;
static const uint8_t  OTA_TX_PRIORITY  = 200;   // MeshCore: 0 = routed, 1 = own floods, relayed floods = their hop count (up to 64): OTA after all of them

// Routes (the relays between puller and server) use MeshCore's own path encoding: one length byte = (hash size - 1) << 6 | hop count,
// followed by hop count x hash size bytes; hash size 1, 2 or 3 (a node's `path.hash.mode` + 1). Nodes before v1.17.15 understand
// 1-byte routes only (they drop a request whose length byte is > 4).
static const uint8_t MAX_HOPS        = 6;    // relays between puller and server
static const uint8_t MAX_ROUTE_BYTES = 12;   // 6 hops x 2-byte hashes, or 4 hops x 3-byte, or 6 x 1-byte
static inline uint8_t rtSize(uint8_t enc)  { return (enc >> 6) + 1; }
static inline uint8_t rtCount(uint8_t enc) { return enc & 63; }
static inline uint8_t rtBytes(uint8_t enc) { return rtCount(enc) * rtSize(enc); }
static inline bool    rtValid(uint8_t enc) { return rtSize(enc) <= 3 && rtCount(enc) <= MAX_HOPS && rtBytes(enc) <= MAX_ROUTE_BYTES; }
static inline void    rtReverse(uint8_t* dst, const uint8_t* src, uint8_t enc) {   // same hops, opposite direction
  uint8_t s = rtSize(enc), c = rtCount(enc);
  for (uint8_t i = 0; i < c; i++) memcpy(dst + i * s, src + (c - 1 - i) * s, s);
}

// a peer that answered `fw list` (zero-hop LIST_REQ, or the flooded discovery that stock repeaters relay)
struct PeerInfo {
  uint8_t  id[4];        // first 4 bytes of the peer's public key
  char     name[17];
  uint32_t fw_version;
  uint32_t payload_len;
  int16_t  rssi;         // dBm (of the last hop)
  int8_t   snr4;         // SNR in quarter dB
  uint8_t  kind;         // KIND_APP | KIND_BOOTLOADER (fw_version is then the bootloader revision)
  uint8_t  route[MAX_ROUTE_BYTES]; // relays towards the peer, learnt from the path the discovery flood took (empty = direct neighbour)
  uint8_t  route_len;       // encoded (rtSize/rtCount); 0 = direct
  // (v1.17.23) the way BACK from the peer, learnt from the path its flooded answer took - links are not always symmetric (2026-09-27:
  // a relay reached board 1, board 1 did not reach that relay). Absent = the reverse of route.
  uint8_t  ret[MAX_ROUTE_BYTES];
  uint8_t  ret_len;
  bool     has_ret;
  bool     used;
  // (swarm v1) what the holder told about its offer: the image identity (first 8 bytes of its SHA-256), its duty, how many pulls it
  // serves now and its power state (0 unknown, 1 OK, 2 LOW, 3 CRITICAL) - absent (swarm = false) from servers before swarm v1
  uint8_t  digest[8];
  uint8_t  sduty, spulls, spower;
  bool     swarm;
};
static const uint8_t MAX_PEERS = 8;
static const uint8_t SWARM_BLK = 8 + 1 + 1 + 1;   // digest, duty, pulls served, power state - after the route of a LIST_RSP (kind bit 7 set)
static const uint8_t KIND_SWARM = 0x80;           // LIST_RSP kind byte: the swarm block follows the route
static const uint8_t SWARM_HOLDERS = 4;           // a pull by image remembers up to 4 holders (failover)
// (swarm v1) one image of the circle, as 'fw peers' lists it: its holders grouped
struct ImgInfo { uint8_t kind; uint32_t ver; uint8_t digest[8]; uint8_t n; uint8_t minhops; int8_t best; bool swarm; };

// ---- transfer state (persisted to /fwota.state for resume) --------------
// (v1.17.28) server: a scan / discovery session it answered lately - direct answers given, flooded answer given
struct AnsRec { uint32_t sess; uint32_t ms; uint8_t direct; uint8_t flooded; };

struct XferManifest {
  char     name[32];
  uint32_t payload_len;
  uint32_t raw_len;
  uint32_t payload_crc32;
  uint32_t raw_crc32;
  uint32_t fw_version;
  uint16_t compression;
  uint16_t chunk_size;
  uint32_t n_chunks;
  uint8_t  sha256[32];
  uint8_t  lzma_props[5];   // LZMA raw-stream props (lc/lp/pb + dict size); the bootloader needs them to decode.
                            // Sent at the END of OFFER_ACK; a pull that omits them cannot be applied (found on the first
                            // end-to-end LoRa pull, 2026-09-19: the slot header was written with zeroed props).
  uint8_t  kind;            // KIND_APP: LZMA app image for the slot | KIND_BOOTLOADER: the sender's raw bootloader (fw_version = its revision)
  uint8_t  pad[3];
  uint32_t dst_off;         // puller only (not on the wire): where in the slot payload area the image is stored (a bootloader goes to the
                            // tail so the app copy in the slot survives; 0 for an app)
};
static const uint8_t KIND_APP = 0, KIND_BOOTLOADER = 1;

enum XferPhase : uint8_t {
  PH_IDLE = 0, PH_OFFERING, PH_DOWNLOADING, PH_VERIFYING, PH_COMPLETE, PH_FAILED,
};

// Floods leave through the host so they carry the node's default region (MyMesh::sendFloodScoped); without a host: unscoped.
struct FloodHost {
  virtual void floodScoped(mesh::Packet* pkt, uint32_t delay_ms, uint8_t path_hash_size) = 0;
  virtual bool regionSet() const = 0;   // a default region is configured (floods are scoped)
  // (v1.17.25) a direct / zero-hop OTA packet queued at OTA_TX_PRIORITY (below every mesh packet); false = not supported (sent as before)
  virtual bool sendOta(mesh::Packet* pkt, const uint8_t* path, uint8_t path_len, uint32_t delay_ms) { return false; }
  virtual uint32_t airtimeMs(uint32_t len) { return 0; }   // the radio's airtime estimate for a packet of len bytes (0 = unknown)
  // (v1.17.28, security v2) ECDH of this node's identity with a peer's public key (MeshCore's own login/DM key exchange); false = n/a
  virtual bool sharedSecret(uint8_t* out32, const uint8_t* peer_pub32) { return false; }
  // (rc21, C57) time from a swarm member: 1 = this node has an RTC chip with a valid time (it answers time requests), 0 = none (it asks)
  virtual uint8_t clockSource() { return 0; }
  virtual void clockDiscipline(uint32_t ref) { }   // a trusted better time: behind -> step forward, ahead -> hold (never steps back)
};

// (rc19, size) no default member initialisers: g_xfer is a zeroed global (the simulator value-initialises its nodes), the non-zero
// starting values are set in begin() - the initialisers had cost a 1.2 KB start-up function
class FwOtaTransfer {
public:
  // node_name / pub_key are POINTERS kept for the node's lifetime (the name may change at runtime).
  void begin(mesh::Mesh* mesh, FILESYSTEM* fs, const char* node_name, const uint8_t* pub_key,
             const uint8_t* path_hash_mode = nullptr);   // live pointer to the node's path.hash.mode (discovery floods use at least 2-byte hashes)
  void setFloodHost(FloodHost* h) { _flood_host = h; }
  struct SwarmStats {   // (rc15) RAM counters since boot: what the swarm logic cost in packets (for tests and pull-status)
    uint32_t adverts, advert_floods, answers_sent, answers_suppressed, moves, searches_zero, searches_flood;
    uint32_t fleet_anns, fills, gap_reqs, gap_supp, repairs, repairs_supp, escalations;   // (rc17)
    uint32_t claims, claim_floods, claims_supp, elected, summs, rests;                       // (study)
  } stats;
  void tick();
  void onPacket(mesh::Packet* pkt);            // from MyMesh::onRawDataRecv
  // path discovery through stock repeaters: a GRP_TXT flood on the LoRa-OTA discovery channel (stock repeaters relay channel
  // messages; they never relay RAW_CUSTOM floods). The server answers direct along the reversed path the flood collected, and
  // tells the puller that path, which becomes the route for the pull.
  int  channelMatch(const uint8_t* hash, mesh::GroupChannel* out, int max) const;   // MyMesh::searchChannelsByHash
  void onDiscovery(mesh::Packet* pkt, const uint8_t* data, size_t len);            // MyMesh::onGroupDataRecv
  void noteRssi(float rssi_dbm) { _rx_rssi = (int16_t)rssi_dbm; }   // radio's RSSI of the packet about to be handled

  // CLI
  bool cmdServe(char* reply, bool force = false, bool user = true);   // offer the bootloader + the slot if installed here (force: dev build); user=false: auto-serve probe, never re-enables
  bool cmdServeOff(char* reply);
#ifdef FW_OTA_DEV
  bool cmdPull(const char* name, char* reply);   // dev build only: '*' or a served name
#endif
  bool cmdList(char* reply);                     // discover peers that offer firmware (results print on the serial console)
  bool cmdRoute(const char* arg, char* reply);   // "fw route AA,BB" | off : relays to reach the server (direct-routed through stock repeaters)
  bool cmdPeers(const char* arg, char* reply);
  bool cmdPeersHosts(const char* arg, char* reply);    // (swarm v1) the per-host view ('fw peers hosts [n]', the view of rc1-rc11)
  bool cmdMinBatt(const char* arg, char* reply);       // (swarm v1) 'fw minbatt [mV|off]': below = LOW (last-resort holder), 150 mV below = CRITICAL
  void noteBattery(uint16_t mv) { _batt_mv = mv; }     // (swarm v1) the board's battery, handed over by the main loop every minute   // show the peers found by the last `fw list` (3 per page)
  bool cmdPullPeer(const char* args, char* reply, bool bl = false);
                                                 // "<id|#n>" (v1.17.28: no password - the circle authenticates both ends)
                                                 // bl: "fw blupdate [force] <id|#n>" - the offered BOOTLOADER instead of the app
  void switchSource(const char* target, char* reply);
  void revertSwitch(const char* why);
  bool cmdAutoApply(const char* arg, char* reply); // "on|off": apply a completed pull automatically (default on)
  bool cmdWdt(const char* arg, char* reply);       // (rc8, C23) watchdog state, last reset reason, watchdog resets; 'clear'; dev: 'hang'
  bool cmdTime(const char* arg, char* reply);      // (rc21, C57) 'fw time [ask]': time to the second, its source, the last peer correction
  bool cmdBlInfo(char* reply);                    // the running bootloader: revision, size, CRC, whether it is offered
  bool cmdStatus(char* reply);
  bool cmdAbort(char* reply);
  bool cmdDuty(const char* arg, char* reply);     // "fw duty [0.1-1.0]": show / set the node's LoRa-OTA duty (persistent, default 0.5)
  bool cmdPullBle(char* reply);                   // (v1.17.28) "fw pull-ble": restart into the bootloader's BLE DFU that fills the SLOT (rev 5+)
  bool cmdApply(char* reply);                     // (v1.17.28) "fw apply": install the slot image here (fw pending + reboot)
  // (rc6, C14) MyMesh::logRx: every received packet - (rc17) also the LoRa-OTA ones a relay only forwards (image chunks, fleet announces,
  // gap requests are handled here, wherever on the path they are heard); snr4 = the packet's SNR x4 (127 = unknown)
  void noteRx(uint8_t payload_type, const uint8_t* payload, uint16_t len, int8_t snr4 = 127);
  bool cmdSlot(char* reply);                      // (rc17, C39) "fw slot": what the slot holds, in the form of "fw bl"
  void setNextPullOpts(bool fleet, uint8_t pause_h) { _next_fleet = fleet; _next_pause_h = pause_h; }   // (rc17) 'fw pull ... fleet pause Nh'
  bool cmdServeStatus(char* reply);               // (rc4, C11) "fw serve" = what is offered
  bool cmdPause(const char* arg, char* reply);    // (v1.17.28, C8) "fw pull-pause [Nh]"
  bool cmdResume(char* reply);                    // (v1.17.28, C8) "fw pull-resume"
  bool cmdPassphrase(const char* arg, char* reply);   // (v1.17.28, security v2) "fw passphrase <phrase>|off": the owner's circle
  void setNextPullDuty(uint8_t pct) { _next_pull_duty = pct; }
  static uint8_t parseDuty(const char* s);        // "0.1" .. "1.0" -> percent 10 .. 100, 0 = invalid   // "fw pull ... duty x": overrides the node's duty for the pull it starts (0 = none)

  // Local staging over the serial link: `fw stage <clen> <rlen> <raw_crc32_hex>
  // <fw_version> <lzma_props_hex10>` then <clen> raw bytes. Lets a host script
  // (compress a real .bin, drive this) write a real image into the slot -
  // `fw fill` only ever produces synthetic test data. Binary-safe: the main
  // loop() must stop line-based command parsing while stagingActive() and
  // feed raw bytes to stageFeed() instead (see main.cpp).
  bool     cmdStage(const char* args, char* reply);
  bool     serviceStaging();   // call every loop(): feeds serial bytes to an active `fw stage`; true while one is active
  bool     stagingActive() const { return _staging; }
  bool     slotInUse() const { return _staging || _phase == PH_OFFERING || _phase == PH_DOWNLOADING || _phase == PH_VERIFYING; }   // (rc7) fw erase refused
  uint32_t stageRemaining() const { return _stage_remaining; }
  void     stageFeed(const uint8_t* data, uint32_t len);   // caller must cap len <= stageRemaining()
  bool     stageTakeResult(char* out, size_t outsz);        // true once, when a stage just finished

private:
  mesh::Mesh*       _mesh;
  FILESYSTEM*       _fs;
  const char*       _name_src;   // node name (live pointer)
  const uint8_t*    _pub;        // node public key (live pointer)
  const uint8_t*    _hash_mode;  // node's path.hash.mode (live pointer), nullptr = 2-byte discovery floods
  int16_t           _rx_rssi;
  FloodHost*        _flood_host;
  // (v1.17.28) security v2 (docs/SECURITY-V2-DESIGN.md s.8): LoRa-OTA works only inside the owner's circle (a passphrase on every node).
  // Circle key C (PBKDF2 of the passphrase) -> discovery channel + list key + offer key; per pull a session key from C, the ECDH of the two
  // identities and both nonces -> MAC of every later packet + AES-CTR of the chunk data. No passphrase = hidden (serves/answers/pulls nothing).
  uint8_t           _circle[32];
  bool              _circle_set;
  uint8_t           _list_key[16], _offer_key[16];  // derived from C
  uint8_t           _sess_mac[16], _sess_enc[16];   // puller: the session keys (after the OFFER_ACK)
  bool              _sess_set;
  uint8_t           _nonce[8];                      // puller: this session's nonce (in every OFFER_REQ2 of the session)
  uint32_t          _nonce_sess;
  void              circleKeys();                   // C -> discovery channel, list key, offer key
  // `fw passphrase` runs PBKDF2 in the background (~20 s on the nRF52840) so the repeater keeps relaying meanwhile
  char              _pb_pw[65];
  uint8_t           _pb_u[32], _pb_out[32];
  uint32_t          _pb_i;                      // iterations done (0 = idle)
  void              passphraseStep();
  bool              sessionKeys(const uint8_t* peer_pub, uint32_t sess, const uint8_t* np, const uint8_t* ns, uint8_t* kmac, uint8_t* kenc);
  void              macFillKey(uint8_t* pkt, uint32_t len, const uint8_t* key) const;
  // (v1.17.28) BLE slot fill: `fw pull-ble` -> bootloader rev 5 writes a .lota into the slot -> at boot it becomes a VALID slot, served
  uint32_t          _reset_at_ms;               // `fw pull-ble` / `fw apply`: restart this long after the reply went out
  uint16_t          _wdt_resets;                // (rc8) watchdog resets counted in /fwota.wdt
  bool              _wdt_planned;           // (rc10, C27) the last reset was the planned one of a timeout change
  uint32_t          _wdt_hang_at;               // (rc8, dev build) `fw wdt hang`: freeze the loop from this millis() on
  uint8_t           _reset_magic;               // GPREGRET for that restart (0xA9 = BLE slot fill, 0 = plain reboot)
  void              slotFromLota();                 // at boot: a .lota uploaded over BLE -> checked -> VALID slot (or refused, slot empty)
  bool              bleFilled(const SlotHeader& h) const;   // this slot image came in over BLE (served although not installed here)
  // duty (v1.17.25)
  uint8_t           _duty_pct;                      // this node's duty, as server (ceiling) and as puller (asked unless overridden)
  uint8_t           _next_pull_duty;            // set by the CLI for the pull it is about to start
  uint8_t           _pull_duty_pct;             // per-pull override of the pull in progress (0 = the node's duty); persisted with the pull
  uint8_t           _agreed_pct;                // puller: agreed with the server in its OFFER_ACK (0 = not yet)
  uint8_t           _reagree_from;              // (rc10, C26) puller: the share before a re-offer for a larger one (0 = none running)
  uint32_t          _reagree_at_ms;             //     when to ask again (0 = got what it asked)
  uint32_t          _reagree_ivl_ms;            //     the current interval (15 min doubling to 2 h)
  bool              _reagree_why;           // (rc14) the re-offer is for a changed 'fw duty' (status text)
  uint32_t          _prev_session;              // puller: the session this one replaces (resume, switch, reboot) - its share is freed at the server
  struct DutySess { uint32_t sess; uint8_t pct; uint32_t last_ms; uint32_t next_tx_ms;     // server: the pace agreed per puller session ...
                    uint8_t kmac[16], kenc[16]; uint8_t np[8], ns[8]; bool keyed;           // ... and (v1.17.28) its session keys
                    // (rc6, C14) the window being served, one chunk per quiet moment
                    uint16_t q_idx[XFER_WINDOW]; uint8_t q_n, q_i; bool q_bl, q_paced; uint8_t q_rlen, q_relays; uint8_t q_rpath[MAX_ROUTE_BYTES];
                    uint8_t q_rseq[2]; uint32_t q_sp, q_due, q_wait0;
                    uint16_t top; bool top_bl;                                 // (rc10, C22) the puller's latest request position
                    // (rc11, C29) fair share: what it asked, its fair share now, what its last request said it paces by, whether it
                    // follows PACE (rc11+, flag in its requests), the measured time between its requests (its share is held 3x that),
                    // and a hold of its window while another pull's share is being lowered (the owner's "wait for the next window")
                    uint8_t ask, target, rep; bool capable, known; uint32_t last_req_ms, ivl_ms, q_hold_until;
                    bool img, listen; };   // (rc15) image chunks for it; a listener (no share until it requests)
  static const uint8_t DUTY_SESS_N = 6;   // (rc15) listeners of a shared stream hold a slot (keys) but no share
  DutySess          _dsess[DUTY_SESS_N];
  uint8_t           askedPct() const { return _pull_duty_pct ? _pull_duty_pct : _duty_pct; }
  uint32_t          chunkAirMs(uint8_t route_enc) const;   // airtime of one CHUNK packet along a route of that encoding
  DutySess*         dutySess(uint32_t sess, bool create);
  uint8_t           dutyOthers(uint32_t sess) const;       // sum of the duty held by the OTHER active sessions
  bool              sessActive(const DutySess& d, uint32_t now) const;   // (rc11, C29) holds a share: requested within 3x its own interval
  void              dutyRebalance(uint32_t now);           // (rc11, C29) fair share over the active pulls -> each one's target
  bool              decreasePending(const DutySess* except) const;   // another pull still runs above its lowered share (its window is not over)
  void              sendPace(DutySess& d, uint8_t pct);
  void              onPace(const uint8_t* b, uint32_t len);
  void              sendDone();                            // (rc11) puller: all chunks in - free the server's share at once (an ABORT)
  // ---- swarm v1 ----
  uint16_t          _batt_mv, _minbatt_mv;         // battery now / 'fw minbatt' (0 = not set: power state unknown)
  uint8_t           powerState() const;                   // 0 unknown, 1 OK, 2 LOW, 3 CRITICAL (no serving)
  uint8_t           activePulls() const;                   // pulls this node serves now (fair share sessions holding a share)
  int               imageList(ImgInfo* out, int cap) const;   // the circle's images from the peer table, newest app first
  uint32_t          holderScore(const PeerInfo& p) const; // expected chunks per hour from that holder
  int               bestHolder(uint8_t kind, uint32_t ver, const uint8_t* digest, const uint8_t* skip_id, uint8_t tried) const;
  bool              switchToHolder(int idx, const char* why, bool back = false);   // back: to the holder we came from, on its saved route
  bool              switchToNextHolder(uint32_t now);
  uint32_t          switchAfterMs() const;                 // silence after which a pull by image moves to the next holder
  void              sendAnnounce(bool zero_hop = false, bool bl = false);   // (rc15) the holder advert: "I hold image X" (zero-hop or one flood)
  bool              _img_set;                      // the pull targets an IMAGE (any holder), not one host
  uint8_t           _img_kind;
  uint32_t          _img_ver;
  uint8_t           _img_digest[8];
  uint8_t           _holders[SWARM_HOLDERS][4];
  uint8_t           _nholders;
  uint8_t           _holders_tried;                    // bit i: holder i was tried during this silence
  bool              _ann_pending;                  // announce the image we serve (after a pull, a BLE fill or an install)
  uint32_t          _ann_at_ms;
  uint32_t          _switches;                         // holder switches of this pull (status)
  uint32_t          _stall_cph;                        // chunks per hour up to the last stall (0 = none yet: 6 min)
  // ---- rc15: demand-driven swarm (docs/SWARM-RC15-DESIGN.md) ----
  uint32_t          _last_chunk_ms;                    // (C34) the last new chunk of this pull: silence counts from here
  uint32_t          _last_switch_ms;                   //       ... or from the last holder switch
  // demand: pulls this node overhears (offers carry an image tag; requests / chunks / PACE carry the session in clear)
  struct DemandRec { uint8_t tag[4]; uint32_t last_ms; };   // (rc15) image streams heard around us (image chunks), by image tag
  static const uint8_t DEMAND_N = 4;
  DemandRec         _dem[DEMAND_N];
  void              noteDemand(const uint8_t* b, uint16_t len);
  uint32_t          newSess(uint32_t salt) const;          // (rc15, C35) a session id unique to this node
  void              sendAbort();                           // (rc15) ABORT of our session, sent twice
  void              sendAbortOf(uint32_t sess, const uint8_t* kmac, const uint8_t* route, uint8_t rlen);
  // (rc20, C49) 'fw pull-abort': the session ABORT is repeated after 2 min (and with a fleet cancel after 10 and 30 min too), so a server
  // that missed both copies frees the share at once instead of after its hold (up to 15 min)
  uint32_t          _abt_sess, _abt_at_ms; uint8_t _abt_key[16], _abt_route[MAX_ROUTE_BYTES], _abt_rlen, _abt_n;
  void              abtTick(uint32_t now);
  uint8_t           demandFor(const uint8_t* digest, bool bl, uint32_t now) const;   // live pulls of that image from OTHER servers
  // holder adverts (merged announce): boot (zero-hop) + idle with demand
  uint32_t          _adv_next_ms;                      // earliest next demand advert
  uint32_t          _adv_ivl_ms;                       // quiet time: 30 min, doubling to 2 h while nobody came
  uint32_t          _adv_offers_mark;                  // our offers counter at the last advert (did anybody come?)
  uint8_t           _adv_count;                        // demand adverts in this episode (the 1st zero-hop, then floods)
  bool              _adv_zero;                     // the pending advert goes zero-hop
  void              advertTick(uint32_t now);
  // moving to a faster holder
  int8_t            _move_to;                              // peer index of the candidate (-1 = none)
  uint32_t          _move_at_ms, _move_check_ms, _last_move_ms;
  // (rc20, C48) a move to a "faster" holder is checked against the MEASURED rate: 15 min after it, a holder slower than the one we left
  // (by > 20 %) is left again (back to the previous one) and not moved to for 2 h. The last holder change is shown by 'fw route'.
  uint32_t          _mv_cph, _mv_eval_ms, _mv_bar_until, _sw_ms; uint8_t _mv_from[4], _mv_bar[4], _sw_id[4], _sw_why;
  void              noteSwitch(uint8_t why, const uint8_t* id);
  uint32_t          rateCph(uint32_t now) const;
  void              moveCheck(uint32_t now, bool soon);
  void              moveEval(uint32_t now);
  int               peerIdx(const uint8_t* id, uint8_t kind = 0xFF) const;
  // expanding search: answers delayed by distance, suppressed after 2 for the same image
  struct PendAns { uint32_t sess; uint8_t digest[8]; uint32_t due_ms; uint8_t heard; uint8_t len; uint8_t b[96]; bool used; };
  static const uint8_t PANS_N = 3;
  PendAns           _pans[PANS_N];
  void              answerLater(const uint8_t* r, uint32_t n, uint32_t delay_ms);
  void              answerHeard(const uint8_t* b, uint32_t len);   // another holder's flooded answer: count it for suppression
  void              pendAnsTick(uint32_t now);
  uint32_t          _disc_flood_ms;                    // image search: the flood step after the neighbours' answers (0 = none pending)
  char              _disc_flood_target[20];
  // (rc15) SHARED IMAGE: image-keyed chunks; one requester per image at a time, the others listen and store (owner 2026-10-07)
  bool              _img_ok;                       // our server sends image chunks for this pull (OFFER_ACK flag 0x02)
  uint8_t           _ik_mac[16], _ik_enc[16];  // the image keys of the pull's image
  uint32_t          _stream_ms, _stream_gap_ms, _gap_at_ms;   // a stream of our image (not ours) heard: last time, largest gap
  bool              _listening;
  uint8_t           _heard_tag[4]; uint32_t _heard_tag_ms;   // the last image tag heard on the air (before a manifest: listen offer)
  bool              listening(uint32_t now);
  uint32_t          streamT() const;
  void              onChunkImg(const uint8_t* b, uint32_t len);
  void              storeChunk(uint16_t idx, const uint8_t* data, uint16_t clen, bool own);
  struct ImgKeys { uint8_t dg[8]; uint8_t k[32]; bool set; } _ikc[2];   // server: cached image keys (app, bootloader)
  const uint8_t*    imageKeys(const XferManifest& m, uint8_t slot);   // -> 32 B: MAC key | encryption key
  bool              _fair_server;                  // (rc11) puller: the server shares fairly and sends PACE (flag in its OFFER_ACK)
  // ---- (rc17) fleet fill (SWARM-DESIGN 9.2/9.3) ----
  bool              _next_fleet;                   // CLI: 'fw pull ... fleet' for the pull it is about to start
  uint8_t           _next_pause_h;                     // CLI: 'fw pull ... pause Nh' (C37): set up + paused before the first offer
  uint8_t           _fleet_mode;                           // puller: the mode its announces carry
  uint32_t          _fann_next_ms;                     // puller: the next fleet announce (0 = none due)
  uint16_t          _fann_seq;
  bool              _fill;                         // this pull collects a stream it heard (fleet fill): no session until its gaps
  uint8_t           _fill_mode;                        //   need the path; never installed here
  struct FillCand { bool set; XferManifest m; uint8_t server[4]; uint8_t mode; uint32_t ms; uint16_t have; uint8_t origin[4], req[4]; };
  uint8_t           _spread_tag[4];                    // (rc18) the image (digest) this node may spread: it came by a FLEET pull
  uint8_t           _origin[4], _origin_tag[4];   // (study src_spread) the node this image was first pulled from (RAM only)
  FillCand          _fc;                              // an accepted announce, waiting for the first chunk of its stream
  uint32_t          _fann_heard_ms;                    // the last announce of OUR image's stream (its requester is alive)
  uint16_t          _fann_have;                        //   ... and how far its requester was then (= n_chunks: done)
  uint32_t          _img_heard_ms;                     // the last chunk of our image heard on the air (any sender, new or not)
  bool              streamStillPulled(uint32_t now) const; // the requester of the stream we collect / listen to still pulls
  void              sendFleetAnn();
  uint32_t          buildFann(uint8_t* b, const XferManifest& m, uint8_t mode, const uint8_t* server, uint16_t have, const uint8_t* req);   // (rc19) one layout
  void              imgKey(const uint8_t* sha256, uint8_t k[32]) const;   // (rc19) the image key (MAC | encryption) of an image
  void              useImgKeys(const uint8_t* sha256);    // (rc19) ... as the keys of the stream this node collects (_ik_mac / _ik_enc)
  // (rc19, C42) fleet cancel: the requester repeats it (RAM: packet + route), every node it reaches pauses its fill and passes it on once
  uint8_t           _cxl[89 + XFER_MAC_LEN], _cxl_route[MAX_ROUTE_BYTES], _cxl_rlen, _cxl_n;
  uint32_t          _cxl_at_ms;
  uint8_t           _cxl_seen[8];                  //   the last cancel handled here (image tag + requester)
  void              cxlTick(uint32_t now);
  void              onFleetCancel(const uint8_t* b, uint32_t len);
  // (rc19, C42) a fill that stopped (cancelled, or a plain pull's listener whose puller went quiet): chunks kept, nothing sent; the
  // same image's next stream continues it, a newer one replaces it, 'fw pull-resume' finishes it from the nearest holder
  bool              _fill_paused;
  uint8_t           _fann_req[4];                  //   the requester whose stream this fill follows (a cancel of another is ignored)
  void              pauseFill(const char* why);
  void              dropPausedFill();                   //   the operator starts something else: a paused fill gives way (chunks dropped)
  bool              reqHeard(uint32_t now) const;       //   a plain pull's listener: its requester was heard within the last hour
  uint16_t          _strm_top;                      // (rc19, C44) the highest chunk of the stream heard: gaps are asked only behind it
  void              onFleetAnn(const uint8_t* b, uint32_t len);
  void              startFill();
  bool              fillAllowed(const XferManifest& m, uint8_t mode);
  // gap repair: local first (zero-hop GAP_REQ, the neighbour with the best link repairs, the others hear it), path second
  bool              _gap_phase;
  uint8_t           _gap_round, _gap_dry;
  uint16_t          _gap_have0;
  uint32_t          _gapr_at_ms;                       // next round
  uint32_t          _esc_pick_ms;                      // a collector on the path: pick the NEAREST holder this soon (search answers in)
  uint8_t           _fecho_tag[4]; uint16_t _fecho_seq;   // the stream's server: the last announce it echoed zero-hop
  uint16_t          _gap_seq;
  uint8_t           _gap_cov[ChunkMap::MAX_CHUNKS / 8];   // chunks another node asked for lately (we wait for those repairs)
  uint32_t          _gap_cov_ms;
  void              gapTick(uint32_t now);
  void              gapEscalate(uint32_t now);
  void              onGapReq(const uint8_t* b, uint32_t len, int8_t snr4);
  struct RepJob { uint8_t tag[4]; uint16_t idx[GAP_REP_CAP]; uint8_t n, i, sent; uint32_t due, wait0, sp; bool full; uint16_t seq; };
  RepJob            _rep;                             // repairs this node will send (cancelled per chunk when another sends it)
  void              repTick(uint32_t now);
  void              repHeard(const uint8_t* tag, uint16_t idx);
  bool              holdsChunk(const uint8_t* tag, uint16_t idx, bool* full) const;
  // (study C/D) election: 1 = waiting for our claim time, 2 = claimed (settling), 3 = deferred to another claimer
  uint8_t           _el_phase, _el_rounds;
  uint32_t          _el_at_ms;
  bool              _el_puller;                    // elected: this collector's pull announces its stream like a requester
  void              enterElect(uint32_t now);
  void              electTick(uint32_t now);
  void              sendClaim();
  void              onClaim(const uint8_t* b, uint32_t len);
  // (study E) trickle
  bool              _tr_on, _e_wait, _resting;
  uint8_t           _tr_c, _tr_tag[4];
  uint32_t          _tr_i, _tr_t, _tr_end, _tr_inc_ms, _e_prog_ms;
  void              trickleTick(uint32_t now);
  void              trickleStart(uint32_t now, uint32_t ivl);
  void              sendSumm();
  void              sendHolderAnn();
  void              spreadStart(const uint8_t* digest);
  bool              holderSwitch(const XferManifest& m, const uint8_t* holder, const uint8_t* origin);
  void              onSumm(const uint8_t* b, uint32_t len);
  // discovery
  PeerInfo          _peers[MAX_PEERS];
  bool              _scan_active;
  bool              _scanned;                      // (rc17, C38) a scan ran since boot ('fw list' before one: a hint, nothing sent)
  uint32_t          _scan_end_ms, _scan_resend_ms;
  uint32_t          _scan_start_ms;                    // (v1.17.28) `fw list` transmits from here (after its reply) ...
  bool              _scan_pending;                 //     ... until then nothing is sent
  uint8_t           _scan_resends;                     //     direct requests repeated so far (max SCAN_REQ_RESENDS)
  static const uint8_t ANS_N = 6;                          // (v1.17.28) server: scan sessions answered lately (answerAllowed)
  AnsRec            _ans[ANS_N];
  uint8_t           _ans_wr;
  bool              answerOnce(uint32_t sess, bool flood);
  uint32_t          _last_foreign_ms;                  // (rc6, C14) the last packet heard that was not LoRa-OTA
  bool              _req_pending;                  // (rc6, C14) puller: a window request waiting for a quiet moment
  uint32_t          _req_wait0;
  void              serveTick(uint32_t now);
  void              sendChunk(DutySess& d, uint16_t idx, uint32_t now);
  uint32_t          _last_ans_ms;                      // (rc4, S4) the last discovery answer for the running pull
  struct Txt16 { char c[16]; };
  Txt16             lastAnsText() const;
  void              routeDropDirect(int keep);             // (rc4, S3)
  bool              _paused;                       // (v1.17.28, C8) `fw pull-pause`: the pull sends nothing
  uint32_t          _pause_until_ms;                   //     timed pause end (0 = until `fw pull-resume`)
  uint8_t           _pause_h;
  void              setPaused(bool on, uint8_t hours);
  void              resumePull(const char* why);
  uint32_t          _scan_sess;
  uint16_t          _scan_seq;
  // multi-hop: relays the puller's messages are direct-routed through (persisted with the pull state)
  uint8_t           _route[MAX_ROUTE_BYTES];
  uint8_t           _route_len;          // encoded (rtSize/rtCount); 0 = direct
  bool              _route_manual;   // `fw route ..` typed: a listed peer's discovered route does not replace it
  // (v1.17.23) the return route the requests carry: learnt (flooded discovery answer / heal) or typed (`fw route fwd/ret`); unset = reverse of _route
  uint8_t           _ret[MAX_ROUTE_BYTES];
  uint8_t           _ret_len;
  bool              _ret_set;
  bool              _ret_typed;      // typed by the operator: a heal does not replace it (v1.17.26: unless the typed route failed)
  bool              _typed_failed;   // (v1.17.26, N1) like MeshCore's saved paths: the typed route got TYPED_FAIL_REDISCOVER unanswered
                                             // requests in a row -> flood re-discovery, heals and discovered routes are taken again
  // auto routing (2026-09-26, modelled on meshcore-open's path history): up to MAX_ROUTES routes to the pull's server, scored; the one in
  // use is copied to _route/_route_len (what requests are sent along). Failover to the next-best on unanswered requests, a targeted
  // discovery flood when all fail. RAM only - after a reboot the table restarts from the persisted current route.
  struct RouteRec {
    uint8_t  path[MAX_ROUTE_BYTES];
    uint8_t  enc;                            // encoded length (rtSize/rtCount), 0 = direct
    uint8_t  ret[MAX_ROUTE_BYTES];           // (v1.17.23) route pair: the way back, when known (else the reverse of path)
    uint8_t  ret_enc;
    bool     has_ret;
    bool     used;
    uint16_t ok, fail;                       // requests answered / unanswered along it
    uint16_t rtt_ms;                         // smoothed time from request to first chunk
    uint32_t last_ok_ms;
    float    weight;                         // +0.5 per answer, -0.5 per timeout (0.1 .. 5)
  };
  static const uint8_t MAX_ROUTES = 4;
  RouteRec          _routes[MAX_ROUTES];
  int8_t            _rt_cur;                 // index in use, -1 = none (direct, before any route is known)
  int8_t            _rt_recent[2];           // the last two routes tried (failover skips them if there is anything else)
  uint8_t           _rt_fail_run;        // consecutive unanswered requests on the current route
  uint32_t          _win_req_ms;         // when the current window was requested (reply time)
  bool              _win_credited;   // the current window's first chunk was already counted for the route
  // "fw pull AABB[CC]": a short (2/3-byte) id not in the peer table is resolved by a scan; 2+ matches are refused (collision)
  bool              _resolving;
  bool              _res_scanning;   // the scan in progress is ours
  bool              _res_bl;
  uint8_t           _res_id[3];
  uint8_t           _res_len;            // 2 or 3 bytes
  uint16_t          _res_scans;
  uint32_t          _res_next_ms, _res_interval_ms;
  char              _res_msg[120];     // the collision list, shown by fw pull-status
  mesh::GroupChannel _disc_ch;               // discovery channel (fixed, well-known key: only carries "who serves firmware?")
  uint32_t          _disc_sess;          // session of the last discovery sent for the pull in progress (re-discovery)
  uint16_t          _disc_seq;
  uint32_t          _next_disc_ms;       // re-discover the pull's server: on a stall (5 min, doubling to 1 h) and, while downloading, every 30 min
  uint32_t          _disc_interval_ms;
  uint16_t          _disc_fail_mark;           // (v1.17.26) unanswered-request count of the route in use at the last re-discovery (back-off when unchanged)
  bool              _switching;    // "fw pull <other peer>" during a pull: re-target it, keep the chunks if the image is identical
  char              _switch_prev_name[16]; // ... and what to go back to if the new server offers a different image
  uint8_t           _switch_prev_route[MAX_ROUTE_BYTES]; uint8_t _switch_prev_route_len;
  uint8_t           _switch_prev_ret[MAX_ROUTE_BYTES]; uint8_t _switch_prev_ret_len; bool _switch_prev_ret_set;
  // auto apply
  bool              _noapply;
  bool              _apply_armed;
  uint32_t          _apply_at_ms;
  uint32_t          _mac_fail;   // fw_ota-magic packets received here that failed the MAC check (any role)

  // local staging (serial, not LoRa) - shares _writer with the puller side
  // below, so a stage and a pull can't run concurrently (cmdStage/cmdPull
  // both refuse if the other is active)
  bool       _staging;
  uint32_t   _stage_total;         // total bytes expected this stage (for progress %)
  uint32_t   _stage_remaining;
  uint32_t   _stage_progress_at;   // bytes-received value we last printed progress at
  SlotHeader _stage_hdr;
  bool       _stage_result_pending;
  char       _stage_result[64];

  // server side
  bool     _serving;
  uint32_t _serve_last_idx;   // highest chunk index requested (progress view) - app offer
  uint32_t _bl_last_idx;      // (v1.17.26, A14) the same for the bootloader offer (was mixed into _serve_last_idx)
  uint32_t _bl_offer_reqs;    // (v1.17.26, A14) bootloader OFFER_REQs answered (were not counted)
  uint32_t _serve_reqs;
  uint32_t _serve_last_req_ms; // (rc10, C22) the last app chunk request served (0 = none since boot)
  uint32_t _serve_offer_reqs; // how many OFFER_REQ this server has answered (any name, match or not)
  uint32_t _serve_offer_nomatch; // ... of those, how many didn't match what we're serving
  static const uint8_t RECENT_N = 8;
  struct Recent { uint16_t idx; uint32_t sess; uint32_t ms; uint8_t rlen; uint8_t rpath[MAX_ROUTE_BYTES]; } _recent[RECENT_N];   // image chunks sent lately (see recentlySent)
  uint8_t  _recent_wr;
  // (v1.17.23) heal: a puller that keeps re-asking the same thing does not get our answers (the way back is broken). We then flood a small
  // notice; the path it collects on its way to the puller is a way back that works, and the puller puts it into its next requests.
  // (v1.17.28) kept per PULLER (who = first 4 bytes of its public key, from its offers): after a heal it does not get further (top =
  // the furthest chunk it asked for) the next one waits an hour, whatever session the puller resumes under
  struct HealRec { uint32_t sess; uint32_t key; uint8_t who[4]; uint8_t reps; uint8_t heals; uint16_t top; uint32_t last_ms; };
  static const uint8_t HEAL_N = 4;
  HealRec  _heal[HEAL_N];
  uint8_t  _heal_wr;
  uint32_t _heals_sent;
  uint32_t _flood_ts[4];   // (v1.17.27) the last automatic floods (heals, re-discoveries): FLOOD_BUDGET_N per rolling hour
  bool     floodBudget(uint32_t now);    //     true = one may be sent now (and it is counted)
  XferManifest _serve_man;
  // bootloader-over-LoRa: a node whose own bootloader carries a valid identity descriptor offers it, straight from its flash
  XferManifest _bl_man;
  bool         _bl_serving, _bl_probed;
  uint32_t     _own_bl_rev;           // revision of the running bootloader (0 = it carries no descriptor)
  bool         _bl_force;         // `fw blupdate force`: allow an equal or older revision (RAM only, pre-flight)

  // puller side
  XferPhase _phase;
  uint32_t  _session;
  uint16_t  _seq;
  XferManifest _man;
  SlotWriter   _writer;
  ChunkMap     _cm;                       // which chunks are in the slot (chunks are programmed where they belong, in any order)
  bool         _erased;           // the payload area is erased: chunks may be programmed
  uint8_t      _erase_next;           // next payload page to erase (tick() does two per call)
  uint32_t     _win_start;            // first chunk of the window last requested
  uint16_t     _win_count;            // chunks in that window
  uint8_t      _win_target;                // adaptive: grows on a clean window, shrinks on a retry
  uint32_t     _last_req_ms;
  uint32_t     _retry_timeout_ms;     // jittered per-attempt; avoids deterministic collision alignment
  uint32_t     _rate_t0_ms;           // ETA: when this download (or resume) started ...
  uint16_t     _rate_have0;           // ... and how many chunks were already in the slot then

  // ---- resilience (2026-09-19): a stalled/rebooted pull WAITS and resumes instead of failing ----
  bool         _resuming;         // a pull is in progress but (re)connecting to a server (PH_OFFERING + this flag)
  XferManifest _saved_man;                // identity of the partial slot; compared with every re-offer
  uint32_t     _wait_started_ms;
  uint32_t     _backoff_ms;
  uint16_t     _attempts;             // OFFER_REQ sent in the current wait
  uint8_t      _verify_retries;       // restarts after a checksum/SHA mismatch (bounded)
  bool         _serve_disabled;   // operator said "fw serve off" (persistent); default is auto-serve
  uint32_t     _next_autoserve_ms;
  uint8_t      _retries;
  uint32_t     _crc_bad;              // MSG_CHUNK packets that arrived but failed crc16 (this pull attempt)
  int32_t      _crc_bad_idx;              // chunk index of the most recent crc16 failure, -1 = none yet
  char         _last_err[48];

  void saveState();
  bool loadState();
  void clearState();
  void saveClock();      // just before the apply reboot: keep the clock, so the node comes back with a time newer than its last advert
  void restoreClock();   // (rc21, C58) after an apply, first tick: an RTC chip kept the time - nothing; else forward-only, a SHORT estimate
                         //   of the apply (behind, never ahead: a swarm member's time then corrects it, C57)
  bool              _region_hint;                  // (rc21, C60) the 'no default region' hint was shown since boot
  bool              _clk_restore;                  // restoreClock() is due (begin() runs before the app hands over the FloodHost)
  // (rc21, C57) time from a swarm member with an RTC
  uint32_t          _tq_nonce, _tq_next_ms, _tq_sent_ms; uint8_t _tq_tries;   // asker: the open request, the next one, retries
  bool              _clk_synced;                   // asker: a peer's time was taken since boot (later ones are bounded to +-1 h)
  uint32_t          _clk_at_ms; int32_t _clk_delta; uint8_t _clk_from[4]; uint16_t _clk_n, _clk_refused; uint8_t _clk_how;   // the last correction (how: 1 stepped, 2 held, 3 refused, 0 in step)
  uint32_t          _ta_nonce, _ta_at_ms;          // answerer: the request to answer and when (dropped if another answer is heard first)
  uint16_t          _ta_sent, _ta_supp;
  void              timeTick(uint32_t now);
  void              sendTime(uint8_t type, uint32_t nonce);
  void              onTime(const uint8_t* b, uint32_t len, uint8_t hops);

  bool  macOk(const uint8_t* key, const uint8_t* pkt, uint32_t len) const;
  void  macFill(uint8_t* pkt, uint32_t len) const;      // writes 8 bytes at pkt+len-8
  void  send(const uint8_t* body, uint32_t len, uint32_t delay_ms, const uint8_t* route = nullptr, uint8_t rlen = 0);   // rlen = encoded route length
  uint32_t addReturnRoute(uint8_t* b, uint32_t n) const;   // requests carry the way back (learnt/typed, else the reversed route)
  void  floodOut(mesh::Packet* p, uint32_t delay_ms);       // a discovery-channel flood: region-scoped via the host, >= 2-byte path hashes
  uint32_t buildListRsp(uint8_t* r, const uint8_t* sess_seq, const XferManifest& m, const uint8_t* fwd, uint8_t enc) const;
  void  floodListRsp(const uint8_t* r, uint32_t n, uint32_t delay_ms, bool heal, bool ann = false);   // LIST_RSP wrapped in a discovery-channel flood
  void  noteReq(const uint8_t* req, bool offer, uint16_t start, const XferManifest& m, const uint8_t* who = nullptr);   // server: count repeats, heal when the way back is broken
  void  setRet(const uint8_t* ret, uint8_t enc, bool typed);
  const uint8_t* keyForSend(uint8_t type, uint8_t* tmp) const;
  const uint8_t* keyForRecv(uint8_t type, uint8_t* tmp) const;
  void  onListReq(const uint8_t* b, uint32_t len);
  void  onListRsp(const uint8_t* b, uint32_t len, int8_t snr4, const uint8_t* ret = nullptr, uint8_t ret_enc = 0, bool heal = false);
  void  sendListReq();
  void  sendDiscovery(const char* target, bool zero_hop = false);                 // "*" | "@ID8" | exact node name
  void  startScan(uint32_t delay_ms);                      // what `fw list` does (LIST_REQ + one discovery flood, ~25 s); delay: after the reply
  void  scanSend();                                        // the scan's first transmissions
  // auto routing
  void  routeReset();                                      // a new pull: the table holds just the current _route
  int   routeAdd(const uint8_t* path, uint8_t enc, const uint8_t* ret = nullptr, uint8_t ret_enc = 0);   // find or insert a route PAIR (replaces the weakest when full); -1 if invalid
  void  routeUse(int i);
  float routeScore(int i) const;
  void  routeResult(bool ok, uint32_t rtt_ms);             // credit / debit the route in use
  bool  routeFailover();                                   // switch to the best other route; false = none left
  void  typedRouteFailed(uint32_t now);                    // (v1.17.26, N1) a typed route stopped answering: flood re-discovery
  // short-id resolve
  void  startResolve(const uint8_t* id, uint8_t len, bool bl, char* reply);
  void  resolveTick(uint32_t now);
  void  saveResolve();
  void  clearResolve();
  int   matchPeers(const uint8_t* id, uint8_t len, bool bl, int* first, char* list, size_t cap) const;
  bool  pullTargetId(uint8_t id[4]) const;                 // the pull in progress targets "@ID8"? (only those can be re-discovered)
  void  startPullAt(const char* target, char* reply, bool bl = false);

  bool  stateUsable() const;
  bool  sameImage(const XferManifest& a, const XferManifest& b) const;
  void  enterWait(const char* why);                     // stalled: keep the partial slot, re-offer with backoff
  void  restartFromScratch(const char* why);            // discard the partial slot, fresh offer
  void  retryOrFail(const char* why);                   // checksum mismatch: restart (bounded) or fail
  bool  installedImage(const SlotHeader& h) const;       // the app region holds exactly the slot image
  bool     _probe_valid, _probe_ok;
  uint32_t _probe_key;
  static uint32_t probeKey(const SlotHeader& h) { return h.payload_crc32 ^ (h.raw_crc32 * 31u) ^ h.fw_version ^ (h.payload_len << 3); }
  bool  imageStillServed();                             // server: is the slot still what we advertised?
  void  stopServing();
  void  requestWindow();
  void  eraseStep();
  bool  recentlySent(uint16_t idx, uint32_t sess, uint32_t now, uint8_t rlen, const uint8_t* rpath) const;
  void  noteSent(uint16_t idx, uint32_t sess, uint32_t now, uint8_t rlen, const uint8_t* rpath);
  void  sendListRsp(const uint8_t* req, const XferManifest& m, uint32_t delay_ms,
                    const uint8_t* fwd = nullptr, uint8_t enc = 0, const uint8_t* back = nullptr);   // fwd = path puller->server (told to the puller), back = reversed (used to answer); enc = encoded length
  void  armBlServe();                                    // probe the own bootloader once per boot; offers it if it passes the same checks a receiver applies
  void  blApplyNow();                                    // verified bootloader image in the slot's tail -> MBR copy -> reset
  uint32_t dstAddr(uint32_t off) const { return SLOT_ADDR + SLOT_PAYLOAD_OFFSET + _man.dst_off + off; }
  void  onOfferReq(const uint8_t* b, uint32_t len);
  void  onOfferAck(const uint8_t* b, uint32_t len);
  void  onChunkReq(const uint8_t* b, uint32_t len);
  void  onChunk(const uint8_t* b, uint32_t len);
  void  finishVerify();
  void  fail(const char* why);
};

extern FwOtaTransfer g_xfer;

}  // namespace fw_ota
