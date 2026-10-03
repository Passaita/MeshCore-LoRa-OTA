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
  MSG_BAD_AUTH  = 10,  // server -> puller: "your admin password is wrong" (public-key MAC, only for requests addressed to the server)
  MSG_OFFER_REQ2 = 11, // (v1.17.25) puller -> server : {duty, replaced session, route back, name} - OFFER_REQ + the duty the pull asks for
};

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
};
static const uint8_t MAX_PEERS = 8;

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
};

class FwOtaTransfer {
public:
  // node_name / pub_key / admin_pw are POINTERS kept for the node's lifetime (name and password may change at runtime).
  void begin(mesh::Mesh* mesh, FILESYSTEM* fs, const char* node_name, const uint8_t* pub_key, const char* admin_pw,
             const uint8_t* path_hash_mode = nullptr);   // live pointer to the node's path.hash.mode (discovery floods use at least 2-byte hashes)
  void setFloodHost(FloodHost* h) { _flood_host = h; }
  void tick();
  void onPacket(mesh::Packet* pkt);            // from MyMesh::onRawDataRecv
  // path discovery through stock repeaters: a GRP_TXT flood on the LoRa-OTA discovery channel (stock repeaters relay channel
  // messages; they never relay RAW_CUSTOM floods). The server answers direct along the reversed path the flood collected, and
  // tells the puller that path, which becomes the route for the pull.
  int  channelMatch(const uint8_t* hash, mesh::GroupChannel* out, int max) const;   // MyMesh::searchChannelsByHash
  void onDiscovery(mesh::Packet* pkt, const uint8_t* data, size_t len);            // MyMesh::onGroupDataRecv
  void noteRssi(float rssi_dbm) { _rx_rssi = (int16_t)rssi_dbm; }   // radio's RSSI of the packet about to be handled

  // CLI
  bool cmdServe(char* reply, bool force = false, bool user = true);   // offer the bootloader + the slot if installed here (force: bench); user=false: auto-serve probe, never re-enables
  bool cmdServeOff(char* reply);
  bool cmdPull(const char* name, char* reply);   // scripted/bench: '*' or a served name, own admin password
  bool cmdList(char* reply);                     // discover peers that offer firmware (results print on the serial console)
  bool cmdRoute(const char* arg, char* reply);   // "fw route AA,BB" | off : relays to reach the server (direct-routed through stock repeaters)
  bool cmdPeers(const char* arg, char* reply);   // show the peers found by the last `fw list` (3 per page)
  bool cmdPullPeer(const char* args, char* reply, bool bl = false, bool allow_prompt = true);
                                                 // "<id|#n> [password]" - prompts for the admin password when omitted (serial only:
                                                 // allow_prompt=false = remote session, the password must be on the same line)
                                                 // bl: "fw blupdate [force] <id|#n> [password]" - the offered BOOTLOADER instead of the app
  bool awaitingPassword() const { return _await_pw; }
  bool submitPassword(const char* pw, char* reply);
  void switchSource(const char* target, const char* pw, char* reply);
  void revertSwitch(const char* why);
  bool cmdAutoApply(const char* arg, char* reply); // "on|off": apply a completed pull automatically (default on)
  bool cmdBlInfo(char* reply);                    // the running bootloader: revision, size, CRC, whether it is offered
  bool cmdStatus(char* reply);
  bool cmdAbort(char* reply);
  bool cmdDuty(const char* arg, char* reply);     // "fw duty [0.1-1.0]": show / set the node's LoRa-OTA duty (persistent, default 0.5)
  bool cmdPullBle(char* reply);                   // (v1.17.28) "fw pull-ble": restart into the bootloader's BLE DFU that fills the SLOT (rev 5+)
  bool cmdApply(char* reply);                     // (v1.17.28) "fw apply": install the slot image here (fw pending + reboot)
  void noteRx(uint8_t payload_type, const uint8_t* payload, uint16_t len);   // (rc6, C14) MyMesh::logRx: every received packet
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
  uint32_t stageRemaining() const { return _stage_remaining; }
  void     stageFeed(const uint8_t* data, uint32_t len);   // caller must cap len <= stageRemaining()
  bool     stageTakeResult(char* out, size_t outsz);        // true once, when a stage just finished

private:
  mesh::Mesh*       _mesh = nullptr;
  FILESYSTEM*       _fs   = nullptr;
  const char*       _name_src = nullptr;   // node name (live pointer)
  const uint8_t*    _pub = nullptr;        // node public key (live pointer)
  const char*       _admin_pw = nullptr;   // admin password (live pointer): its derived key is the server-side MAC key
  const uint8_t*    _hash_mode = nullptr;  // node's path.hash.mode (live pointer), nullptr = 2-byte discovery floods
  uint8_t           _pull_key[16];         // MAC key for OUR pull = derived from the password the operator typed
  bool              _pull_key_set = false;
  int16_t           _rx_rssi = 0;
  FloodHost*        _flood_host = nullptr;
  // (v1.17.28) security v2 (docs/SECURITY-V2-DESIGN.md s.8): LoRa-OTA works only inside the owner's circle (a passphrase on every node).
  // Circle key C (PBKDF2 of the passphrase) -> discovery channel + list key + offer key; per pull a session key from C, the ECDH of the two
  // identities and both nonces -> MAC of every later packet + AES-CTR of the chunk data. No passphrase = hidden (serves/answers/pulls nothing).
  uint8_t           _circle[32];
  bool              _circle_set = false;
  uint8_t           _list_key[16], _offer_key[16];  // derived from C
  uint8_t           _sess_mac[16], _sess_enc[16];   // puller: the session keys (after the OFFER_ACK)
  bool              _sess_set = false;
  uint8_t           _nonce[8];                      // puller: this session's nonce (in every OFFER_REQ2 of the session)
  uint32_t          _nonce_sess = 0;
  void              circleKeys();                   // C -> discovery channel, list key, offer key
  // `fw passphrase` runs PBKDF2 in the background (~20 s on the nRF52840) so the repeater keeps relaying meanwhile
  char              _pb_pw[65];
  uint8_t           _pb_u[32], _pb_out[32];
  uint32_t          _pb_i = 0;                      // iterations done (0 = idle)
  void              passphraseStep();
  bool              sessionKeys(const uint8_t* peer_pub, uint32_t sess, const uint8_t* np, const uint8_t* ns, uint8_t* kmac, uint8_t* kenc);
  void              macFillKey(uint8_t* pkt, uint32_t len, const uint8_t* key) const;
  // (v1.17.28) BLE slot fill: `fw pull-ble` -> bootloader rev 5 writes a .lota into the slot -> at boot it becomes a VALID slot, served
  uint32_t          _reset_at_ms = 0;               // `fw pull-ble` / `fw apply`: restart this long after the reply went out
  uint8_t           _reset_magic = 0;               // GPREGRET for that restart (0xA9 = BLE slot fill, 0 = plain reboot)
  void              slotFromLota();                 // at boot: a .lota uploaded over BLE -> checked -> VALID slot (or refused, slot empty)
  bool              bleFilled(const SlotHeader& h) const;   // this slot image came in over BLE (served although not installed here)
  // duty (v1.17.25)
  uint8_t           _duty_pct = DUTY_DEFAULT_PCT;   // this node's duty, as server (ceiling) and as puller (asked unless overridden)
  uint8_t           _next_pull_duty = 0;            // set by the CLI for the pull it is about to start
  uint8_t           _pull_duty_pct = 0;             // per-pull override of the pull in progress (0 = the node's duty); persisted with the pull
  uint8_t           _agreed_pct = 0;                // puller: agreed with the server in its OFFER_ACK (0 = not yet)
  uint32_t          _prev_session = 0;              // puller: the session this one replaces (resume, switch, reboot) - its share is freed at the server
  struct DutySess { uint32_t sess; uint8_t pct; uint32_t last_ms; uint32_t next_tx_ms;     // server: the pace agreed per puller session ...
                    uint8_t kmac[16], kenc[16]; uint8_t np[8], ns[8]; bool keyed;           // ... and (v1.17.28) its session keys
                    // (rc6, C14) the window being served, one chunk per quiet moment
                    uint16_t q_idx[XFER_WINDOW]; uint8_t q_n, q_i; bool q_bl, q_paced; uint8_t q_rlen, q_relays; uint8_t q_rpath[MAX_ROUTE_BYTES];
                    uint8_t q_rseq[2]; uint32_t q_sp, q_due, q_wait0; };
  static const uint8_t DUTY_SESS_N = 4;
  DutySess          _dsess[DUTY_SESS_N] = {};
  uint8_t           askedPct() const { return _pull_duty_pct ? _pull_duty_pct : _duty_pct; }
  uint32_t          chunkAirMs(uint8_t route_enc) const;   // airtime of one CHUNK packet along a route of that encoding
  DutySess*         dutySess(uint32_t sess, bool create);
  uint8_t           dutyOthers(uint32_t sess) const;       // sum of the duty held by the OTHER active sessions
  // discovery
  PeerInfo          _peers[MAX_PEERS];
  bool              _scan_active = false;
  uint32_t          _scan_end_ms = 0, _scan_resend_ms = 0;
  uint32_t          _scan_start_ms = 0;                    // (v1.17.28) `fw list` transmits from here (after its reply) ...
  bool              _scan_pending = false;                 //     ... until then nothing is sent
  uint8_t           _scan_resends = 0;                     //     direct requests repeated so far (max SCAN_REQ_RESENDS)
  static const uint8_t ANS_N = 6;                          // (v1.17.28) server: scan sessions answered lately (answerAllowed)
  AnsRec            _ans[ANS_N] = {};
  uint8_t           _ans_wr = 0;
  bool              answerOnce(uint32_t sess, bool flood);
  uint32_t          _last_foreign_ms = 0;                  // (rc6, C14) the last packet heard that was not LoRa-OTA
  bool              _req_pending = false;                  // (rc6, C14) puller: a window request waiting for a quiet moment
  uint32_t          _req_wait0 = 0;
  void              serveTick(uint32_t now);
  void              sendChunk(DutySess& d, uint16_t idx, uint32_t now);
  uint32_t          _last_ans_ms = 0;                      // (rc4, S4) the last discovery answer for the running pull
  struct Txt16 { char c[16]; };
  Txt16             lastAnsText() const;
  void              routeDropDirect(int keep);             // (rc4, S3)
  bool              _paused = false;                       // (v1.17.28, C8) `fw pull-pause`: the pull sends nothing
  uint32_t          _pause_until_ms = 0;                   //     timed pause end (0 = until `fw pull-resume`)
  uint8_t           _pause_h = 0;
  void              setPaused(bool on, uint8_t hours);
  void              resumePull(const char* why);
  uint32_t          _scan_sess = 0;
  uint16_t          _scan_seq = 0;
  // multi-hop: relays the puller's messages are direct-routed through (persisted with the pull state)
  uint8_t           _route[MAX_ROUTE_BYTES];
  uint8_t           _route_len = 0;          // encoded (rtSize/rtCount); 0 = direct
  bool              _route_manual = false;   // `fw route ..` typed: a listed peer's discovered route does not replace it
  // (v1.17.23) the return route the requests carry: learnt (flooded discovery answer / heal) or typed (`fw route fwd/ret`); unset = reverse of _route
  uint8_t           _ret[MAX_ROUTE_BYTES];
  uint8_t           _ret_len = 0;
  bool              _ret_set = false;
  bool              _ret_typed = false;      // typed by the operator: a heal does not replace it (v1.17.26: unless the typed route failed)
  bool              _typed_failed = false;   // (v1.17.26, N1) like MeshCore's saved paths: the typed route got TYPED_FAIL_REDISCOVER unanswered
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
  RouteRec          _routes[MAX_ROUTES] = {};
  int8_t            _rt_cur = -1;            // index in use, -1 = none (direct, before any route is known)
  int8_t            _rt_recent[2] = {-1, -1};// the last two routes tried (failover skips them if there is anything else)
  uint8_t           _rt_fail_run = 0;        // consecutive unanswered requests on the current route
  uint32_t          _win_req_ms = 0;         // when the current window was requested (reply time)
  bool              _win_credited = false;   // the current window's first chunk was already counted for the route
  // "fw pull AABB[CC] pw": a short (2/3-byte) id not in the peer table is resolved by a scan; 2+ matches are refused (collision)
  bool              _resolving = false;
  bool              _await_res = false;    // the password prompt is for a short-id resolve
  bool              _res_scanning = false;   // the scan in progress is ours
  bool              _res_bl = false;
  uint8_t           _res_id[3] = {0, 0, 0};
  uint8_t           _res_len = 0;            // 2 or 3 bytes
  uint8_t           _res_key[16];
  uint16_t          _res_scans = 0;
  uint32_t          _res_next_ms = 0, _res_interval_ms = 0;
  char              _res_msg[120] = {0};     // the collision list, shown by fw pull-status
  mesh::GroupChannel _disc_ch;               // discovery channel (fixed, well-known key: only carries "who serves firmware?")
  uint32_t          _disc_sess = 0;          // session of the last discovery sent for the pull in progress (re-discovery)
  uint16_t          _disc_seq = 0;
  uint32_t          _next_disc_ms = 0;       // re-discover the pull's server: on a stall (5 min, doubling to 1 h) and, while downloading, every 30 min
  uint32_t          _disc_interval_ms = 0;
  uint16_t          _disc_fail_mark = 0xFFFF;  // (v1.17.26) unanswered-request count of the route in use at the last re-discovery (back-off when unchanged)
  // password prompt
  bool              _await_pw = false;
  bool              _await_bl = false;     // the prompt is for a bootloader update
  uint8_t           _await_peer = 0;
  bool              _switching = false;    // "fw pull <other peer>" during a pull: re-target it, keep the chunks if the image is identical
  char              _switch_prev_name[16]; // ... and what to go back to if the new server offers a different image
  uint8_t           _switch_prev_key[16];
  uint8_t           _switch_prev_route[MAX_ROUTE_BYTES]; uint8_t _switch_prev_route_len = 0;
  uint8_t           _switch_prev_ret[MAX_ROUTE_BYTES]; uint8_t _switch_prev_ret_len = 0; bool _switch_prev_ret_set = false;
  uint32_t          _await_since_ms = 0;
  // auto apply
  bool              _noapply = false;
  bool              _apply_armed = false;
  uint32_t          _apply_at_ms = 0;
  uint32_t          _mac_fail = 0;   // fw_ota-magic packets received here that failed the MAC check (any role)

  // local staging (serial, not LoRa) - shares _writer with the puller side
  // below, so a stage and a pull can't run concurrently (cmdStage/cmdPull
  // both refuse if the other is active)
  bool       _staging = false;
  uint32_t   _stage_total = 0;         // total bytes expected this stage (for progress %)
  uint32_t   _stage_remaining = 0;
  uint32_t   _stage_progress_at = 0;   // bytes-received value we last printed progress at
  SlotHeader _stage_hdr;
  bool       _stage_result_pending = false;
  char       _stage_result[64] = {0};

  // server side
  bool     _serving = false;
  uint32_t _serve_last_idx = 0;   // highest chunk index requested (progress view) - app offer
  uint32_t _bl_last_idx = 0;      // (v1.17.26, A14) the same for the bootloader offer (was mixed into _serve_last_idx)
  uint32_t _bl_offer_reqs = 0;    // (v1.17.26, A14) bootloader OFFER_REQs answered (were not counted)
  uint32_t _serve_reqs = 0;
  uint32_t _serve_offer_reqs = 0; // how many OFFER_REQ this server has answered (any name, match or not)
  uint32_t _serve_offer_nomatch = 0; // ... of those, how many didn't match what we're serving
  static const uint8_t RECENT_N = 8;
  struct Recent { uint16_t idx; uint32_t sess; uint32_t ms; } _recent[RECENT_N] = {};   // chunks sent lately (see recentlySent)
  uint8_t  _recent_wr = 0;
  uint32_t _last_bad_auth_ms = 0;
  // (v1.17.23) heal: a puller that keeps re-asking the same thing does not get our answers (the way back is broken). We then flood a small
  // notice; the path it collects on its way to the puller is a way back that works, and the puller puts it into its next requests.
  // (v1.17.28) kept per PULLER (who = first 4 bytes of its public key, from its offers): after a heal it does not get further (top =
  // the furthest chunk it asked for) the next one waits an hour, whatever session the puller resumes under
  struct HealRec { uint32_t sess; uint32_t key; uint8_t who[4]; uint8_t reps; uint8_t heals; uint16_t top; uint32_t last_ms; };
  static const uint8_t HEAL_N = 4;
  HealRec  _heal[HEAL_N] = {};
  uint8_t  _heal_wr = 0;
  uint32_t _heals_sent = 0;
  uint32_t _flood_ts[4] = {0, 0, 0, 0};   // (v1.17.27) the last automatic floods (heals, re-discoveries): FLOOD_BUDGET_N per rolling hour
  bool     floodBudget(uint32_t now);    //     true = one may be sent now (and it is counted)
  XferManifest _serve_man;
  // bootloader-over-LoRa: a node whose own bootloader carries a valid identity descriptor offers it, straight from its flash
  XferManifest _bl_man;
  bool         _bl_serving = false, _bl_probed = false;
  uint32_t     _own_bl_rev = 0;           // revision of the running bootloader (0 = it carries no descriptor)
  bool         _bl_force = false;         // `fw blupdate force`: allow an equal or older revision (RAM only, pre-flight)

  // puller side
  XferPhase _phase = PH_IDLE;
  uint32_t  _session = 0;
  uint16_t  _seq = 0;
  XferManifest _man;
  SlotWriter   _writer;
  ChunkMap     _cm;                       // which chunks are in the slot (chunks are programmed where they belong, in any order)
  bool         _erased = false;           // the payload area is erased: chunks may be programmed
  uint8_t      _erase_next = 0;           // next payload page to erase (tick() does two per call)
  uint32_t     _win_start = 0;            // first chunk of the window last requested
  uint16_t     _win_count = 0;            // chunks in that window
  uint8_t      _win_target = XFER_WIN_INIT; // adaptive: grows on a clean window, shrinks on a retry
  uint8_t      _bad_auth = 0;             // BAD_AUTH notices received for this pull (2 = abort: wrong admin password)
  uint32_t     _last_req_ms = 0;
  uint32_t     _retry_timeout_ms = 0;     // jittered per-attempt; avoids deterministic collision alignment
  uint32_t     _rate_t0_ms = 0;           // ETA: when this download (or resume) started ...
  uint16_t     _rate_have0 = 0;           // ... and how many chunks were already in the slot then

  // ---- resilience (2026-09-19): a stalled/rebooted pull WAITS and resumes instead of failing ----
  bool         _resuming = false;         // a pull is in progress but (re)connecting to a server (PH_OFFERING + this flag)
  XferManifest _saved_man;                // identity of the partial slot; compared with every re-offer
  uint32_t     _wait_started_ms = 0;
  uint32_t     _backoff_ms = 0;
  uint16_t     _attempts = 0;             // OFFER_REQ sent in the current wait
  uint8_t      _verify_retries = 0;       // restarts after a checksum/SHA mismatch (bounded)
  bool         _serve_disabled = false;   // operator said "fw serve off" (persistent); default is auto-serve
  uint32_t     _next_autoserve_ms = 0;
  uint8_t      _retries = 0;
  uint32_t     _crc_bad = 0;              // MSG_CHUNK packets that arrived but failed crc16 (this pull attempt)
  int32_t      _crc_bad_idx = -1;         // chunk index of the most recent crc16 failure, -1 = none yet
  char         _last_err[48] = {0};

  void saveState();
  bool loadState();
  void clearState();
  void saveClock();      // just before the apply reboot: keep the clock, so the node comes back with a time newer than its last advert
  void restoreClock();   // at boot: forward-only, +30 s for the apply itself (a real RTC module or a later `clock sync` still wins)

  bool  macOk(const uint8_t* key, const uint8_t* pkt, uint32_t len) const;
  void  macFill(uint8_t* pkt, uint32_t len) const;      // writes 8 bytes at pkt+len-8
  void  send(const uint8_t* body, uint32_t len, uint32_t delay_ms, const uint8_t* route = nullptr, uint8_t rlen = 0);   // rlen = encoded route length
  uint32_t addReturnRoute(uint8_t* b, uint32_t n) const;   // requests carry the way back (learnt/typed, else the reversed route)
  void  floodOut(mesh::Packet* p, uint32_t delay_ms);       // a discovery-channel flood: region-scoped via the host, >= 2-byte path hashes
  uint32_t buildListRsp(uint8_t* r, const uint8_t* sess_seq, const XferManifest& m, const uint8_t* fwd, uint8_t enc) const;
  void  floodListRsp(const uint8_t* r, uint32_t n, uint32_t delay_ms, bool heal);   // LIST_RSP wrapped in a discovery-channel flood
  void  noteReq(const uint8_t* req, bool offer, uint16_t start, const XferManifest& m, const uint8_t* who = nullptr);   // server: count repeats, heal when the way back is broken
  void  setRet(const uint8_t* ret, uint8_t enc, bool typed);
  void  deriveKey(const char* pw, uint8_t out[16]) const;
  const uint8_t* keyForSend(uint8_t type, uint8_t* tmp) const;
  const uint8_t* keyForRecv(uint8_t type, uint8_t* tmp) const;
  void  onListReq(const uint8_t* b, uint32_t len);
  void  onListRsp(const uint8_t* b, uint32_t len, int8_t snr4, const uint8_t* ret = nullptr, uint8_t ret_enc = 0, bool heal = false);
  void  sendListReq();
  void  sendDiscovery(const char* target);                 // "*" | "@ID8" | exact node name
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
  void  startResolve(const uint8_t* id, uint8_t len, bool bl, const char* pw, char* reply);
  void  resolveTick(uint32_t now);
  void  saveResolve();
  void  clearResolve();
  int   matchPeers(const uint8_t* id, uint8_t len, bool bl, int* first, char* list, size_t cap) const;
  bool  pullTargetId(uint8_t id[4]) const;                 // the pull in progress targets "@ID8"? (only those can be re-discovered)
  void  writePullKey();
  void  startPullAt(const char* target, char* reply, bool bl = false);
  void  clearPullKey();

  bool  stateUsable() const;
  bool  sameImage(const XferManifest& a, const XferManifest& b) const;
  void  enterWait(const char* why);                     // stalled: keep the partial slot, re-offer with backoff
  void  restartFromScratch(const char* why);            // discard the partial slot, fresh offer
  void  retryOrFail(const char* why);                   // checksum mismatch: restart (bounded) or fail
  bool  installedImage(const SlotHeader& h) const;       // the app region holds exactly the slot image
  bool     _probe_valid = false, _probe_ok = false;
  uint32_t _probe_key = 0;
  bool  imageStillServed();                             // server: is the slot still what we advertised?
  void  stopServing();
  void  requestWindow();
  void  eraseStep();
  bool  recentlySent(uint16_t idx, uint32_t sess, uint32_t now) const;
  void  noteSent(uint16_t idx, uint32_t sess, uint32_t now);
  void  replyBadAuth(const uint8_t* b, uint32_t len);
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
