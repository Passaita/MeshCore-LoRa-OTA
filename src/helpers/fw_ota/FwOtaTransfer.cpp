#include "FwOtaTransfer.h"
#include <string.h>
#include <SHA256.h>
#include <AES.h>        // (v1.17.28) AES-128-CTR of the chunk data
#include <nrf_mbr.h>    // sd_mbr_command(COPY_BL)
#include <nrf_sdm.h>    // sd_softdevice_disable()
#include <nrf_soc.h>    // sd_power_gpregret_*() (v1.17.28: `fw pull-ble` restart)

namespace fw_ota {

FwOtaTransfer g_xfer;

// key for the unauthenticated-by-password discovery messages (fw list); transfers use a key derived from the admin password
// (v1.17.28) there is no public key any more: discovery and lists use keys derived from the owner's circle (circleKeys())

static const uint32_t OFFER_RETRY_MS  = 4000;
static const uint32_t WIN_RETRY_MS     = 6000;
static const uint8_t  MAX_RETRIES      = 12;
static const char*    STATE_FILE       = "/fwota.state";
static const char*    NOSERVE_FILE     = "/fwota.noserve";        // present = operator opted out of auto-serve ("fw serve off")
static const uint32_t AUTOSERVE_POLL_MS = 15000;                  // how often a node without an active offer re-checks its slot
static const uint32_t STATE_MAGIC      = 0x38585746;              // "FWX8" (v8: + the pull's duty override; v7 = + the return route of the pair, still read)
static const uint32_t STATE_MAGIC_V7   = 0x37585746;              // "FWX7" (v7: + the return route of the pair; v6 = route only, still read)
static const uint32_t STATE_MAGIC_V6   = 0x36585746;              // "FWX6" (v6: route = encoded length + up to MAX_ROUTE_BYTES; v5 had 4 one-byte hashes)
static const uint32_t MAX_RESUME_WAIT_MS = 7UL * 24UL * 3600UL * 1000UL; // a pull waits for its server up to a week (only a version change ends it earlier)
static const uint32_t WAIT_BACKOFF_MAX_MS = 60000;
static const uint32_t WAIT_BACKOFF_LONG_MS = 5UL * 60 * 1000;    // (v1.17.28) ... and after WAIT_LONG_AFTER unanswered offers up to 5 min: a stall of hours
static const uint16_t WAIT_LONG_AFTER   = 10;                     //     (the way back broken) costs ~12 offers an hour instead of ~60, each repeated by every relay
static const uint8_t  MAX_VERIFY_RETRIES = 2;
static const char*    PK_FILE          = "/fwota.pk";             // MAC key of the pull in progress (derived from the admin password typed)
static const char*    NOAPPLY_FILE     = "/fwota.noapply";        // present = do NOT auto-apply a completed pull
static const char*    CLOCK_FILE       = "/fwota.clock";          // epoch saved right before an apply reboot (the software clock resets on reboot)
static const uint32_t SCAN_MS          = 25000;                   // fw list listens this long ...
static const uint32_t SCAN_START_MS    = 2500;                    // (v1.17.28) ... starting 2.5 s after the command (its reply leaves first) ...
static const uint32_t SCAN_REQ_MIN_MS   = 6000;                    // ... and asks its direct neighbours ONE more time after a random 6 - 10 s
static const uint32_t SCAN_REQ_SPREAD_MS = 4000;                   //     (it asked every 1 - 3 s before: 8 requests, each answered twice by
static const uint8_t  SCAN_REQ_RESENDS = 1;                        //     every server in range - a burst that drowned a weak relay path)
static const uint32_t ANS_MEMORY_MS    = 60000;                    // (v1.17.28) server: a scan session is answered ONCE (see answerAllowed)
static const uint32_t DISC_RSP_MIN_MS  = 1000;                     // (v1.17.28) flooded answer 1 - 5 s after the discovery (+0.3 s per hop):
static const uint32_t DISC_RSP_SPREAD_MS = 4000;                   //     clear of the relays' own repeats of that discovery
static const uint32_t DISC_FIRST_MS    = 60000;                   // re-discover the server 60 s into a stall / unanswered pull ...
static const uint32_t DISC_WAIT_MS     = 5UL * 60 * 1000;         // ... then every 5 min while waiting ...
static const uint32_t DISC_DOWNLOAD_MS = 30UL * 60 * 1000;        // ... and every 30 min while downloading (a shorter path is adopted) ...
static const uint32_t DISC_DOWNLOAD_MAX_MS = 4UL * 3600 * 1000;   // ... (v1.17.26) doubling up to 4 h while the route in use answers everything
                                                                  //     (multi-hop pulls run for days: 48 discovery floods a day for nothing)
static const uint32_t RECENT_MS         = 1500;                    // server: a chunk sent this recently to another puller is not sent again
static const uint32_t PW_PROMPT_MS     = 60000;
static const char*    RESOLVE_FILE     = "/fwota.resolve";        // "fw pull AABB[CC] pw" still looking for its server (survives a reboot)
static const uint32_t RESOLVE_MAGIC    = 0x31525746;              // "FWR1"
static const uint8_t  ROUTE_FAIL_SWITCH = 2;                      // unanswered requests in a row on one route before trying another
static const uint32_t RES_FIRST_MS     = 60000;                   // short id not found: scan again after 1 min, doubling ...
static const uint32_t RES_MAX_MS       = 30UL * 60 * 1000;        // ... up to every 30 min (until found or 'fw pull-abort')
static const uint8_t  HEAL_REPS        = 3;                       // server: the same request this many times in a row = our answers do not arrive
static const uint32_t HEAL_NOPROG_MS   = 3600000UL;               // (v1.17.28) ... a heal the puller did not get past (its requests stay where they
                                                                  //     were): the next one for that PULLER (not session) no sooner than 1 h later
static const uint8_t  HEAL_MAX         = 4;                       // ... and at most 4 in a row without progress (progress starts over)
static const uint8_t  TYPED_FAIL_REDISCOVER = 6;                  // (v1.17.26, N1) unanswered requests in a row on a TYPED route before a flood
                                                                  // re-discovery. meshcore-open: 4 direct attempts, the 5th floods (maxRetries 5);
                                                                  // OTA requests come every few seconds, so a little more patience here
static const char     FLOOD_RSP[]      = "LOTA1R";                // discovery-channel flood: answer to a discovery that asked for a flooded answer
static const char     FLOOD_HEAL[]     = "LOTA1H";                // discovery-channel flood: heal notice (the path it collects = a working way back)
static const char*    CIRCLE_FILE      = "/fwota.circle";         // (v1.17.28) the circle key C (32 B, PBKDF2 of the owner's passphrase; never the phrase)
static const uint32_t PBKDF2_ITER      = 50000;                   // ~2-4 s once on the nRF52840 (`fw passphrase`)
static const uint8_t  PASS_MIN         = 8;
static const char*    PAUSE_FILE       = "/fwota.pause";          // (v1.17.28, C8) present = the pull is paused; 1 byte: hours (0 = until fw pull-resume)
static const char*    BLEFILL_FILE     = "/fwota.blefill";        // (v1.17.28) payload CRC-32 of the slot image uploaded over BLE (`fw pull-ble`)
static const uint32_t LOTA_HDR         = 160;                     // .lota header (lora-ota-serve doc/LOTA-FORMAT.md)
static const uint8_t  GPREGRET_SLOT_DFU = 0xA9;                   // bootloader rev 5: BLE DFU into the slot
static const char*    DUTY_FILE        = "/fwota.duty";           // (v1.17.25) the node's LoRa-OTA duty in percent (absent = DUTY_DEFAULT_PCT)
static const uint32_t CHUNK_GAP_MS     = 90;                      // spacing of the chunks of one window at duty 1.0 (as before v1.17.25)
static const uint32_t CHUNK_AIR_FALLBACK_MS = 650;                // a chunk's airtime when the radio gives no estimate (SF7/62.5 kHz: ~610 ms)
static const uint32_t DUTY_WIN_MS      = 10000;                   // a paced window lasts at most ~10 s (fewer chunks per window at a low duty)
static const uint32_t DUTY_ACTIVE_MS   = 60000;                   // server: a session without a request for this long no longer holds a share
static const uint8_t  RELAY_BATCH      = 5;                       // (v1.17.27) missing chunks asked per window through relays (fixed, so never below 4
                                                                  //     unless fewer remain) - lost ones are re-requested with the next batch
static const uint32_t BATCH_GAP_MS     = 5000;                    // (v1.17.27) quiet gap after each paced window: logins/messages get through every hop
static const uint32_t BATCH_GAP_HOP_MS = 2500;
static const uint8_t  DUTY_FULL_PCT    = 30;                      // (rc6, owner) `fw duty 1.0` = 30 % of the channel model (the old 0.3: what the owner
                                                                  //     accepts at full speed); 0.3 = 9 %. The model counts only the chunks and their
                                                                  //     repeats, not requests, collisions and the relays' other traffic
static const uint32_t YIELD_QUIET_MS   = 2000;                    // (rc6, C14) send LoRa-OTA only after 2 s without other traffic heard ...
static const uint32_t YIELD_MAX_MS     = 20000;                   // ... but never wait longer than 20 s for that (a busy mesh must not stall it)
static const uint32_t YIELD_PER_CHUNK_MS = 1500;                  // ... puller: window timeout margin per chunk for the server's waits                    // (rc5, C12) ... and it grows with the chain: 2.5 s x (relays + 1), at least 5 s
static const uint8_t  FLOOD_BUDGET_N   = 4;                       // (v1.17.27, user) ONE node-wide budget for automatic LoRa-OTA floods (heal notices,
static const uint32_t FLOOD_BUDGET_MS  = 3600000UL;               //     automatic re-discoveries): at most 4 per rolling hour, no daily cap

static inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

// ---- duty (v1.17.25, A10) - pure functions, host-tested (test/fw_ota_duty_test.sh) ----
// "0.1" .. "1" / "1.0" (at most two decimals) -> percent 10..100; 0 = not a valid duty
static uint8_t dutyParse(const char* s) {
  while (*s == ' ') s++;
  if (!isdigit((unsigned char)*s)) return 0;
  uint32_t ip = 0; while (isdigit((unsigned char)*s) && ip < 10) ip = ip * 10 + (uint32_t)(*s++ - '0');
  uint32_t fr = 0, nd = 0;
  if (*s == '.') { s++; while (isdigit((unsigned char)*s)) { if (nd < 2) { fr = fr * 10 + (uint32_t)(*s - '0'); nd++; } else if (*s != '0') return 0; s++; } }
  while (*s == ' ') s++;
  if (*s) return 0;
  if (nd == 1) fr *= 10;
  uint32_t pct = ip * 100 + fr;
  return (pct >= DUTY_MIN_PCT && pct <= 100) ? (uint8_t)pct : 0;
}
// What a server grants a new session: the asked duty (0 = no wish = its own), capped by its own duty minus what its other active sessions
// already hold - never below DUTY_MIN_PCT (a saturated server still serves, slowly).
static uint8_t dutyAgree(uint8_t asked, uint8_t own, uint16_t others) {
  uint8_t cap = own > others ? (uint8_t)(own - others) : 0;
  uint8_t a = asked ? asked : own;
  if (a > cap) a = cap;
  return a < DUTY_MIN_PCT ? DUTY_MIN_PCT : a;
}
// (v1.17.27) Relay-aware: every relay on the chunks' way repeats each chunk, so one chunk costs air x (relays + 1) of the CHANNEL, and the
// duty caps that - not only the server's own airtime (v1.17.25/26 capped only the server: through 2 relays the channel near them was full,
// 2026-09-28). On a relayed route the chunks are also never closer than one chain transit: a relay cannot receive the next chunk while it
// repeats the previous one (half duplex) - at 1.2 s spacing the chunks collided with their own repeats, the window fell to 1.
static bool dutyPaced(uint8_t pct, uint8_t relays) { return (pct && pct < 100) || relays; }
// start-to-start spacing of the chunks of one window (both ends compute it the same way)
static uint32_t dutySpacingMs(uint32_t air, uint8_t pct, uint8_t relays) {
  const uint32_t chain = air * ((uint32_t)relays + 1);
  if (pct >= 100 || pct == 0) return relays ? chain : CHUNK_GAP_MS;
  uint32_t s = chain * 100 / pct;
  return s > CHUNK_GAP_MS ? s : CHUNK_GAP_MS;
}
// chunks per window: a fixed batch through relays (the missing ones are re-requested in the next batch); direct: as many as fit ~10 s
static uint8_t dutyWinMax(uint32_t air, uint8_t pct, uint8_t relays) {
  if (relays) return RELAY_BATCH;
  uint32_t per = dutySpacingMs(air, pct, 0); if (per < air) per = air;
  uint32_t n = per ? DUTY_WIN_MS / per : XFER_WINDOW;
  return (uint8_t)(n < 1 ? 1 : n > XFER_WINDOW ? XFER_WINDOW : n);
}
// the puller's window timeout on top of the 3-9 s it always had. The timer restarts on every chunk that arrives, so it covers ONE pacing
// gap, the quiet gap the server keeps after each batch (before the first chunk of the next one), and 1 s of queueing behind mesh traffic.
// (rc5, C12, owner) the quiet gap after a paced window: a longer chain needs longer to settle before the next batch (5 s up to 1 relay,
// 7.5 s at 2, 10 s at 3). Server (next window) and puller (window timeout) use the same value.
static uint32_t batchGapMs(uint8_t relays) {
  const uint32_t g = BATCH_GAP_HOP_MS * ((uint32_t)relays + 1);
  return g > BATCH_GAP_MS ? g : BATCH_GAP_MS;
}
// (rc6, owner) the duty the user sets -> the channel share the pacing uses (both ends: server spacing, puller timeout)
static uint8_t chanPct(uint8_t pct) {
  if (!pct) return 0;
  uint32_t c = (uint32_t)pct * DUTY_FULL_PCT / 100;
  return (uint8_t)(c < 1 ? 1 : c);
}
// (rc6, C14) may a LoRa-OTA packet go out now? Only when nothing else (mesh traffic, not LoRa-OTA) was heard for YIELD_QUIET_MS - the
// transfer fills the quiet moments - or after YIELD_MAX_MS of waiting. last_foreign = 0: nothing heard yet.
static bool otaQuiet(uint32_t now, uint32_t last_foreign, uint32_t waiting_since) {
  if (!last_foreign || now - last_foreign >= YIELD_QUIET_MS) return true;
  return waiting_since && now - waiting_since >= YIELD_MAX_MS;
}
static uint32_t dutyWinExtraMs(uint32_t air, uint8_t pct, uint8_t relays) {
  if (!dutyPaced(pct, relays)) return 0;
  uint32_t sp = dutySpacingMs(air, pct, relays);
  return (sp > air ? sp - air : 0) + batchGapMs(relays) + 1000;
}

// (v1.17.28) server: answer a scan session once. A direct request (zero-hop LIST_REQ) may be answered twice - the scanner asks its
// neighbours twice, the second time in case the first answer was lost; a flooded discovery once, and not at all when the same session
// was already answered directly (a neighbour hears the direct answer; the flooded one would only cross the whole mesh for nothing).
static bool answerAllowed(AnsRec* r, uint8_t n, uint8_t& wr, uint32_t sess, bool flood, uint32_t now) {
  AnsRec* h = nullptr;
  for (uint8_t i = 0; i < n && !h; i++) if (r[i].sess == sess && r[i].ms && now - r[i].ms < ANS_MEMORY_MS) h = &r[i];
  if (!h) { h = &r[wr]; wr = (uint8_t)((wr + 1) % n); memset(h, 0, sizeof(*h)); h->sess = sess; }
  h->ms = now ? now : 1;
  if (flood) { if (h->flooded || h->direct) return false; h->flooded = 1; return true; }
  if (h->direct >= 1 + SCAN_REQ_RESENDS) return false;
  h->direct++; return true;
}

// (v1.17.28, C2/C6/C7) how a server answers a discovery: which offers (bit 0 app, bit 1 bootloader) and how (bit 2 flooded, bit 3 direct
// back along the discovery's reversed path). A pull looking for us ('@ID8' app, '@ID8!' bootloader) gets only the kind it asks for (C7),
// flooded (works over one-way links) AND direct (a flood that collides at the puller's edge is often lost - found 2026-09-30, C6).
// A scan ('*' or a name) heard at 0 hops is NOT answered here: its zero-hop LIST_REQ gets the direct answer (C2 - no city-wide flood
// for a neighbour). `flood` = the discovery asked for a flooded answer (" F"); older discoveries are answered direct only.
static uint8_t discAnswer(const char* target, uint8_t hops, bool flood, bool app_avail, bool bl_avail) {
  const bool tgt = target[0] == '@';
  const bool want_bl = tgt && strlen(target) == 10 && target[9] == '!';
  uint8_t kinds = (uint8_t)((app_avail && (!tgt || !want_bl) ? 1 : 0) | (bl_avail && (!tgt || want_bl) ? 2 : 0));
  if (!kinds) return 0;
  if (!flood) return (uint8_t)(kinds | 8);
  if (!tgt) return hops ? (uint8_t)(kinds | 4) : 0;
  return (uint8_t)(kinds | 4 | 8);
}

// (v1.17.28, C8) "fw pull-pause [Nh]": -1 = not understood, 0 = until 'fw pull-resume', N = hours (1 - 168)
static int pauseHours(const char* a) {
  while (*a == ' ') a++;
  if (!*a) return 0;
  char* e = nullptr; long h = strtol(a, &e, 10);
  if (e == a || (*e != 'h' && *e != 'H') || e[1] != 0 || h < 1 || h > 168) return -1;
  return (int)h;
}

// (v1.17.28) scanner: when to ask the direct neighbours again (0 = no more), `resends` = requests already repeated
static uint32_t scanResendAt(uint8_t resends, uint32_t now, uint32_t rnd, uint32_t end_ms) {
  if (resends >= SCAN_REQ_RESENDS) return 0;
  uint32_t t = now + SCAN_REQ_MIN_MS + rnd % (SCAN_REQ_SPREAD_MS + 1);
  return (int32_t)(end_ms - t) < 3000 ? 0 : t;                 // leave time for the last answers
}

// n bytes from the first 2n hex digits of src; the rest of src is ignored. (mesh::Utils::fromHex() insists that strlen(src) == 2n, so it
// failed for every hop but the last of "fw route AA,BB" and for a bootloader target "@ID8!" - both found 2026-09-26.)
static bool hexN(uint8_t* dst, int n, const char* src) {
  for (int i = 0; i < 2 * n; i++) if (!isxdigit((unsigned char)src[i])) return false;
  for (int i = 0; i < n; i++) {
    auto v = [](char c) -> uint8_t { return (uint8_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); };
    dst[i] = (uint8_t)((v(src[2 * i]) << 4) | v(src[2 * i + 1]));
  }
  return true;
}

// BlImage.h reader over memory-mapped flash: ctx = base address
static void rdFlash(void* ctx, uint32_t off, void* dst, uint32_t n) { flashRead(dst, (uint32_t)ctx + off, n); }
static uint32_t uicrBl()  { return NRF_UICR->NRFFW[0]; }
static uint32_t uicrMbr() { return NRF_UICR->NRFFW[1]; }
static const uint32_t BL_TAIL_OFF = SLOT_PAYLOAD_MAX - BL_MAX_SIZE;   // a pulled bootloader is stored in the last 40 KiB of the slot payload area

// -------- CRC-16/CCITT for per-chunk integrity --------
static uint16_t crc16(const uint8_t* d, uint32_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)*d++ << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

// -------- (v1.17.28, security v2) keys and MACs --------
// Every LoRa-OTA packet ends with 8 bytes of HMAC-SHA256(key, packet). Keys: list (discovery) and offer = derived from the circle key C;
// everything after the offer = the session key of that pull (C + ECDH of the two identities + both nonces). The admin password is not used.
static void hmac(const uint8_t* key, size_t klen, const uint8_t* a, size_t alen, const uint8_t* b, size_t blen, uint8_t out[32]) {
  SHA256 h; h.resetHMAC(key, klen); h.update(a, alen); if (blen) h.update(b, blen); h.finalizeHMAC(key, klen, out, 32);
}
// PBKDF2-HMAC-SHA256, one 32-byte block (RFC 8018), in steps so `fw passphrase` can run it in the background:
// start = U1 (i = 1 done); steps = up to n more iterations; pbkdf2() = both at once (the host test checks it against known vectors)
static void pbkdf2Start(const char* pw, const uint8_t* salt, size_t slen, uint8_t u[32], uint8_t out[32]) {
  uint8_t blk[4] = {0, 0, 0, 1};
  hmac((const uint8_t*)pw, strlen(pw), salt, slen, blk, 4, u); memcpy(out, u, 32);
}
static uint32_t pbkdf2Steps(const char* pw, uint8_t u[32], uint8_t out[32], uint32_t i, uint32_t n, uint32_t iter) {
  const size_t pl = strlen(pw);
  for (uint32_t k = 0; k < n && i < iter; k++, i++) {
    hmac((const uint8_t*)pw, pl, u, 32, nullptr, 0, u);
    for (int j = 0; j < 32; j++) out[j] ^= u[j];
  }
  return i;
}
static void pbkdf2(const char* pw, const uint8_t* salt, size_t slen, uint32_t iter, uint8_t out[32]) {
  uint8_t u[32]; pbkdf2Start(pw, salt, slen, u, out);
  for (uint32_t i = 1; i < iter; ) { i = pbkdf2Steps(pw, u, out, i, 1024, iter); yield(); }
}
// AES-128-CTR over one chunk: counter block = session | chunk index | block number (a session key is used for one session only)
static void ctrXor(const uint8_t key[16], uint32_t sess, uint16_t idx, uint8_t* d, uint32_t n) {
  AES128 aes; aes.setKey(key, 16); uint8_t cb[16], ks[16];
  for (uint32_t off = 0, b = 0; off < n; off += 16, b++) {
    memset(cb, 0, 16); memcpy(cb, &sess, 4); memcpy(cb + 4, &idx, 2); cb[6] = (uint8_t)b;
    aes.encryptBlock(ks, cb);
    for (uint32_t k = 0; k < 16 && off + k < n; k++) d[off + k] ^= ks[k];
  }
}
void FwOtaTransfer::deriveKey(const char* pw, uint8_t out[16]) const { (void)pw; memcpy(out, _offer_key, 16); }   // v1.17.28: the circle's offer key
static bool isListMsg(uint8_t t) { return t == MSG_LIST_REQ || t == MSG_LIST_RSP; }
const uint8_t* FwOtaTransfer::keyForSend(uint8_t type, uint8_t* tmp) const {
  (void)tmp;
  if (!_circle_set) return nullptr;                       // hidden: no circle, no valid packet
  if (isListMsg(type)) return _list_key;
  if (type == MSG_OFFER_REQ2 || type == MSG_OFFER_NAK) return _offer_key;
  if (type == MSG_CHUNK_REQ || type == MSG_ABORT) return _sess_set ? _sess_mac : nullptr;
  return nullptr;                                         // server answers carry their session key explicitly (macFillKey)
}
const uint8_t* FwOtaTransfer::keyForRecv(uint8_t type, uint8_t* tmp) const {
  (void)tmp;
  if (!_circle_set) return nullptr;
  if (isListMsg(type)) return _list_key;
  if (type == MSG_OFFER_REQ2 || type == MSG_OFFER_NAK) return _offer_key;
  if (type == MSG_CHUNK) return _sess_set ? _sess_mac : nullptr;
  return nullptr;                                         // CHUNK_REQ / ABORT: the server's session table; OFFER_ACK: derived from the ACK
}
void FwOtaTransfer::macFillKey(uint8_t* pkt, uint32_t len, const uint8_t* k) const {
  if (!k) { memset(pkt + len - XFER_MAC_LEN, 0, XFER_MAC_LEN); return; }
  uint8_t full[32]; hmac(k, 16, pkt, len - XFER_MAC_LEN, nullptr, 0, full);
  memcpy(pkt + len - XFER_MAC_LEN, full, XFER_MAC_LEN);
}
void FwOtaTransfer::macFill(uint8_t* pkt, uint32_t len) const { macFillKey(pkt, len, keyForSend(pkt[1], nullptr)); }
bool FwOtaTransfer::macOk(const uint8_t* key, const uint8_t* pkt, uint32_t len) const {
  if (!key || len < XFER_MAC_LEN + 2) return false;
  uint8_t full[32]; hmac(key, 16, pkt, len - XFER_MAC_LEN, nullptr, 0, full);
  return memcmp(full, pkt + len - XFER_MAC_LEN, XFER_MAC_LEN) == 0;
}
// C -> the discovery channel (MeshCore group channel: secret + hash), the list key and the offer key
void FwOtaTransfer::circleKeys() {
  memset(&_disc_ch, 0, sizeof(_disc_ch)); memset(_list_key, 0, 16); memset(_offer_key, 0, 16);
  if (!_circle_set) return;
  uint8_t t[32];
  hmac(_circle, 32, (const uint8_t*)"lota2-disc", 10, nullptr, 0, t);
  memcpy(_disc_ch.secret, t, sizeof(_disc_ch.secret) < 32 ? sizeof(_disc_ch.secret) : 32);
  mesh::Utils::sha256(_disc_ch.hash, sizeof(_disc_ch.hash), _disc_ch.secret, sizeof(_disc_ch.secret));
  hmac(_circle, 32, (const uint8_t*)"lota2-list", 10, nullptr, 0, t);  memcpy(_list_key, t, 16);
  hmac(_circle, 32, (const uint8_t*)"lota2-offer", 11, nullptr, 0, t); memcpy(_offer_key, t, 16);
}
// session keys = HMAC(C, "lota2-sess" | ECDH(me, peer) | session | puller nonce | server nonce) -> MAC key | encryption key
bool FwOtaTransfer::sessionKeys(const uint8_t* peer_pub, uint32_t sess, const uint8_t* np, const uint8_t* ns, uint8_t* kmac, uint8_t* kenc) {
  if (!_circle_set || !_flood_host) return false;
  uint8_t m[10 + 32 + 4 + 8 + 8], k[32];
  memcpy(m, "lota2-sess", 10);
  if (!_flood_host->sharedSecret(m + 10, peer_pub)) return false;
  memcpy(m + 42, &sess, 4); memcpy(m + 46, np, 8); memcpy(m + 54, ns, 8);
  hmac(_circle, 32, m, sizeof(m), nullptr, 0, k);
  memcpy(kmac, k, 16); memcpy(kenc, k + 16, 16);
  memset(m, 0, sizeof(m)); memset(k, 0, sizeof(k));
  return true;
}

void FwOtaTransfer::send(const uint8_t* body, uint32_t len, uint32_t delay_ms, const uint8_t* route, uint8_t rlen) {
  if (!_mesh) return;
  mesh::Packet* p = _mesh->createRawData(body, len);
  if (!p) return;
  if (_flood_host && _flood_host->sendOta(p, route, rtCount(rlen) ? rlen : 0, delay_ms)) return;   // (v1.17.25) queued below all mesh traffic
  if (rtCount(rlen)) _mesh->sendDirect(p, route, rlen, delay_ms);   // stock repeaters forward direct packets along the path hashes (rlen = MeshCore path_len encoding)
  else      _mesh->sendZeroHop(p, delay_ms);
}

uint32_t FwOtaTransfer::addReturnRoute(uint8_t* b, uint32_t n) const {
  if (_ret_set) {                                     // a way back that is known to work (learnt from a flood that travelled server -> here), or typed
    b[n++] = _ret_len; memcpy(&b[n], _ret, rtBytes(_ret_len));
    return n + rtBytes(_ret_len);
  }
  b[n++] = _route_len;
  rtReverse(&b[n], _route, _route_len);
  return n + rtBytes(_route_len);
}

void FwOtaTransfer::setRet(const uint8_t* ret, uint8_t enc, bool typed) {
  if (!ret) { _ret_set = false; _ret_typed = false; _ret_len = 0; return; }
  if (rtBytes(enc)) memcpy(_ret, ret, rtBytes(enc));
  _ret_len = enc; _ret_set = true; _ret_typed = typed;
}

void FwOtaTransfer::floodOut(mesh::Packet* p, uint32_t delay_ms) {
  if (!p) return;
  uint8_t hs = _hash_mode ? (uint8_t)(*_hash_mode + 1) : 2;   // the node's path.hash.mode, but never 1-byte: two repeaters sharing a first byte would misroute
  if (hs < 2) hs = 2; if (hs > 3) hs = 3;
  if (_flood_host) _flood_host->floodScoped(p, delay_ms, hs);   // with the node's default region, like the repeater's own floods
  else _mesh->sendFlood(p, delay_ms, hs);                       // the collected path (MeshCore encoding) is directly our route format
}

// -------- key / state persistence --------
void FwOtaTransfer::saveState() {
  if (!_fs) return;
  _fs->remove(STATE_FILE);
  File f = _fs->open(STATE_FILE, FILE_O_WRITE);
  if (!f) return;
  uint32_t magic = STATE_MAGIC; uint8_t er = _erased;
  f.write((uint8_t*)&magic, 4);
  f.write((uint8_t*)&_session, 4);
  f.write(&er, 1);                        // payload area erased (chunks may be programmed) - the chunk map itself is rebuilt from flash
  f.write((uint8_t*)&_man, sizeof(_man));
  f.write(&_route_len, 1); f.write(_route, MAX_ROUTE_BYTES);
  uint8_t rs = (_ret_set ? 1 : 0) | (_ret_typed ? 2 : 0);
  f.write(&rs, 1); f.write(&_ret_len, 1); f.write(_ret, MAX_ROUTE_BYTES);
  f.write(&_pull_duty_pct, 1);            // v8: the pull's duty override (0 = the node's duty) survives a reboot
  f.close();
}
bool FwOtaTransfer::loadState() {
  if (!_fs || !_fs->exists(STATE_FILE)) return false;
  File f = _fs->open(STATE_FILE);
  if (!f) return false;
  uint32_t magic = 0; uint8_t er = 0;
  bool ok = f.read((uint8_t*)&magic, 4) == 4 && (magic == STATE_MAGIC || magic == STATE_MAGIC_V7 || magic == STATE_MAGIC_V6)
         && f.read((uint8_t*)&_session, 4) == 4
         && f.read(&er, 1) == 1
         && f.read((uint8_t*)&_man, sizeof(_man)) == (int)sizeof(_man)
         && f.read(&_route_len, 1) == 1 && f.read(_route, MAX_ROUTE_BYTES) == (int)MAX_ROUTE_BYTES && rtValid(_route_len);
  setRet(nullptr, 0, false); _pull_duty_pct = 0;
  if (ok && magic != STATE_MAGIC_V6) {                // v7+: the return route of the pair (a v6 file from an older version has none: reverse)
    uint8_t rs = 0, rl = 0, rb[MAX_ROUTE_BYTES];
    ok = f.read(&rs, 1) == 1 && f.read(&rl, 1) == 1 && f.read(rb, MAX_ROUTE_BYTES) == (int)MAX_ROUTE_BYTES && rtValid(rl);
    if (ok && (rs & 1)) setRet(rb, rl, (rs & 2) != 0);
  }
  if (ok && magic == STATE_MAGIC) {                   // v8: the pull's duty override
    uint8_t d = 0;
    if (f.read(&d, 1) == 1 && d >= DUTY_MIN_PCT && d <= 100) _pull_duty_pct = d;
  }
  f.close();
  _erased = ok && er;
  if (ok) routeReset();
  return ok;
}
void FwOtaTransfer::clearState() { if (_fs) _fs->remove(STATE_FILE); }

// The software clock restarts at the build date on every reboot, so after an automatic apply the node's adverts would carry
// an OLDER timestamp than its last one and every phone / companion that knows the node drops them as replays until an admin
// runs `clock sync`. Keep the time across the apply instead: forward-only, so an RTC module or a later sync is never overridden.
void FwOtaTransfer::saveClock() {
  if (!_fs || !_mesh) return;
  uint32_t t = _mesh->getRTCClock()->getCurrentTime();
  _fs->remove(CLOCK_FILE);
  File f = _fs->open(CLOCK_FILE, FILE_O_WRITE);
  if (f) { f.write((const uint8_t*)&t, 4); f.close(); }
}
void FwOtaTransfer::restoreClock() {
  if (!_fs || !_mesh || !_fs->exists(CLOCK_FILE)) return;
  uint32_t t = 0;
  File f = _fs->open(CLOCK_FILE);
  if (f) { if (f.read((uint8_t*)&t, 4) != 4) t = 0; f.close(); }
  _fs->remove(CLOCK_FILE);
  if (t == 0) return;
  t += 30;                                                      // the bootloader's apply + boot take well under this
  if (t > _mesh->getRTCClock()->getCurrentTime()) _mesh->getRTCClock()->setCurrentTime(t);
}

void FwOtaTransfer::fail(const char* why) {
  strncpy(_last_err, why, sizeof(_last_err) - 1);
  if (_paused) setPaused(false, 0);                      // (v1.17.28, C8)
  _phase = PH_FAILED;
  _resuming = false;
  clearPullKey();
  clearState();
}

// A persisted pull is only resumed if it still makes sense: sane manifest, and the slot is NOT valid (a valid slot
// means something else completed or replaced it since).
bool FwOtaTransfer::stateUsable() const {
  if (_man.n_chunks == 0) return _man.name[0] != 0;          // a pull that has not met its server yet: just retry the request
  bool bl = _man.kind == KIND_BOOTLOADER;
  return _man.chunk_size == XFER_CHUNK && _man.payload_len > 0 && _man.payload_len <= SLOT_PAYLOAD_MAX
      && _man.n_chunks == (_man.payload_len + XFER_CHUNK - 1) / XFER_CHUNK && _man.n_chunks <= ChunkMap::MAX_CHUNKS
      && (bl ? (_man.payload_len <= BL_MAX_SIZE && _man.dst_off + _man.payload_len <= SLOT_PAYLOAD_MAX && (_man.dst_off % FLASH_PAGE_SIZE) == 0)
             : (_man.dst_off == 0 && !slotValid()));
}

// Same served image? The NAME is deliberately not compared ('*' vs 'fw-10'); everything that defines the bytes is.
bool FwOtaTransfer::sameImage(const XferManifest& a, const XferManifest& b) const {
  return a.kind == b.kind && a.payload_len == b.payload_len && a.raw_len == b.raw_len
      && a.payload_crc32 == b.payload_crc32 && a.raw_crc32 == b.raw_crc32
      && a.fw_version == b.fw_version && a.compression == b.compression
      && a.chunk_size == b.chunk_size && a.n_chunks == b.n_chunks
      && memcmp(a.sha256, b.sha256, sizeof(a.sha256)) == 0
      && memcmp(a.lzma_props, b.lzma_props, sizeof(a.lzma_props)) == 0;
}

// (v1.17.28) the next wait between offers while the server does not answer: 4 s doubling to 60 s, and after WAIT_LONG_AFTER unanswered
// offers on to 5 min (an answer ends the wait; a new stall starts again at 4 s)
static uint32_t waitBackoffMs(uint32_t prev, uint16_t attempts) {
  if (!prev) return OFFER_RETRY_MS;
  const uint32_t cap = attempts >= WAIT_LONG_AFTER ? WAIT_BACKOFF_LONG_MS : WAIT_BACKOFF_MAX_MS;
  return prev * 2 > cap ? cap : prev * 2;
}

// The server went quiet / the link dropped mid-pull: do NOT give up. Keep the partial slot and the state, and go back to
// asking for an OFFER with exponential backoff; if the server comes back with the same image the pull continues from
// the first missing chunk.
void FwOtaTransfer::enterWait(const char* why) {
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
  _saved_man = _man;
  _win_count = 0;
  _prev_session = _session;                             // (v1.17.25) the resume runs under a NEW session, announced in its offers (the duty is agreed
  _session = ((uint32_t)millis() << 8) ^ 0xC3C3 ^ _cm.have; _sess_set = false;   // for it; the old one's share is freed) - before, it changed only at the answer
  _resuming = true;                                     // the chunks already in the slot are kept (the map is in RAM and in flash)
  _phase = PH_OFFERING;
  _attempts = 0; _backoff_ms = 0; _retries = 0; _last_req_ms = 0; _retry_timeout_ms = 0;
  _wait_started_ms = millis();                          // the one-week limit counts CONTINUOUS silence from here
  _next_disc_ms = millis() + DISC_FIRST_MS; _disc_interval_ms = 0;   // a relay may have gone: look for a new path to the server
}

void FwOtaTransfer::restartFromScratch(const char* why) {
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
  clearState();
  char keep[sizeof(_man.name)]; memcpy(keep, _man.name, sizeof(keep));   // remember what the user asked for
  uint8_t keep_kind = _man.kind;
  memset(&_man, 0, sizeof(_man)); memcpy(_man.name, keep, sizeof(keep)); _man.kind = keep_kind;
  _cm.reset(0); _erased = false; _win_count = 0; _crc_bad = 0; _crc_bad_idx = -1;
  _resuming = false; _retries = 0; _last_req_ms = 0; _retry_timeout_ms = 0;
  _prev_session = _session;
  _session = ((uint32_t)millis() << 8) ^ 0xA5A5; _sess_set = false;
  _phase = PH_OFFERING;
}

void FwOtaTransfer::retryOrFail(const char* why) {
  if (_verify_retries < MAX_VERIFY_RETRIES) {
    _verify_retries++;
    char msg[sizeof(_last_err)]; snprintf(msg, sizeof(msg), "%s - retry %u", why, _verify_retries);
    restartFromScratch(msg);
  } else {
    fail(why);
  }
}

void FwOtaTransfer::stopServing() { _serving = false; _probe_valid = false; }   // not persistent: auto-serve re-arms when the slot is (again) valid

// Server: is the slot still the image we advertised? (cheap: header only). If not, stop serving rather than send
// bytes that no longer match the manifest.
bool FwOtaTransfer::imageStillServed() {
  SlotHeader h;
  if (readSlotHeader(h) && h.magic == SLOT_MAGIC && h.payload_crc32 == _serve_man.payload_crc32
      && h.payload_len == _serve_man.payload_len && h.fw_version == _serve_man.fw_version) return true;
  stopServing();
  return false;
}

// A node offers an image only when it has proven it can run it itself: the application region holds exactly the slot's image
// (the bootloader applied it, or it was flashed identically). A freshly pulled/staged image that is still waiting for its apply,
// one the bootloader refused (no tag, bad decode ...) or one whose apply keeps being interrupted is therefore NOT served.
bool FwOtaTransfer::installedImage(const SlotHeader& h) const {
  return h.raw_len > 0 && h.raw_len <= APP_MAX_SIZE && crc32_flash(APP_REGION_START, h.raw_len) == h.raw_crc32;
}

// ------------------------------------------------------------------------
void FwOtaTransfer::begin(mesh::Mesh* mesh, FILESYSTEM* fs, const char* node_name, const uint8_t* pub_key, const char* admin_pw, const uint8_t* path_hash_mode) {
  _mesh = mesh; _fs = fs;
  _name_src = node_name; _pub = pub_key; _admin_pw = admin_pw; _hash_mode = path_hash_mode;
  memset(_peers, 0, sizeof(_peers));
  _noapply = _fs && _fs->exists(NOAPPLY_FILE);
  _pull_key_set = false;
  _serve_disabled = _fs && _fs->exists(NOSERVE_FILE);    // auto-serve is the default; only an explicit "fw serve off" disables it
  _serving = false; _next_autoserve_ms = 4000; _probe_valid = false;
  _resuming = false; _erased = false;
  _duty_pct = DUTY_DEFAULT_PCT;                            // (v1.17.25) the node's LoRa-OTA duty ("fw duty"), default 0.5
  if (_fs && _fs->exists(DUTY_FILE)) { File f = _fs->open(DUTY_FILE); uint8_t d = 0; if (f) { if (f.read(&d, 1) == 1 && d >= DUTY_MIN_PCT && d <= 100) _duty_pct = d; f.close(); } }
  _circle_set = false;                                     // (v1.17.28) the owner's circle: without it LoRa-OTA is hidden (serves/answers/pulls nothing)
  if (_fs && _fs->exists(CIRCLE_FILE)) { File f = _fs->open(CIRCLE_FILE); if (f) { _circle_set = f.read(_circle, 32) == 32; f.close(); } }
  circleKeys();                                            // discovery channel, list key, offer key - all from C
  _next_disc_ms = millis() + DISC_FIRST_MS;
  restoreClock();
  slotFromLota();                                          // (v1.17.28) a .lota just uploaded over BLE (`fw pull-ble`) becomes the slot
  if (loadState() && stateUsable()) {
    // a pull was in progress when we rebooted (or lost power): pick it up again once a server answers
    _saved_man = _man;
    deriveKey(nullptr, _pull_key); _pull_key_set = _circle_set;   // (v1.17.28) the circle's offer key
    bool fresh = (_man.n_chunks == 0);                       // no manifest yet: nothing to resume, only the request
    _resuming = !fresh;
    _cm.reset(_man.n_chunks);
    if (!fresh && _erased)                                   // which chunks made it into flash before the reboot / power cut
      _cm.rebuild([&](uint16_t i) { uint32_t off = (uint32_t)i * XFER_CHUNK; return slotPayloadPresent(_man.dst_off + off, umin(XFER_CHUNK, _man.payload_len - off)); });
    _phase = PH_OFFERING;
    if (!fresh && _erased && _cm.complete()) { _phase = PH_VERIFYING; _resuming = false; }   // every chunk made it before the reboot: verify (and apply) without a server
    _prev_session = _session;                                  // (v1.17.25) the one before the reboot (from the state file): its share is freed
    _session = ((uint32_t)millis() << 8) ^ 0x5A5A; _sess_set = false;              // fresh session id for this boot
    _seq = 0; _retries = 0; _attempts = 0; _backoff_ms = 0; _last_req_ms = 0; _retry_timeout_ms = 0;
    _wait_started_ms = millis();
    strncpy(_last_err, fresh ? "request restored after reboot" : "resuming after reboot", sizeof(_last_err) - 1);
    if (_fs && _fs->exists(PAUSE_FILE)) {                  // (v1.17.28, C8) it was paused: stays paused; a timed pause restarts its timer
      File f = _fs->open(PAUSE_FILE); uint8_t h = 0; if (f) { f.read(&h, 1); f.close(); }   //   (no RTC: the clock restarts on a reboot)
      _paused = true; _pause_until_ms = h ? millis() + (uint32_t)h * 3600000UL : 0; _pause_h = h;
      if (_phase == PH_VERIFYING) setPaused(false, 0);     //   every chunk is in: nothing to pause
    }
  } else {
    clearState();
    if (_fs) _fs->remove(PAUSE_FILE);
  }
  if (_fs && _fs->exists(RESOLVE_FILE)) {                  // a "fw pull AABB[CC]" was still looking for its server
    File f = _fs->open(RESOLVE_FILE); uint32_t m = 0; uint8_t bl = 0;
    bool ok = f && f.read((uint8_t*)&m, 4) == 4 && m == RESOLVE_MAGIC && f.read(_res_id, 3) == 3 && f.read(&_res_len, 1) == 1
              && f.read(&bl, 1) == 1 && f.read(_res_key, 16) == 16 && (_res_len == 2 || _res_len == 3);
    if (f) f.close();
    if (ok && _phase == PH_IDLE) { _resolving = true; _res_bl = bl; _res_next_ms = millis() + 15000; _res_interval_ms = 0; }
    else clearResolve();
  }
}

// Bootloader offer: the node's OWN bootloader flash, from BL_ADDR to the last programmed byte, uncompressed. Offered only if it passes the very
// checks a receiver applies (vectors, identity descriptor, layout, board) - so a node never advertises something its neighbours would refuse.
void FwOtaTransfer::armBlServe() {
  _bl_probed = true; _bl_serving = false; _own_bl_rev = 0; _bl_last_idx = 0; _bl_offer_reqs = 0;
  uint32_t len = blImageLength(rdFlash, (void*)BL_ADDR);
  BlDesc d;
  if (len && blFindDesc(rdFlash, (void*)BL_ADDR, len, &d) == BL_OK) _own_bl_rev = d.bl_rev;
  if (!len || blImageCheck(rdFlash, (void*)BL_ADDR, len, g_fwpull_tag.board, uicrBl(), uicrMbr(), &d) != BL_OK) return;
  XferManifest& m = _bl_man; memset(&m, 0, sizeof(m));
  snprintf(m.name, sizeof(m.name), "bl-%lu", (unsigned long)d.bl_rev);
  m.payload_len = m.raw_len = len;
  m.payload_crc32 = m.raw_crc32 = crc32_flash(BL_ADDR, len);
  m.fw_version = d.bl_rev; m.compression = COMP_NONE; m.chunk_size = XFER_CHUNK; m.n_chunks = (len + XFER_CHUNK - 1) / XFER_CHUNK; m.kind = KIND_BOOTLOADER;
  SHA256 sh; static uint8_t buf[512]; sh.reset();
  for (uint32_t off = 0; off < len; off += sizeof(buf)) { uint32_t n = umin(len - off, sizeof(buf)); flashRead(buf, BL_ADDR + off, n); sh.update(buf, n); }
  sh.finalize(m.sha256, sizeof(m.sha256));
  _bl_serving = !_serve_disabled && _circle_set;            // (v1.17.28) hidden without a circle
}

// The last step of a bootloader update. The image sits verified in the slot's tail (or its start). The MBR does the copy: it erases and rewrites
// the bootloader pages and records its progress in the MBR params page, so after a power cut it FINISHES the copy at the next boot, before any
// bootloader or application code runs - the slot must simply not be touched until then (nothing does: we reset straight away).
void FwOtaTransfer::blApplyNow() {
  uint32_t len = _man.payload_len;
  if (crc32_flash(dstAddr(0), len) != _man.payload_crc32) { fail("bootloader image changed before the copy - not applied"); return; }
  clearState(); clearPullKey();                          // from here the copy is the MBR's business; a reboot must not start another pull/verify/apply
  saveClock();
  Serial.println("fw_ota: bootloader verified - copying it (MBR), the node restarts"); Serial.flush();
  uint8_t sd = 0; sd_softdevice_is_enabled(&sd);
  if (sd) sd_softdevice_disable();
  sd_mbr_command_t c; memset(&c, 0, sizeof(c));
  c.command = SD_MBR_COMMAND_COPY_BL;
  c.params.copy_bl.bl_src = (uint32_t*)dstAddr(0);
  c.params.copy_bl.bl_len = len / 4;
  uint32_t r = sd_mbr_command(&c);
  Serial.print("fw_ota: MBR copy returned "); Serial.println(r); Serial.flush(); delay(200);
  NVIC_SystemReset();
}

// ======================= CLI ============================================
// (v1.17.28-rc4, C11) `fw serve` without an argument: what this node offers now, and why not more
bool FwOtaTransfer::cmdServeStatus(char* reply) {
  if (!_circle_set) { strcpy(reply, "not serving: no passphrase ('fw passphrase <phrase>')"); return true; }
  if (_serve_disabled) { strcpy(reply, "not serving: 'fw serve off' ('fw serve on' offers the slot image and the bootloader)"); return true; }
  char app[48], bl[24];
  if (_serving && imageStillServed()) snprintf(app, sizeof(app), "app %s", _serve_man.name);
  else {
    SlotHeader h; const bool v = readSlotHeader(h) && slotValid();
    strcpy(app, !v ? "no app (slot empty)" : "no app (slot image not installed here)");
  }
  if (_bl_serving) snprintf(bl, sizeof(bl), "bootloader rev %lu", (unsigned long)_bl_man.fw_version); else strcpy(bl, "no bootloader");
  snprintf(reply, 160, "serving: %s + %s ('fw serve off' stops both)", app, bl);
  return true;
}

// `fw serve on` (rc4, C9: serve what is available): the bootloader always, the slot image when it is valid and installed here (or BLE-filled)
bool FwOtaTransfer::cmdServe(char* reply, bool force, bool user) {
  if (!_circle_set) { strcpy(reply, "not serving: set 'fw passphrase <phrase>' first (LoRa-OTA serves only the owner's circle)"); return true; }
  if (user) {                                           // the owner switches serving on: the bootloader offer is (re)armed whatever the slot holds
    _serve_disabled = false; _bl_probed = false;
    if (_fs) _fs->remove(NOSERVE_FILE);
  }
  SlotHeader h;
  if (!readSlotHeader(h) || !slotValid()) { stopServing(); strcpy(reply, "OK - serving the bootloader only (slot empty); a pull or 'fw pull-ble' fills the slot"); return true; }
  if (!force && !installedImage(h) && !bleFilled(h)) {   // (v1.17.28) an image the owner put here over BLE is served without being installed
    stopServing();
    strcpy(reply, "OK - serving the bootloader only: the slot image is not installed here (yet) - offered once applied ('fw serve force' = bench)");
    return true;
  }

  memset(&_serve_man, 0, sizeof(_serve_man));
  snprintf(_serve_man.name, sizeof(_serve_man.name), "fw-%lu", (unsigned long)h.fw_version);
  _serve_man.payload_len   = h.payload_len;
  _serve_man.raw_len       = h.raw_len;
  _serve_man.payload_crc32 = h.payload_crc32;
  _serve_man.raw_crc32     = h.raw_crc32;
  _serve_man.fw_version    = h.fw_version;
  _serve_man.compression   = h.compression;
  memcpy(_serve_man.lzma_props, h.lzma_props, sizeof(_serve_man.lzma_props));
  _serve_man.chunk_size    = XFER_CHUNK;
  _serve_man.n_chunks      = (h.payload_len + XFER_CHUNK - 1) / XFER_CHUNK;

  // SHA-256 over the slot payload (streamed from flash)
  SHA256 sh; static uint8_t buf[512];
  sh.reset();
  for (uint32_t off = 0; off < h.payload_len; ) {
    uint32_t n = (h.payload_len - off) < sizeof(buf) ? (h.payload_len - off) : (uint32_t)sizeof(buf);
    flashRead(buf, SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off, n);
    sh.update(buf, n);
    off += n;
    if ((off & 0x1FFF) == 0) yield();
  }
  sh.finalize(_serve_man.sha256, sizeof(_serve_man.sha256));

  _serving = true;
  _serve_last_idx = 0; _serve_reqs = 0; _serve_offer_reqs = 0; _serve_offer_nomatch = 0;
  _mac_fail = 0;
  snprintf(reply, 160, "OK - serving '%s' (%lu B, %lu chunks) + the bootloader",
           _serve_man.name,
           (unsigned long)_serve_man.payload_len, (unsigned long)_serve_man.n_chunks);
  return true;
}

bool FwOtaTransfer::cmdServeOff(char* reply) {
  stopServing(); _bl_serving = false;
  _serve_disabled = true;
  if (_fs) { _fs->remove(NOSERVE_FILE); File f = _fs->open(NOSERVE_FILE, FILE_O_WRITE); if (f) { f.write((const uint8_t*)"1", 1); f.close(); } }
  strcpy(reply, "OK - serving nothing (app and bootloader), also after a reboot ('fw serve on' re-enables)");
  return true;
}

void FwOtaTransfer::writePullKey() {
  if (_fs) _fs->remove(PK_FILE);                          // (v1.17.28) nothing to persist: the offer key comes from the circle at every boot
}
void FwOtaTransfer::clearPullKey() {
  _pull_key_set = false; memset(_pull_key, 0, sizeof(_pull_key));
  if (_fs) _fs->remove(PK_FILE);
}

// scripted / bench form: '*' or a served name, authenticated with THIS node's own admin password (same fleet password)
bool FwOtaTransfer::cmdPull(const char* name, char* reply) {
  if (!_circle_set) { strcpy(reply, "ERR - set 'fw passphrase <phrase>' first (the same on every node of your circle)"); return true; }
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - pull already in progress ('fw pull-abort' to cancel)"); return true; }
  deriveKey(_admin_pw, _pull_key); _pull_key_set = true; writePullKey();
  startPullAt(name, reply, false);
  return true;
}

void FwOtaTransfer::startPullAt(const char* name, char* reply, bool bl) {
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - pull already in progress ('fw pull-abort' to cancel)"); return; }
  if (_staging) { strcpy(reply, "ERR - stage in progress"); return; }
  _apply_armed = false;
  _resuming = false; _verify_retries = 0; _bad_auth = 0; _cm.reset(0); _erased = false;
  if (!bl) stopServing();   // we are about to rewrite the slot (a bootloader goes to its tail and leaves the app copy alone: decided when the manifest arrives)
  memset(&_man, 0, sizeof(_man));
  strncpy(_man.name, name && *name ? name : "*", sizeof(_man.name) - 1);
  _man.kind = bl ? KIND_BOOTLOADER : KIND_APP;
  _session   = ((uint32_t)millis() << 8) ^ 0xA5A5; _sess_set = false;
  _seq = 0; _retries = 0; _last_req_ms = 0; _win_start = 0;
  _attempts = 0; _backoff_ms = 0; _retry_timeout_ms = 0; _wait_started_ms = millis();
  _mac_fail = 0; _next_disc_ms = millis() + DISC_FIRST_MS; _disc_interval_ms = 0;
  _pull_duty_pct = _next_pull_duty; _next_pull_duty = 0; _agreed_pct = 0;   // "fw pull ... duty x" (0 = the node's duty); agreed at the offer
  _prev_session = 0; _typed_failed = false; _last_ans_ms = 0;
  _phase = PH_OFFERING;
  _last_err[0] = 0; _res_msg[0] = 0;
  routeReset();
  saveState();   // the request itself survives a reboot / power cut: it is retried until a server answers
  strcpy(reply, bl ? "OK - fetching the bootloader; it runs unattended, and once verified the node copies it and restarts ('fw pull-status')"
                   : "OK - pulling; it runs unattended and applies itself when verified ('fw pull-status')");
}

bool FwOtaTransfer::cmdStatus(char* reply) {
  if (_resolving) {
    char hex[8]; snprintf(hex, sizeof(hex), "%02X%02X", _res_id[0], _res_id[1]); if (_res_len == 3) snprintf(hex + 4, 4, "%02X", _res_id[2]);
    if (_res_scanning || !_res_scans) snprintf(reply, 160, "looking for %s%s: scanning (scan %u)...", hex, _res_bl ? " (bootloader)" : "", (unsigned)(_res_scans ? _res_scans : 1));
    else snprintf(reply, 160, "looking for %s%s: not in reach yet after %u scan(s) - next in %lus ('fw pull-abort' stops)", hex, _res_bl ? " (bootloader)" : "",
                  (unsigned)_res_scans, (unsigned long)((_res_next_ms - millis()) / 1000));
    return true;
  }
  if (_phase == PH_FAILED && _res_msg[0]) { snprintf(reply, 160, "FAILED: %s", _res_msg); return true; }
  const char* ph = _phase == PH_IDLE ? "idle" : _phase == PH_OFFERING ? "offering"
                 : _phase == PH_DOWNLOADING ? "downloading" : _phase == PH_VERIFYING ? "verifying"
                 : _phase == PH_COMPLETE ? "COMPLETE" : "FAILED";
  if (_paused && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING)) {   // (v1.17.28, C8)
    char left[24] = "until fw pull-resume";
    if (_pause_until_ms) { uint32_t m = (_pause_until_ms - millis()) / 60000; snprintf(left, sizeof(left), "resumes in %luh%02lum", (unsigned long)(m / 60), (unsigned long)(m % 60)); }
    snprintf(reply, 160, "PAUSED '%s' : %lu/%lu chunks kept, %s (nothing is sent)", _man.name, (unsigned long)_cm.have, (unsigned long)_man.n_chunks, left);
    return true;
  }
  if (_phase == PH_OFFERING && _resuming) {
    snprintf(reply, 160, "WAITING for server '%s' : %lu/%lu chunks kept, attempt %u, waited %lus, hops %u/%u, %s %s",
             _man.name, (unsigned long)_cm.have, (unsigned long)_man.n_chunks, _attempts,
             (unsigned long)((millis() - _wait_started_ms) / 1000), (unsigned)rtCount(_route_len), (unsigned)rtCount(_ret_set ? _ret_len : _route_len),
             lastAnsText().c, _last_err);
    return true;
  }
  if (_phase == PH_OFFERING && !_resuming) {
    if (_retries >= MAX_RETRIES)
      snprintf(reply, 160, "offering '%s' : no answer yet, attempt %u, waited %lus, %s - out of reach or another passphrase? (retrying)",
               _man.name, _attempts, (unsigned long)((millis() - _wait_started_ms) / 1000), lastAnsText().c);
    else
      snprintf(reply, 160, "offering '%s' : asking the server (try %u)%s%s", _man.name, _retries, _last_err[0] ? " - " : "", _last_err);
    return true;
  }
  if (_phase == PH_DOWNLOADING || _phase == PH_VERIFYING || _phase == PH_COMPLETE || _phase == PH_FAILED) {
    uint32_t pct = _man.n_chunks ? (100 * (uint32_t)_cm.have / _man.n_chunks) : 0;
    char eta[28] = "";
    if (_phase == PH_DOWNLOADING) {                       // rate since the download (re)started -> time left for the missing chunks
      uint32_t el_s = (millis() - _rate_t0_ms) / 1000, got = _cm.have - _rate_have0, left = _man.n_chunks - _cm.have;
      if (el_s >= 20 && got >= 3) {
        uint32_t eta_s = (uint32_t)((uint64_t)left * el_s / got);
        if (eta_s >= 3600) snprintf(eta, sizeof(eta), " %lu.%luc/s ETA %luh%02lum", (unsigned long)(got / el_s), (unsigned long)((got * 10 / el_s) % 10), (unsigned long)(eta_s / 3600), (unsigned long)((eta_s / 60) % 60));
        else               snprintf(eta, sizeof(eta), " %lu.%luc/s ETA %lum%02lus", (unsigned long)(got / el_s), (unsigned long)((got * 10 / el_s) % 10), (unsigned long)(eta_s / 60), (unsigned long)(eta_s % 60));
      } else strcpy(eta, " ETA measuring");
    }
    const uint8_t dp = _agreed_pct ? _agreed_pct : askedPct();
    snprintf(reply, 160, "%s '%s' : %lu/%lu chunks (%lu%%)%s duty %u.%02u win=%u retries=%u crc_bad=%lu@%ld mac_fail=%lu hops %u/%u %s",
             ph, _man.name,
             (unsigned long)_cm.have, (unsigned long)_man.n_chunks, (unsigned long)pct, eta, dp / 100, dp % 100,
             _win_target, _retries, (unsigned long)_crc_bad, (long)_crc_bad_idx,
             (unsigned long)_mac_fail, (unsigned)rtCount(_route_len), (unsigned)rtCount(_ret_set ? _ret_len : _route_len), _last_err);
    if (_phase == PH_COMPLETE && _serving) { size_t l = strlen(reply); snprintf(reply + l, l < 159 ? 160 - l : 0, " | serving"); }
  } else if (_serving) {
    uint32_t pct = _serve_man.n_chunks ? (100 * _serve_last_idx / _serve_man.n_chunks) : 0;
    snprintf(reply, 160, "%s | serving '%s' : offers=%lu(%lu nomatch) mac_fail=%lu sent up to chunk %lu/%lu (%lu%%) reqs=%lu heals=%lu",
             ph, _serve_man.name, (unsigned long)_serve_offer_reqs, (unsigned long)_serve_offer_nomatch,
             (unsigned long)_mac_fail,
             (unsigned long)_serve_last_idx, (unsigned long)_serve_man.n_chunks,
             (unsigned long)pct, (unsigned long)_serve_reqs, (unsigned long)_heals_sent);
  } else {
    const bool ble_msg = _phase == PH_IDLE && strncmp(_last_err, "BLE upload", 10) == 0;   // (v1.17.28) the result of the last `fw pull-ble`
    snprintf(reply, 160, "%s%s%s%s", ph, _serve_disabled ? " | auto-serve off" : "", ble_msg ? " | " : "", ble_msg ? _last_err : "");
  }
  if (_bl_offer_reqs && _phase != PH_DOWNLOADING && _phase != PH_VERIFYING) {   // (v1.17.26, A14) the bootloader offer's own counters
    size_t l = strlen(reply);
    if (l < 159) snprintf(reply + l, 160 - l, " | bl offers=%lu up to %lu/%lu", (unsigned long)_bl_offer_reqs, (unsigned long)_bl_last_idx, (unsigned long)_bl_man.n_chunks);
  }
  return true;
}

bool FwOtaTransfer::cmdBlInfo(char* reply) {
  if (!_bl_probed) armBlServe();
  uint32_t len = blImageLength(rdFlash, (void*)BL_ADDR);
  snprintf(reply, 160, "bootloader rev %lu, %lu B, crc %08lX, uicr %lX/%lX, %s", (unsigned long)_own_bl_rev, (unsigned long)len,
           (unsigned long)crc32_flash(BL_ADDR, len), (unsigned long)uicrBl(), (unsigned long)uicrMbr(),
           _bl_serving ? "offered to neighbours" : _serve_disabled ? "not offered (fw serve off)" : "not offered (no valid identity descriptor)");
  return true;
}

bool FwOtaTransfer::cmdAbort(char* reply) {
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) {
    uint8_t b[16]; b[0] = XFER_MAGIC; b[1] = MSG_ABORT;
    memcpy(&b[2], &_session, 4); memset(&b[6], 0, 2);
    macFill(b, 16); send(b, 16, 100, _route, _route_len);
  }
  const bool was_res = _resolving;
  _phase = PH_IDLE; _resuming = false; clearState(); clearPullKey(); clearResolve(); _res_msg[0] = 0;
  setPaused(false, 0);                                   // (v1.17.28, C8)
  strcpy(reply, was_res ? "OK - search for the short id stopped" : "OK - aborted");
  return true;
}

// (v1.17.28, C8) pause / resume a pull. Paused = a stall the owner chose: the chunks and the pull state are kept, the server's duty share
// is freed (ABORT for our session), and the node sends nothing until 'fw pull-resume' or the timer. One flash write each way.
void FwOtaTransfer::setPaused(bool on, uint8_t hours) {
  _paused = on; _pause_h = on ? hours : 0; _pause_until_ms = (on && hours) ? millis() + (uint32_t)hours * 3600000UL : 0;
  if (!_fs) return;
  _fs->remove(PAUSE_FILE);
  if (on) { File f = _fs->open(PAUSE_FILE, FILE_O_WRITE); if (f) { f.write(&hours, 1); f.close(); } }
}
void FwOtaTransfer::resumePull(const char* why) {
  setPaused(false, 0);
  _attempts = 0; _backoff_ms = 0; _retries = 0; _last_req_ms = 0; _retry_timeout_ms = 0;
  _wait_started_ms = millis();                          // the one-week limit counts silence, not the pause
  _next_disc_ms = millis() + DISC_FIRST_MS; _disc_interval_ms = 0;
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
}
bool FwOtaTransfer::cmdPause(const char* arg, char* reply) {
  if (_phase != PH_OFFERING && _phase != PH_DOWNLOADING) { strcpy(reply, "ERR - no pull to pause"); return true; }
  const int h = pauseHours(arg);
  if (h < 0) { strcpy(reply, "usage: fw pull-pause [Nh]  (N = 1..168 hours; no N = until 'fw pull-resume')"); return true; }
  if (_phase == PH_DOWNLOADING) {                       // free the server's share now; the pull resumes later like after a stall
    uint8_t b[16]; b[0] = XFER_MAGIC; b[1] = MSG_ABORT; memcpy(&b[2], &_session, 4); memset(&b[6], 0, 2);
    macFill(b, 16); send(b, 16, 100, _route, _route_len);
    enterWait("paused");
  }
  saveState();
  setPaused(true, (uint8_t)h);
  if (h) snprintf(reply, 160, "OK - paused at %lu/%lu chunks: nothing is sent; resumes by itself in %d h (or 'fw pull-resume')", (unsigned long)_cm.have, (unsigned long)_man.n_chunks, h);
  else   snprintf(reply, 160, "OK - paused at %lu/%lu chunks: nothing is sent until 'fw pull-resume' ('fw pull-abort' drops it)", (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
  return true;
}
bool FwOtaTransfer::cmdResume(char* reply) {
  if (!_paused) { strcpy(reply, "ERR - no paused pull"); return true; }
  resumePull("resumed");
  snprintf(reply, 160, "OK - resumed at %lu/%lu chunks: asking the server again", (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
  return true;
}

// ======================= local staging (serial) ========================
bool FwOtaTransfer::cmdStage(const char* args, char* reply) {
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - pull in progress"); return true; }
  if (_staging) { strcpy(reply, "ERR - stage already in progress"); return true; }
  stopServing();   // the slot is about to be replaced; whatever we advertised is no longer true

  unsigned long clen = 0, rlen = 0, fwver = 0;
  char rcrc_hex[16] = {0}, props_hex[16] = {0};
  int n = sscanf(args, "%lu %lu %15s %lu %15s", &clen, &rlen, rcrc_hex, &fwver, props_hex);
  if (n != 5 || clen == 0 || clen > SLOT_PAYLOAD_MAX || rlen == 0 || rlen > APP_MAX_SIZE
      || strlen(props_hex) != 10) {
    strcpy(reply, "usage: fw stage <clen> <rlen> <raw_crc32_hex> <fw_version> <lzma_props_hex10>");
    return true;
  }

  memset(&_stage_hdr, 0, sizeof(_stage_hdr));
  _stage_hdr.raw_len     = (uint32_t)rlen;
  _stage_hdr.raw_crc32   = (uint32_t)strtoul(rcrc_hex, nullptr, 16);
  _stage_hdr.fw_version  = (uint32_t)fwver;
  _stage_hdr.compression = COMP_LZMA;
  for (int i = 0; i < 5; i++) {
    char byte_str[3] = { props_hex[i * 2], props_hex[i * 2 + 1], 0 };
    _stage_hdr.lzma_props[i] = (uint8_t)strtoul(byte_str, nullptr, 16);
  }

  if (!_writer.begin()) { strcpy(reply, "ERR - header erase failed"); return true; }
  _staging = true;
  _stage_total = (uint32_t)clen;
  _stage_remaining = (uint32_t)clen;
  _stage_progress_at = 0;
  _stage_result_pending = false;
  snprintf(reply, 160, "OK - send %lu bytes now", (unsigned long)clen);
  return true;
}

void FwOtaTransfer::stageFeed(const uint8_t* data, uint32_t len) {
  if (!_staging || len == 0) return;
  bool ok = _writer.write(data, len);
  if (len <= _stage_remaining) _stage_remaining -= len; else _stage_remaining = 0;
  uint32_t received = _stage_total - _stage_remaining;

  // Plain progress line, no "  -> " prefix - the host's read_reply() only
  // treats "  -> "-prefixed lines as the actual result, so this is safe to
  // print freely without confusing it; it's here so a long stage (over a
  // channel this code owns exclusively - no room to interleave a status
  // command while it's running) is visibly making progress, not just silent
  // for however long it takes.
  if (received - _stage_progress_at >= 16384 || _stage_remaining == 0) {
    Serial.print("fw_ota: staged "); Serial.print(received);
    Serial.print("/"); Serial.print(_stage_total); Serial.println(" B");
    _stage_progress_at = received;
  }

  if (!ok || _stage_remaining == 0) {
    _staging = false;
    bool fin = ok && _writer.finish(_stage_hdr);
    bool valid = fin && slotValid();
    snprintf(_stage_result, sizeof(_stage_result), valid ? "OK - staged, VALID" : "ERR - stage failed");
    _stage_result_pending = true;
  }
}

// Called from loop(): while `fw stage` is active the serial byte stream is raw compressed image data, which must not be echoed or parsed
// as a command. Reads at most stageRemaining() bytes per call so nothing that belongs to the next command is consumed.
bool FwOtaTransfer::serviceStaging() {
  if (!_staging) return false;
  int avail = Serial.available();
  if ((uint32_t)avail > _stage_remaining) avail = (int)_stage_remaining;
  if (avail > 0) {
    uint8_t buf[256];
    if (avail > (int)sizeof(buf)) avail = (int)sizeof(buf);
    int n = Serial.readBytes(buf, avail);
    stageFeed(buf, (uint32_t)n);
    if (!_staging) {                                        // finished: the reply carries the "  -> " marker host scripts wait for
      char msg[64];
      if (stageTakeResult(msg, sizeof(msg))) { Serial.print("  -> "); Serial.println(msg); }
    }
  }
  return true;
}

bool FwOtaTransfer::stageTakeResult(char* out, size_t outsz) {
  if (!_stage_result_pending) return false;
  strncpy(out, _stage_result, outsz - 1);
  out[outsz - 1] = 0;
  _stage_result_pending = false;
  return true;
}

// ======================= tick ==========================================
void FwOtaTransfer::tick() {
  uint32_t now = millis();

  // auto-serve: any node holding a valid image in its slot offers it to its neighbours - after a boot, right after its own pull
  // completed, after a local stage. Not while pulling/staging (the slot is being rewritten). Cheap: header probe every 15 s.
  if (!_serving && !_serve_disabled && !_staging && (int32_t)(now - _next_autoserve_ms) >= 0
      && (_phase == PH_IDLE || _phase == PH_COMPLETE || _phase == PH_FAILED)) {
    _next_autoserve_ms = now + AUTOSERVE_POLL_MS;
    SlotHeader ah;
    if (readSlotHeader(ah) && ah.magic == SLOT_MAGIC) {
      // probe each distinct slot image once per boot (the CRC over slot + app is not free)
      uint32_t key = ah.payload_crc32 ^ (ah.raw_crc32 * 31u) ^ ah.fw_version ^ (ah.payload_len << 3);
      if (!_probe_valid || key != _probe_key) {
        _probe_valid = true; _probe_key = key;
        char tmp[160]; cmdServe(tmp, false, false);   // arms serving only if the slot is valid AND installed
        _probe_ok = _serving;
      } else if (_probe_ok) { char tmp[160]; cmdServe(tmp, false, false); }
    }
  }

  if (!_bl_probed && !_staging && now > 6000 && (_phase == PH_IDLE || _phase == PH_COMPLETE || _phase == PH_FAILED)) armBlServe();
  if (_reset_at_ms && (int32_t)(now - _reset_at_ms) >= 0) {   // (v1.17.28) `fw pull-ble` / `fw apply`: the reply has gone out, restart now
    saveClock();
    uint8_t sd = 0; sd_softdevice_is_enabled(&sd);
    if (sd) { sd_power_gpregret_clr(0, 0xffffffff); if (_reset_magic) sd_power_gpregret_set(0, _reset_magic); }
    else NRF_POWER->GPREGRET = _reset_magic;
    Serial.flush(); delay(100);
    NVIC_SystemReset();
  }
  if (_await_pw && now - _await_since_ms > PW_PROMPT_MS) _await_pw = false;   // the prompt expires silently
  if (_scan_active) {
    if (_scan_pending && (int32_t)(now - _scan_start_ms) >= 0) scanSend();   // (v1.17.28) after the command's reply
    else if (_scan_resend_ms && (int32_t)(now - _scan_resend_ms) >= 0) {   // ask the direct neighbours once more
      sendListReq(); _scan_resends++;
      _scan_resend_ms = scanResendAt(_scan_resends, now, (uint32_t)_mesh->getRNG()->nextInt(0, SCAN_REQ_SPREAD_MS + 1), _scan_end_ms);
    }
    if ((int32_t)(now - _scan_end_ms) >= 0) _scan_active = false;
  }
  resolveTick(now);
  serveTick(now);                                         // (rc6, C14) queued chunks, one at a time in quiet moments
  if (_pb_i) passphraseStep();                              // (v1.17.28) `fw passphrase` in the background
  if (_apply_armed) {
    if (_phase != PH_COMPLETE) _apply_armed = false;
    else if ((int32_t)(now - _apply_at_ms) >= 0) {
      _apply_armed = false;
      if (_man.kind == KIND_BOOTLOADER) blApplyNow();
      else if (ctrlMarkPending()) {
        saveClock();
        Serial.println("fw_ota: pull verified - rebooting to apply"); Serial.flush(); delay(300);
        NVIC_SystemReset();
      } else Serial.println("fw_ota: could not set PENDING - not applying");
    }
  }

  if (_paused) {                                            // (v1.17.28, C8) paused: no offer, no request, no discovery - nothing at all
    if (_pause_until_ms && (int32_t)(now - _pause_until_ms) >= 0) resumePull("timed pause over: resumed");
    else return;
  }
  uint8_t did[4];
  if ((_phase == PH_OFFERING || _phase == PH_DOWNLOADING) && (int32_t)(now - _next_disc_ms) >= 0
      && pullTargetId(did) && !floodBudget(now)) _next_disc_ms = now + 60000;   // (v1.17.27) flood budget used up: try again in a minute
  else if ((_phase == PH_OFFERING || _phase == PH_DOWNLOADING) && (int32_t)(now - _next_disc_ms) >= 0) {
    uint8_t id[4];
    if (pullTargetId(id)) {                               // ask the mesh for a (new) path to exactly this server
      char t[12]; snprintf(t, sizeof(t), "@%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
      _disc_sess = (now << 8) ^ 0x3D3D;
      sendDiscovery(t);
    }
    if (_phase == PH_DOWNLOADING) {                       // (v1.17.26) back off while the route in use has no unanswered request
      const uint16_t fails = (_rt_cur >= 0 && _routes[_rt_cur].used) ? _routes[_rt_cur].fail : 0xFFFF;
      const bool clean = fails != 0xFFFF && fails == _disc_fail_mark && _disc_interval_ms >= DISC_DOWNLOAD_MS;
      _disc_interval_ms = clean ? umin(_disc_interval_ms * 2, DISC_DOWNLOAD_MAX_MS) : DISC_DOWNLOAD_MS;
      _disc_fail_mark = fails;
      _next_disc_ms = now + _disc_interval_ms;
    }
    else {                                                // a week-long wait must not flood the mesh every 5 min: 5, 10, 20, 40, 60, 60 ... min
      _disc_interval_ms = _disc_interval_ms ? umin(_disc_interval_ms * 2, 12 * DISC_WAIT_MS) : DISC_WAIT_MS;
      _next_disc_ms = now + _disc_interval_ms;
    }
  }

  if (_phase == PH_OFFERING) {
    if (now - _last_req_ms > _retry_timeout_ms) {
      _last_req_ms = now;
      if (_resuming || _retries >= MAX_RETRIES) {
        // waiting for a server to (re)appear - after a stall, after a reboot, or because a NEW pull has not reached its server yet
        // (bad weather, low signal, server off): exponential backoff 4 s -> 60 s -> 5 min (with jitter) so a long outage costs almost no
        // airtime. There is no attempt limit; only a week of continuous silence ends it ('fw pull-abort' ends it any time).
        _backoff_ms = waitBackoffMs(_backoff_ms, _attempts);
        _retry_timeout_ms = _backoff_ms / 2 + _mesh->getRNG()->nextInt(0, _backoff_ms);
        if (now - _wait_started_ms > MAX_RESUME_WAIT_MS) { fail("server unavailable too long"); return; }
        _attempts++;
        if (_attempts > 1 && !_route_manual) { routeResult(false, 0); routeFailover(); }   // unanswered: the next offer tries another known route
        if (_attempts >= TYPED_FAIL_REDISCOVER && _route_manual && !_typed_failed) typedRouteFailed(now);   // (v1.17.26, N1) typed: flood re-discovery
      } else {
        // jittered, not a fixed interval - a fixed retry period can align in
        // lockstep with something else on the channel (including our own
        // retries) and collide with the same exchange every single time
        _retry_timeout_ms = OFFER_RETRY_MS / 2 + _mesh->getRNG()->nextInt(0, OFFER_RETRY_MS);
        _retries++;
        if (_retries >= TYPED_FAIL_REDISCOVER && _route_manual && !_typed_failed) typedRouteFailed(now);   // (v1.17.27) a fresh pull's retries count too
      }
      _seq++;   // vary the payload so a retry isn't dropped as an already-seen dup
      uint8_t b[8 + 1 + 4 + 32 + 8 + 1 + MAX_ROUTE_BYTES + 32 + XFER_MAC_LEN];
      uint32_t n = 0;
      b[n++] = XFER_MAGIC; b[n++] = MSG_OFFER_REQ2;        // (v1.17.25) with the duty this pull asks for; every (re)start renegotiates it
      memcpy(&b[n], &_session, 4); n += 4;
      memcpy(&b[n], &_seq, 2); n += 2;
      b[n++] = askedPct();
      memcpy(&b[n], &_prev_session, 4); n += 4;            // the session this one replaces (0 = none): the server frees its share
      if (_nonce_sess != _session) { _mesh->getRNG()->random(_nonce, sizeof(_nonce)); _nonce_sess = _session; }   // one nonce per session
      memcpy(&b[n], _pub, 32); n += 32;                     // (v1.17.28) our identity (ECDH) ...
      memcpy(&b[n], _nonce, 8); n += 8;                     // ... and nonce: the session key depends on both ends' nonces
      n = addReturnRoute(b, n);
      uint32_t nl = strlen(_man.name); if (nl > 31) nl = 31;
      memcpy(&b[n], _man.name, nl); n += nl;
      n += XFER_MAC_LEN;
      macFill(b, n); send(b, n, 50, _route, _route_len);
    }
    return;
  }

  if (_phase == PH_DOWNLOADING) {
    if (!_erased) { eraseStep(); return; }
    if (_cm.complete()) { _phase = PH_VERIFYING; return; }
    if (_req_pending) { requestWindow(); return; }      // (rc6, C14) a request waiting for a quiet moment - not a retry
    if (now - _last_req_ms > _retry_timeout_ms) {
      routeResult(false, 0);
      if (++_retries > MAX_RETRIES) {                   // keep what we have; do not fail
        enterWait("server silent - waiting");
        if (!_route_manual) _next_disc_ms = millis();     // every known route failed: flood a discovery for a new one right away
        else if (!_typed_failed) typedRouteFailed(millis());
        return;
      }
      if (!_route_manual && ++_rt_fail_run >= ROUTE_FAIL_SWITCH && routeFailover())
        snprintf(_last_err, sizeof(_last_err), "route switched: %u hop(s)", (unsigned)rtCount(_route_len));
      else if (_route_manual && ++_rt_fail_run >= TYPED_FAIL_REDISCOVER && !_typed_failed) typedRouteFailed(now);   // (v1.17.26, N1)
      // ease off for the NEXT window - this one's size is already fixed
      if (_win_target > XFER_WIN_MIN) _win_target--;
      requestWindow();
    }
    return;
  }

  if (_phase == PH_VERIFYING) { finishVerify(); return; }
}

void FwOtaTransfer::requestWindow() {
  { const uint32_t nw = millis();                        // (rc6, C14) the request waits for a quiet moment too (both ends yield)
    if (!otaQuiet(nw, _last_foreign_ms, _req_wait0)) { _req_pending = true; if (!_req_wait0) _req_wait0 = nw ? nw : 1; return; }
    _req_pending = false; _req_wait0 = 0; }
  _last_req_ms = millis();
  _win_req_ms = _last_req_ms; _win_credited = false;
  _retry_timeout_ms = WIN_RETRY_MS / 2 + _mesh->getRNG()->nextInt(0, WIN_RETRY_MS);   // jittered - see PH_OFFERING for why
  const uint8_t pct = chanPct(_agreed_pct ? _agreed_pct : askedPct());   // (rc6) the channel share both ends pace by
  const uint8_t back = _ret_set ? _ret_len : _route_len;           // chunks come along the way back ...
  const uint32_t air = chunkAirMs(back);
  const uint8_t relays = rtCount(back);                             // ... through these relays (the server counts the same ones)
  _seq++;   // vary the payload so a retry isn't dropped as an already-seen dup
            // (MeshCore's dedup hashes the raw payload bytes; a byte-identical
            // retransmission of the same window is otherwise silently swallowed
            // by the receiver's own mesh stack before fw_ota ever sees it)
  uint16_t start = _cm.firstMissing();                  // chunks may arrive in any order: always ask for what is still missing
  if (start >= _cm.n) { _phase = PH_VERIFYING; return; }
  uint16_t count;
  if (relays) {                                           // (v1.17.27) a fixed batch: RELAY_BATCH missing chunks (widened up to 8 slots when some
    uint16_t left = (uint16_t)(_cm.n - start);            //     in between have arrived); fewer only at the end of the image
    count = left < RELAY_BATCH ? left : RELAY_BATCH;
    while (count < XFER_WINDOW && count < left) {
      uint8_t mm = _cm.missingMask(start, (uint8_t)count), need = 0;
      for (uint8_t i = 0; i < count; i++) if (mm & (1u << i)) need++;
      if (need >= RELAY_BATCH) break;
      count++;
    }
  } else {
    uint8_t wmax = _win_target; { uint8_t dm = dutyWinMax(air, pct, 0); if (dm < wmax) wmax = dm; }   // direct: adaptive, within ~10 s
    count = (uint16_t)(_cm.n - start) < wmax ? (uint16_t)(_cm.n - start) : wmax;
  }
  _win_start = start; _win_count = count;
  _retry_timeout_ms += dutyWinExtraMs(air, pct, relays);  // the server paces the chunks (and keeps the quiet gap) at the agreed duty
  _retry_timeout_ms += (uint32_t)count * YIELD_PER_CHUNK_MS;   // (rc6, C14) the server may wait for quiet moments before each chunk
  if (rtCount(_route_len) && _rt_cur >= 0 && _routes[_rt_cur].used)   // (v1.17.25) through relays: + the route's measured reply time, so a slow
    _retry_timeout_ms += umin(_routes[_rt_cur].rtt_ms, 20000);        // multi-hop answer is not asked for again (a duplicate) before it can arrive
  uint8_t b[8 + 4 + 1 + MAX_ROUTE_BYTES + XFER_MAC_LEN];
  uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_CHUNK_REQ;
  memcpy(&b[n], &_session, 4); n += 4;
  memcpy(&b[n], &_seq, 2); n += 2;
  memcpy(&b[n], &start, 2); n += 2;
  b[n++] = (uint8_t)count | (_man.kind == KIND_BOOTLOADER ? 0x80 : 0);   // bit 7 = "of the bootloader offer"
  b[n++] = _cm.missingMask(start, (uint8_t)count);      // bit i set = chunk (start+i) still needed: a retry never re-asks for what arrived
  n = addReturnRoute(b, n);
  n += XFER_MAC_LEN;
  macFill(b, n); send(b, n, 50, _route, _route_len);
}

// The payload area is erased up front (two pages per tick, so the radio keeps running); only then may chunks be programmed.
void FwOtaTransfer::eraseStep() {
  uint32_t pages = (_man.payload_len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
  for (uint8_t k = 0; k < 2 && _erase_next < pages; k++, _erase_next++)
    if (!slotErasePayloadPage(_man.dst_off / FLASH_PAGE_SIZE + _erase_next)) { fail("slot erase failed"); return; }
  if (_erase_next < pages) return;
  _erased = true;
  saveState();
  requestWindow();
}

// ======================= packet in =====================================
void FwOtaTransfer::onPacket(mesh::Packet* pkt) {
  const uint8_t* b = pkt->payload;
  uint32_t len = pkt->payload_len;
  if (len < 2 + XFER_MAC_LEN || b[0] != XFER_MAGIC || !_circle_set) return;   // (v1.17.28) no circle = hidden: nothing is answered
  if (b[1] == MSG_OFFER_ACK) { onOfferAck(b, len); return; }   // its MAC uses the session key derived from the ACK itself (onOfferAck)
  if (b[1] == MSG_CHUNK && len >= 6) {                          // chunks carry the session: another pull's chunks are not ours (no MAC fail)
    uint32_t cs; memcpy(&cs, &b[2], 4);
    if (_phase != PH_DOWNLOADING || cs != _session) return;
  }
  uint8_t ktmp[16]; const uint8_t* k = keyForRecv(b[1], ktmp);
  DutySess* ks = nullptr;
  if (b[1] == MSG_CHUNK_REQ || b[1] == MSG_ABORT) {             // server: the key of that session (agreed at its offer)
    uint32_t rs; memcpy(&rs, &b[2], 4);
    ks = dutySess(rs, false);
    if (ks && ks->keyed) k = ks->kmac;
  }
  if (!k) return;                                   // not for us (no session, not pulling)
  if (!macOk(k, b, len)) { _mac_fail++; return; }

  switch (b[1]) {
    case MSG_OFFER_REQ2: onOfferReq(b, len); break;
    case MSG_OFFER_NAK: if (_phase == PH_OFFERING && !_resuming) fail("server NAK"); break;   // while waiting, keep trying
    case MSG_CHUNK_REQ: onChunkReq(b, len); break;
    case MSG_CHUNK:      onChunk(b, len); break;
    case MSG_LIST_REQ: onListReq(b, len); break;
    case MSG_LIST_RSP: onListRsp(b, len, (int8_t)(pkt->getSNR() * 4)); break;
    case MSG_ABORT: {
      // only OUR session: another puller's ABORT (zero-hop broadcast) must not kill this pull
      uint32_t asess; memcpy(&asess, &b[2], 4);
      { DutySess* ad = dutySess(asess, false); if (ad) memset(ad, 0, sizeof(*ad)); }   // (v1.17.25) server: that pull no longer holds a share
      if (asess == _session && (_phase == PH_DOWNLOADING || _phase == PH_OFFERING)) fail("peer abort");
      break;
    }
    default: break;
  }
}

// ---- server: answer an offer ----
void FwOtaTransfer::onOfferReq(const uint8_t* b, uint32_t len) {
  uint32_t sess; memcpy(&sess, &b[2], 4);
  uint8_t rseq[2]; memcpy(rseq, &b[6], 2);   // echoed: identical replies to a repeated request would be dropped as duplicates by the mesh
  // OFFER_REQ2 (v1.17.28): [8] duty asked [9..12] session it replaces [13..44] puller's public key [45..52] its nonce [53] route back ...
  const uint32_t o = 53;
  if (len < o + 1 + XFER_MAC_LEN) return;
  const uint8_t asked = b[8];
  { uint32_t prev; memcpy(&prev, &b[9], 4); DutySess* pd = prev && prev != sess ? dutySess(prev, false) : nullptr; if (pd) memset(pd, 0, sizeof(*pd)); }
  const uint8_t* ppub = &b[13]; const uint8_t* pnonce = &b[45];
  uint8_t rlen = b[o];                       // route back to the puller (reversed relay hashes, encoded length), empty when it is in direct range
  if (!rtValid(rlen) || o + 1 + rtBytes(rlen) + XFER_MAC_LEN > len) return;
  const uint8_t* rpath = &b[o + 1];
  const char* want = (const char*)&b[o + 1 + rtBytes(rlen)];
  uint32_t wl = len - o - 1 - rtBytes(rlen) - XFER_MAC_LEN;
  bool bl = false;
  if (wl > 0 && want[0] == '@') {                 // targeted at one node: "@<8 hex of its public key>", plus "!" for its BOOTLOADER; every other server stays silent
    char me[9]; snprintf(me, sizeof(me), "%02X%02X%02X%02X", _pub[0], _pub[1], _pub[2], _pub[3]);
    bl = (wl == 10 && want[9] == '!');
    if ((wl != 9 && !bl) || strncasecmp(want + 1, me, 8) != 0) return;
  }
  if (bl ? !_bl_serving : (!_serving || !imageStillServed())) return;
  const XferManifest& m = bl ? _bl_man : _serve_man;
  if (!bl) _serve_offer_reqs++; else _bl_offer_reqs++;   // (v1.17.26, A14) bootloader offers counted separately
  bool match = bl || (wl == 1 && want[0] == '*') || (wl > 0 && want[0] == '@') ||
               (wl > 0 && wl < 32 && strncmp(want, m.name, wl) == 0);
  if (!match) _serve_offer_nomatch++;
  else noteReq(b, true, 0, m, ppub);            // the same offer again and again: our OFFER_ACKs do not get back (heal)
  uint8_t r[8 + 64 + 32 + 16 + 1 + 40 + XFER_MAC_LEN]; uint32_t n = 0;
  r[n++] = XFER_MAGIC;
  if (!match) {
    r[n++] = MSG_OFFER_NAK; memcpy(&r[n], &sess, 4); n += 4; memcpy(&r[n], rseq, 2); n += 2;
    n += XFER_MAC_LEN; macFill(r, n); send(r, n, 100, rpath, rlen); return;   // circle offer key
  }
  r[n++] = MSG_OFFER_ACK;
  memcpy(&r[n], &sess, 4); n += 4;
  memcpy(&r[n], rseq, 2); n += 2;
  memcpy(&r[n], &m.payload_len, 4);  n += 4;
  memcpy(&r[n], &m.raw_len, 4);      n += 4;
  memcpy(&r[n], &m.payload_crc32, 4); n += 4;
  memcpy(&r[n], &m.raw_crc32, 4);    n += 4;
  memcpy(&r[n], &m.fw_version, 4);   n += 4;
  memcpy(&r[n], &m.compression, 2);  n += 2;
  memcpy(&r[n], &m.chunk_size, 2);   n += 2;
  memcpy(&r[n], &m.n_chunks, 4);     n += 4;
  memcpy(&r[n], m.sha256, 32);       n += 32;
  memcpy(&r[n], m.lzma_props, 5);    n += 5;   // appended last so an older puller just ignores it
  r[n++] = bl ? KIND_BOOTLOADER : KIND_APP;    // (2026-09-20) what kind of image this is ...
  memcpy(&r[n], g_fwpull_tag.board, 8); n += 8;   // ... and the board it is for: the puller refuses a foreign one before downloading anything
  // (v1.17.25) the duty agreed for this session: min(asked, ours minus the other pullers' shares)
  uint8_t ag = dutyAgree(asked >= DUTY_MIN_PCT && asked <= 100 ? asked : 0, _duty_pct, dutyOthers(sess));
  DutySess* ds = dutySess(sess, true);
  if (!ds) return;
  ds->pct = ag; ds->last_ms = millis(); ds->next_tx_ms = 0;
  r[n++] = ag;
  // (v1.17.28) session keys: the same server nonce for every repeat of this session's offer (the puller keeps the first ACK it accepts)
  if (!ds->keyed || memcmp(ds->np, pnonce, 8) != 0) {
    memcpy(ds->np, pnonce, 8); _mesh->getRNG()->random(ds->ns, 8);
    ds->keyed = sessionKeys(ppub, sess, ds->np, ds->ns, ds->kmac, ds->kenc);
  }
  if (!ds->keyed) return;
  memcpy(&r[n], _pub, 32); n += 32;                // our identity ...
  memcpy(&r[n], ds->ns, 8); n += 8;                // ... and nonce
  n += XFER_MAC_LEN;
  macFillKey(r, n, ds->kmac); send(r, n, 150, rpath, rlen);
}

// ---- puller: got the manifest ----
void FwOtaTransfer::onOfferAck(const uint8_t* b, uint32_t len) {
  if (_phase != PH_OFFERING) return;
  uint32_t sess; memcpy(&sess, &b[2], 4);
  if (sess != _session) return;
  const uint32_t offer_rtt = _last_req_ms ? millis() - _last_req_ms : 0;   // request -> answer on the route in use (before _last_req_ms is reset)
  // (v1.17.28) the answer carries the server's identity + nonce after the agreed duty: derive the session keys and check the ACK with them
  const uint32_t kp = 8 + 4 + 4 + 4 + 4 + 4 + 2 + 2 + 4 + 32 + 5 + 1 + 8 + 1;
  if (len < kp + 32 + 8 + XFER_MAC_LEN || _nonce_sess != _session) return;
  { uint8_t tid[4]; if (pullTargetId(tid) && memcmp(tid, &b[kp], 4) != 0) return; }   // must come from the node this pull targets
  { uint8_t km[16], ke[16];
    if (!sessionKeys(&b[kp], _session, _nonce, &b[kp + 32], km, ke) || !macOk(km, b, len)) { _mac_fail++; return; }
    memcpy(_sess_mac, km, 16); memcpy(_sess_enc, ke, 16); _sess_set = true; }
  XferManifest nm = _man;                 // keeps the requested name
  uint32_t n = 8;
  memcpy(&nm.payload_len, &b[n], 4);   n += 4;
  memcpy(&nm.raw_len, &b[n], 4);       n += 4;
  memcpy(&nm.payload_crc32, &b[n], 4); n += 4;
  memcpy(&nm.raw_crc32, &b[n], 4);     n += 4;
  memcpy(&nm.fw_version, &b[n], 4);    n += 4;
  memcpy(&nm.compression, &b[n], 2);   n += 2;
  memcpy(&nm.chunk_size, &b[n], 2);    n += 2;
  memcpy(&nm.n_chunks, &b[n], 4);      n += 4;
  memcpy(nm.sha256, &b[n], 32);        n += 32;
  // (v1.17.25) the duty the server agreed to for this session follows the board (absent = an older server: the pull paces by what it asked)
  if (len >= n + 5 + 1 + 8 + 1 + XFER_MAC_LEN) { uint8_t a = b[n + 14]; _agreed_pct = (a >= DUTY_MIN_PCT && a <= 100) ? a : askedPct(); }
  else _agreed_pct = askedPct();
  // LZMA props follow the sha256 (added 2026-09-19). Without them the bootloader cannot decode the image.
  memset(nm.lzma_props, 0, sizeof(nm.lzma_props));
  if (len >= n + sizeof(nm.lzma_props) + XFER_MAC_LEN) memcpy(nm.lzma_props, &b[n], sizeof(nm.lzma_props));
  else if (nm.compression == COMP_LZMA) { if (!_resuming) fail("server sent no LZMA props (old firmware?)"); return; }
  nm.kind = KIND_APP; nm.dst_off = 0;
  if (len >= n + sizeof(nm.lzma_props) + 1 + 8 + XFER_MAC_LEN) {          // kind + board (2026-09-20); absent from older servers = an app image
    nm.kind = b[n + 5];
    if (memcmp(&b[n + 6], g_fwpull_tag.board, 8) != 0) { if (!_resuming) fail("that image is for another board"); return; }
  }
  if (nm.kind != _man.kind) { if (!_resuming) fail("the server offered another kind of image"); return; }
  if (nm.kind == KIND_BOOTLOADER &&
      (nm.compression != COMP_NONE || nm.raw_len != nm.payload_len || nm.payload_len > BL_MAX_SIZE || (nm.payload_len & 3) || nm.payload_len < 0x100)) {
    if (!_resuming) fail("bad bootloader manifest");
    return;
  }

  if (nm.chunk_size != XFER_CHUNK || nm.n_chunks == 0 || nm.payload_len > SLOT_PAYLOAD_MAX) {
    if (!_resuming) fail("bad manifest");
    return;                               // while waiting, just ignore a server we cannot use
  }

  if (_resuming) {
    if (sameImage(nm, _saved_man)) {
      // the same image is being served again: continue from the first missing chunk
      _man = nm; _man.dst_off = _saved_man.dst_off;
      _resuming = false;
      // (v1.17.25) the session is the one the offer announced (new since the stall / reboot / switch), so the server paces it at the duty
      // it agreed. _seq keeps counting: a chunk echoes the request's seq, and the mesh's duplicate filter has already seen the chunk
      // packets of the old session (including the discarded half window) - identical re-sent packets would be dropped.
      _win_count = 0; _retries = 0; _last_req_ms = 0;
      _crc_bad = 0; _crc_bad_idx = -1;
      routeResult(true, offer_rtt);                       // (v1.17.25: was millis() - 0 = the uptime, after _last_req_ms had been reset)
      strncpy(_last_err, _switching ? "resumed from the new server" : "resumed", sizeof(_last_err) - 1);
      _switching = false;
      _rate_t0_ms = millis(); _rate_have0 = _cm.have;     // ETA counts from the resume, not from the outage
      _next_disc_ms = millis() + DISC_DOWNLOAD_MS;
      _phase = PH_DOWNLOADING;                            // if the erase was not finished, tick() continues it (chunks kept otherwise)
      if (!_erased) _erase_next = 0;
      saveState();
      if (_erased) requestWindow();
      return;
    }
    if (_switching) { revertSwitch("switch refused: that server has a different image - back to the previous one (chunks kept)"); return; }
    // a DIFFERENT image is being served now: the partial slot is useless - start over with the new one
    strncpy(_last_err, "server image changed - restarted", sizeof(_last_err) - 1);
    _resuming = false;
  }

  if (nm.kind == KIND_APP) {
    SlotHeader sh;
    if (readSlotHeader(sh) && sh.magic == SLOT_MAGIC && sh.payload_crc32 == nm.payload_crc32 && sh.payload_len == nm.payload_len
        && sh.fw_version == nm.fw_version && slotValid()) {
      _man = nm; _phase = PH_COMPLETE; clearPullKey();
      strncpy(_last_err, "already have this image", sizeof(_last_err) - 1);
      return;
    }
  }
  if (nm.kind == KIND_BOOTLOADER) {         // into the tail of the slot payload area if the app copy there leaves room, so the node keeps serving / self-healing;
    SlotHeader h0;                          // otherwise the app copy is given up (its header is erased below)
    nm.dst_off = (readSlotHeader(h0) && h0.payload_len <= BL_TAIL_OFF) ? BL_TAIL_OFF : 0;
  }
  _man = nm; _saved_man = nm;
  if (nm.n_chunks > ChunkMap::MAX_CHUNKS) { fail("image too big for the slot"); return; }
  if (!(nm.kind == KIND_BOOTLOADER && nm.dst_off)) eraseSlotHeader();   // an app: the slot stays invalid until the whole image is in and verified
  _cm.reset(nm.n_chunks); _erased = false; _erase_next = 0;
  _win_count = 0; _win_target = XFER_WIN_INIT;   // start each pull attempt with a fresh guess
  _retries = 0; _last_req_ms = 0;
  _crc_bad = 0; _crc_bad_idx = -1;
  _rate_t0_ms = millis(); _rate_have0 = 0; _next_disc_ms = millis() + DISC_DOWNLOAD_MS;
  if (offer_rtt) routeResult(true, offer_rtt);   // (v1.17.25) the route's reply time is known before the first window (its timeout uses it)
  _phase = PH_DOWNLOADING;                  // tick() erases the payload area, then requests the first window
  saveState();
  MESH_DEBUG_PRINTLN("fw_ota: manifest ok, %lu chunks", (unsigned long)_man.n_chunks);
}

// Server: remember the last few chunks sent, and to whom. Two pullers of the same image ask for the same chunks at about the same
// time; the second request is not answered again (its sender hears the first transmission), it re-asks after its own timeout if not.
bool FwOtaTransfer::recentlySent(uint16_t idx, uint32_t sess, uint32_t now) const {
  for (uint8_t i = 0; i < RECENT_N; i++)
    if (_recent[i].ms && _recent[i].idx == idx && _recent[i].sess != sess && now - _recent[i].ms < RECENT_MS) return true;
  return false;
}
void FwOtaTransfer::noteSent(uint16_t idx, uint32_t sess, uint32_t now) {
  _recent[_recent_wr] = { idx, sess, now ? now : 1 };
  _recent_wr = (_recent_wr + 1) % RECENT_N;
}

// Server: an OFFER_REQ addressed to THIS node ('@<my id>') failed its MAC = the operator typed the wrong admin password. Tell the puller
// (unauthenticated - the key is what is wrong), rate-limited. Requests for other servers, or to nobody in particular, get no answer.
void FwOtaTransfer::replyBadAuth(const uint8_t* b, uint32_t len) {
  const uint32_t o = b[1] == MSG_OFFER_REQ2 ? 13 : 8;
  if ((!_serving && !_bl_serving) || len < o + 1 + XFER_MAC_LEN) return;
  uint8_t rlen = b[o];
  if (!rtValid(rlen) || o + 1 + rtBytes(rlen) + XFER_MAC_LEN > len) return;
  const char* want = (const char*)&b[o + 1 + rtBytes(rlen)];
  uint32_t wl = len - o - 1 - rtBytes(rlen) - XFER_MAC_LEN;
  char me[9]; snprintf(me, sizeof(me), "%02X%02X%02X%02X", _pub[0], _pub[1], _pub[2], _pub[3]);
  if ((wl != 9 && !(wl == 10 && want[9] == '!')) || want[0] != '@' || strncasecmp(want + 1, me, 8) != 0) return;
  uint32_t now = millis();
  if (_last_bad_auth_ms && now - _last_bad_auth_ms < 3000) return;
  _last_bad_auth_ms = now;
  uint8_t r[2 + 4 + 2 + XFER_MAC_LEN]; uint32_t n = 0;
  r[n++] = XFER_MAGIC; r[n++] = MSG_BAD_AUTH;
  memcpy(&r[n], &b[2], 4); n += 4;                      // the puller's session
  memcpy(&r[n], &b[6], 2); n += 2;                      // its seq (keeps the notices distinct)
  n += XFER_MAC_LEN; macFill(r, n);
  send(r, n, 100, &b[o + 1], rlen);
}

// ---- server: send a run of chunks ----
void FwOtaTransfer::onChunkReq(const uint8_t* b, uint32_t len) {
  bool bl = (b[10] & 0x80) != 0;                       // bit 7 of the count byte: a chunk of the bootloader offer
  if (bl ? !_bl_serving : !_serving) return;
  if (!bl && (_serve_reqs & 0x3F) == 0 && !imageStillServed()) return;   // cheap periodic check that the slot is still what we advertised
  const XferManifest& m = bl ? _bl_man : _serve_man;
  uint32_t sess; memcpy(&sess, &b[2], 4);
  uint16_t start; memcpy(&start, &b[8], 2);
  uint8_t count = b[10] & 0x7F;
  if (count > XFER_WINDOW) count = XFER_WINDOW;
  uint8_t mask = b[11];   // bit i set = puller still needs chunk (start+i); 0xFF on a first request
  uint8_t rlen = b[12];   // route back to the puller (encoded length)
  if (!rtValid(rlen) || 13u + rtBytes(rlen) + XFER_MAC_LEN > len) return;
  const uint8_t* rpath = &b[13];
  uint8_t rseq[2]; memcpy(rseq, &b[6], 2);   // request seq, echoed in every chunk: a re-sent chunk must not be byte-identical or a relay drops it as a duplicate
  _serve_reqs++;
  uint32_t& last_idx = bl ? _bl_last_idx : _serve_last_idx;   // (v1.17.26, A14) app and bootloader progress kept apart
  if ((uint32_t)start + count > last_idx) last_idx = start + count;
  noteReq(b, false, start, m);                         // the same window again and again: our chunks do not get back (heal)
  // (v1.17.25) pace this session at its agreed duty (a session we do not know - older puller, or we rebooted - at our own duty)
  DutySess* ds = dutySess(sess, true);
  if (ds && !ds->pct) ds->pct = _duty_pct;
  const uint8_t pct = ds ? ds->pct : _duty_pct;
  const uint32_t now0 = millis();
  if (ds) ds->last_ms = now0;
  const uint8_t relays = rtCount(rlen);                // (v1.17.27) the chunks' way back: every relay on it repeats each chunk
  const uint8_t cp = chanPct(pct);                     // (rc6) the channel share the pacing uses
  const bool paced = dutyPaced(cp, relays);
  const uint32_t sp = dutySpacingMs(chunkAirMs(rlen), cp, relays);
  uint32_t t = 100;                                    // delay of the next chunk from now
  if (paced && ds && ds->next_tx_ms && (int32_t)(ds->next_tx_ms - now0) > (int32_t)t) t = ds->next_tx_ms - now0;   // the previous window's gap first
  if (!ds || !ds->keyed) return;                       // (v1.17.28) no session key: this session never made a valid offer
  // (rc6, C14) queue the window: tick() sends the chunks one by one, each in a quiet moment (serveTick) - a newer request replaces it
  ds->q_n = 0; ds->q_i = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (!(mask & (1u << i))) continue;   // puller already has this one - don't waste airtime
    uint32_t idx = start + i;
    if (idx >= m.n_chunks) break;
    ds->q_idx[ds->q_n++] = (uint16_t)idx;
  }
  ds->q_bl = bl; ds->q_rlen = rlen; if (rtBytes(rlen)) memcpy(ds->q_rpath, rpath, rtBytes(rlen));
  memcpy(ds->q_rseq, rseq, 2); ds->q_paced = paced; ds->q_sp = paced ? sp : CHUNK_GAP_MS; ds->q_relays = relays;
  ds->q_due = now0 + t; ds->q_wait0 = 0;
}

// (rc6, C14) server: the queued chunks go out one at a time, at the agreed pace, each only when the channel has been quiet for
// YIELD_QUIET_MS (other traffic first) or after YIELD_MAX_MS of waiting. The quiet gap follows the last chunk of a window.
void FwOtaTransfer::serveTick(uint32_t now) {
  for (uint8_t k = 0; k < DUTY_SESS_N; k++) {
    DutySess& d = _dsess[k];
    if (!d.sess || d.q_i >= d.q_n || (int32_t)(now - d.q_due) < 0) continue;
    if (d.q_bl ? !_bl_serving : !_serving) { d.q_n = 0; continue; }
    if (!otaQuiet(now, _last_foreign_ms, d.q_wait0)) { if (!d.q_wait0) d.q_wait0 = now ? now : 1; d.q_due = now + 200; continue; }
    d.q_wait0 = 0;
    sendChunk(d, d.q_idx[d.q_i++], now);
    d.q_due = now + d.q_sp + _mesh->getRNG()->nextInt(0, 60);
    if (d.q_i >= d.q_n && d.q_paced) d.next_tx_ms = now + d.q_sp + batchGapMs(d.q_relays);   // next window: one spacing + the quiet gap (C12)
  }
}

void FwOtaTransfer::sendChunk(DutySess& d, uint16_t idx, uint32_t now) {
  const bool bl = d.q_bl;
  const XferManifest& m = bl ? _bl_man : _serve_man;
  if (idx >= m.n_chunks) return;
  const uint32_t sess = d.sess;
  uint32_t off = (uint32_t)idx * XFER_CHUNK;
  uint32_t clen = umin(m.payload_len - off, XFER_CHUNK);
  uint16_t rkey = (uint16_t)idx | (bl ? 0x8000 : 0);      // (a bootloader chunk and an app chunk with the same index are different chunks)
  uint8_t r[2 + 4 + 2 + 2 + 2 + XFER_CHUNK + 2 + XFER_MAC_LEN]; uint32_t n = 0;
  r[n++] = XFER_MAGIC; r[n++] = MSG_CHUNK;
  memcpy(&r[n], &sess, 4); n += 4;              // (v1.17.28) the session: the data is encrypted with its key
  memcpy(&r[n], &idx, 2); n += 2;
  uint16_t wl = (uint16_t)clen;  memcpy(&r[n], &wl, 2);   n += 2;
  memcpy(&r[n], d.q_rseq, 2); n += 2;           // request seq echoed: a re-sent chunk must not be byte-identical (relays drop duplicates)
  flashRead(&r[n], bl ? BL_ADDR + off : SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off, clen); n += clen;
  uint16_t cc = crc16(&r[n - clen], clen); memcpy(&r[n], &cc, 2); n += 2;   // CRC of the plain data (checked after decryption)
  ctrXor(d.kenc, sess, idx, &r[n - 2 - clen], clen);   // AES-128-CTR, same length
  n += XFER_MAC_LEN;
  macFillKey(r, n, d.kmac);
  noteSent(rkey, sess, now);
  send(r, n, 0, d.q_rpath, d.q_rlen);
}

// (rc6, C14) every packet the node receives (MyMesh::logRx): anything that is not LoRa-OTA is "other traffic" the transfer yields to
void FwOtaTransfer::noteRx(uint8_t payload_type, const uint8_t* payload, uint16_t len) {
  if (payload_type == PAYLOAD_TYPE_RAW_CUSTOM && len && payload[0] == XFER_MAGIC) return;
  const uint32_t n = millis(); _last_foreign_ms = n ? n : 1;
}

// (v1.17.26, N1) The typed route got TYPED_FAIL_REDISCOVER unanswered requests in a row. Like MeshCore with a saved path that stops working:
// keep sending along it, but flood a discovery for the server right away and accept what comes back - a discovered route, or the server's
// heal notice for the way back. The typed pair stays in the route table. Reset by a new 'fw route ..' or a new pull.
void FwOtaTransfer::typedRouteFailed(uint32_t now) {
  _typed_failed = true;
  _next_disc_ms = now;                                   // tick() floods the targeted discovery
  snprintf(_last_err, sizeof(_last_err), "typed route unanswered: flood re-discovery");
}

// (v1.17.27, user) One node-wide budget for AUTOMATIC LoRa-OTA floods - heal notices and automatic re-discoveries - at most FLOOD_BUDGET_N
// per rolling hour (flood adverts at 1/h are the accepted norm; 2-4/h for a firmware transfer is acceptable). Floods an operator asks for
// (`fw list`) and flooded ANSWERS to other nodes' discoveries are not counted (each answers one request that was itself budgeted).
// When the budget is used up nothing breaks: heals wait; the puller keeps its route failover, its wait/resume and retries the discovery later.
bool FwOtaTransfer::floodBudget(uint32_t now) {
  uint8_t o = 0;
  for (uint8_t i = 1; i < FLOOD_BUDGET_N; i++) if (_flood_ts[i] == 0 || (_flood_ts[o] && now - _flood_ts[i] > now - _flood_ts[o])) o = i;   // free or oldest
  if (_flood_ts[o] && now - _flood_ts[o] < FLOOD_BUDGET_MS) return false;
  _flood_ts[o] = now ? now : 1;
  return true;
}

// ---- (v1.17.28) security v2: the owner's circle ----
// "fw passphrase <phrase>": C = PBKDF2-HMAC-SHA256(phrase, "LoRa-OTA circle v1", 50,000) - the same phrase on every node of the owner's
// circle. Only C is stored. Without it the node is hidden (serves, answers and pulls nothing). "fw passphrase off" leaves the circle;
// "fw passphrase" shows a short fingerprint of C to compare between nodes (never the phrase).
bool FwOtaTransfer::cmdPassphrase(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (!*arg) {
    if (_pb_i) { snprintf(reply, 160, "computing the circle key: %lu%% ...", (unsigned long)(_pb_i * 100 / PBKDF2_ITER)); return true; }
    if (!_circle_set) { strcpy(reply, "no passphrase: LoRa-OTA is off on this node ('fw passphrase <phrase>', the same on every node of your circle)"); return true; }
    uint8_t fp[32]; mesh::Utils::sha256(fp, 32, _circle, 32);
    snprintf(reply, 160, "circle set, fingerprint %02X%02X%02X%02X (compare it between your nodes)", fp[0], fp[1], fp[2], fp[3]);
    return true;
  }
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - a pull is in progress ('fw pull-abort' first)"); return true; }
  if (!strcmp(arg, "off")) {
    _circle_set = false; memset(_circle, 0, 32); circleKeys(); stopServing(); _bl_serving = false;
    if (_fs) _fs->remove(CIRCLE_FILE);
    strcpy(reply, "OK - passphrase removed: LoRa-OTA is off on this node (hidden)"); return true;
  }
  if (strlen(arg) < PASS_MIN || strlen(arg) >= sizeof(_pb_pw)) { snprintf(reply, 160, "ERR - %u to %u characters (4 random words are good)", (unsigned)PASS_MIN, (unsigned)sizeof(_pb_pw) - 1); return true; }
  if (_pb_i) { strcpy(reply, "ERR - still computing the previous passphrase (~20 s)"); return true; }
  strcpy(_pb_pw, arg);                                    // PBKDF2 block 1: U1 = HMAC(P, salt | INT(1)) - the rest runs in tick()
  static const char salt[] = "LoRa-OTA circle v1";
  pbkdf2Start(_pb_pw, (const uint8_t*)salt, sizeof(salt) - 1, _pb_u, _pb_out);
  _pb_i = 1;
  strcpy(reply, "OK - computing the circle key (~20 s, the node keeps relaying); 'fw passphrase' shows its fingerprint when done");
  return true;
}

// 250 PBKDF2 iterations per tick (~0.1 s); when all are done: store C, derive the circle keys, serve again
void FwOtaTransfer::passphraseStep() {
  _pb_i = pbkdf2Steps(_pb_pw, _pb_u, _pb_out, _pb_i, 250, PBKDF2_ITER);
  if (_pb_i < PBKDF2_ITER) return;
  memcpy(_circle, _pb_out, 32); _circle_set = true; circleKeys();
  memset(_pb_pw, 0, sizeof(_pb_pw)); memset(_pb_u, 0, 32); memset(_pb_out, 0, 32); _pb_i = 0;
  if (_fs) { _fs->remove(CIRCLE_FILE); File f = _fs->open(CIRCLE_FILE, FILE_O_WRITE); if (f) { f.write(_circle, 32); f.close(); } }
  _bl_probed = false; _probe_valid = false; _next_autoserve_ms = millis();   // serve again with the new circle
  uint8_t fp[32]; mesh::Utils::sha256(fp, 32, _circle, 32);
  Serial.printf("fw_ota: circle key set, fingerprint %02X%02X%02X%02X\n", fp[0], fp[1], fp[2], fp[3]);
}

// ---- (v1.17.28) BLE slot fill ----
// `fw pull-ble`: the node restarts into the bootloader's Bluetooth DFU (rev 5, reset magic 0xA9) - the phone's DFU app then uploads a
// "slot zip" whose image is 0x1000 - 160 bytes of 0xFF + a .lota file: the bootloader writes it into the SLOT (never over the app), so the
// .lota header ends at the slot's first page and its payload sits exactly at the slot payload offset. The node restarts into this app
// (5 min without DFU activity: also back here), and slotFromLota() turns it into a VALID slot that is served.
bool FwOtaTransfer::cmdPullBle(char* reply) {
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - a pull is in progress ('fw pull-abort' first)"); return true; }
  if (_staging) { strcpy(reply, "ERR - stage in progress"); return true; }
  if (!_bl_probed) armBlServe();
  if (_own_bl_rev < 5) { snprintf(reply, 160, "ERR - needs bootloader rev 5+ (this node: rev %lu) - 'fw blupdate' first", (unsigned long)_own_bl_rev); return true; }
  _reset_magic = GPREGRET_SLOT_DFU; _reset_at_ms = millis() + 3000;
  strcpy(reply, "OK - Bluetooth upload into the slot in 3 s: DFU app -> 3401_DFU -> the release's -slot.zip. Off the mesh until done (5 min idle = back)");
  return true;
}

// `fw apply`: install the slot image on THIS node - the bootloader applies it at the next boot (= fw pending + reboot)
bool FwOtaTransfer::cmdApply(char* reply) {
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING || _staging) { strcpy(reply, "ERR - a pull or stage is in progress"); return true; }
  if (!slotValid()) { strcpy(reply, "ERR - slot not VALID: nothing to install"); return true; }
  { SlotHeader ih; if (readSlotHeader(ih) && installedImage(ih)) {       // (v1.17.28, C1) nothing to do: no flash write, no reboot
      snprintf(reply, 160, "fw-%lu is already installed (the slot holds the running app) - nothing to do", (unsigned long)ih.fw_version); return true; } }
  if (!ctrlMarkPending()) { strcpy(reply, "ERR - could not mark the slot pending"); return true; }
  SlotHeader h; readSlotHeader(h);
  _reset_magic = 0; _reset_at_ms = millis() + 3000;
  snprintf(reply, 160, "OK - installing fw-%lu: the node restarts in 3 s and the bootloader applies the slot", (unsigned long)h.fw_version);
  return true;
}

bool FwOtaTransfer::bleFilled(const SlotHeader& h) const {
  if (!_fs || !_fs->exists(BLEFILL_FILE)) return false;
  File f = _fs->open(BLEFILL_FILE); uint32_t c = 0; bool ok = f && f.read((uint8_t*)&c, 4) == 4; if (f) f.close();
  return ok && c == h.payload_crc32;
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void FwOtaTransfer::slotFromLota() {
  const uint32_t at = SLOT_ADDR + SLOT_PAYLOAD_OFFSET - LOTA_HDR;
  uint8_t h[LOTA_HDR]; flashRead(h, at, LOTA_HDR);
  if (memcmp(h, "LOTA", 4) != 0) return;                 // nothing uploaded over BLE
  const char* why = nullptr;
  const uint32_t plen = le32(h + 20), rlen = le32(h + 24), pcrc = le32(h + 28);
  if (crc32_flash(at, 156) != le32(h + 156))                                   why = "header CRC";
  else if (h[4] != 1 || h[5] != 0)                                             why = "format version";
  else if (h[6] != KIND_APP)                                                   why = "not an app image";
  else if (memcmp(h + 8, g_fwpull_tag.board, 8) != 0)                          why = "for another board";
  else if (!plen || plen > SLOT_PAYLOAD_MAX || !rlen || rlen > APP_MAX_SIZE)   why = "size";
  else if ((h[36] | (h[37] << 8)) != COMP_LZMA || h[44] != 0x5D || le32(h + 45) != 8192) why = "compression (LZMA, dict 8192)";
  else if (crc32_flash(SLOT_ADDR + SLOT_PAYLOAD_OFFSET, plen) != pcrc)        why = "payload CRC (incomplete?)";
  else {
    SHA256 sh; static uint8_t buf[512]; uint8_t out[32]; sh.reset();
    for (uint32_t off = 0; off < plen; ) {
      uint32_t n = umin(plen - off, sizeof(buf)); flashRead(buf, SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off, n); sh.update(buf, n);
      off += n; if ((off & 0x3FFF) == 0) yield();
    }
    sh.finalize(out, sizeof(out));
    if (memcmp(out, h + 52, 32) != 0) why = "payload SHA-256";
  }
  if (why) {
    eraseSlotHeader();                                   // the slot reads as empty again; the app was never touched
    snprintf(_last_err, sizeof(_last_err), "BLE upload refused: %s", why);
    Serial.print("fw_ota: "); Serial.println(_last_err);
    return;
  }
  SlotHeader sh2; memset(&sh2, 0, sizeof(sh2));
  sh2.raw_len = rlen; sh2.raw_crc32 = le32(h + 32); sh2.compression = COMP_LZMA; sh2.fw_version = le32(h + 16);
  memcpy(sh2.lzma_props, h + 44, 5);
  if (!_writer.finishSparse(sh2, plen, pcrc) || !slotValid()) { strcpy(_last_err, "BLE upload: slot header write failed"); return; }
  if (_fs) { _fs->remove(BLEFILL_FILE); File f = _fs->open(BLEFILL_FILE, FILE_O_WRITE); if (f) { f.write((const uint8_t*)&pcrc, 4); f.close(); } }
  snprintf(_last_err, sizeof(_last_err), "BLE upload: fw-%lu in the slot, served", (unsigned long)sh2.fw_version);
  Serial.print("fw_ota: "); Serial.println(_last_err);
}

// ---- duty (v1.17.25) ----
uint32_t FwOtaTransfer::chunkAirMs(uint8_t route_enc) const {
  uint32_t on_air = 2 + rtBytes(route_enc) + 2 + 4 + 2 + 2 + 2 + XFER_CHUNK + 2 + XFER_MAC_LEN;   // header + path len + path + CHUNK message
  uint32_t a = _flood_host ? _flood_host->airtimeMs(on_air) : 0;
  return a ? a : CHUNK_AIR_FALLBACK_MS;
}

FwOtaTransfer::DutySess* FwOtaTransfer::dutySess(uint32_t sess, bool create) {
  DutySess* old = nullptr; const uint32_t now = millis();
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) {
    if (_dsess[i].last_ms && _dsess[i].sess == sess) return &_dsess[i];
    if (!old || !_dsess[i].last_ms || (old->last_ms && now - _dsess[i].last_ms > now - old->last_ms)) old = &_dsess[i];   // free, else the stalest
  }
  if (!create || !old) return nullptr;
  memset(old, 0, sizeof(*old)); old->sess = sess; old->last_ms = now ? now : 1;
  return old;
}

uint8_t FwOtaTransfer::dutyOthers(uint32_t sess) const {
  uint16_t sum = 0; const uint32_t now = millis();
  for (uint8_t i = 0; i < DUTY_SESS_N; i++)
    if (_dsess[i].last_ms && _dsess[i].sess != sess && now - _dsess[i].last_ms < DUTY_ACTIVE_MS) sum += _dsess[i].pct;
  return sum > 255 ? 255 : (uint8_t)sum;
}

uint8_t FwOtaTransfer::parseDuty(const char* s) { return dutyParse(s); }

// "fw duty" - show; "fw duty 0.3" - set (0.1 .. 1.0, persistent). As server: the most any pull of ours may use. As puller: what a pull asks for
// (unless "fw pull ... duty x"). Takes effect at the next start / restart of a pull (the pace is agreed once, at the offer).
bool FwOtaTransfer::cmdDuty(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (*arg) {
    uint8_t p = dutyParse(arg);
    if (!p) { strcpy(reply, "ERR - fw duty 0.1 .. 1.0 (share of airtime LoRa-OTA may use; 1.0 = no limit)"); return true; }
    _duty_pct = p;
    if (_fs) { _fs->remove(DUTY_FILE); File f = _fs->open(DUTY_FILE, FILE_O_WRITE); if (f) { f.write(&p, 1); f.close(); } }
    snprintf(reply, 160, "OK - duty %u.%02u saved (LoRa-OTA <= %u%% airtime); applies from the next pull start/restart", p / 100, p % 100, p);
    return true;
  }
  int w = snprintf(reply, 160, "duty %u.%02u (LoRa-OTA <= %u%% of the channel incl. relays, default 0.30)", _duty_pct / 100, _duty_pct % 100, _duty_pct);
  if (w > 0 && w < 160 && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING))
    w += snprintf(reply + w, 160 - w, " | pull asks %u.%02u%s, agreed %s", askedPct() / 100, askedPct() % 100, _pull_duty_pct ? " (this pull)" : "",
                  _agreed_pct ? "" : "- (at the offer)");
  if (w > 0 && w < 160 && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING) && _agreed_pct)
    w += snprintf(reply + w, 160 - w, "%u.%02u", _agreed_pct / 100, _agreed_pct % 100);
  const uint32_t now = millis(); uint8_t ns = 0; uint16_t sum = 0;
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) if (_dsess[i].last_ms && now - _dsess[i].last_ms < DUTY_ACTIVE_MS) { ns++; sum += _dsess[i].pct; }
  if (w > 0 && w < 160 && ns) snprintf(reply + w, 160 - w, " | serving %u pull(s) at %u.%02u", ns, sum / 100, sum % 100);
  return true;
}

// ---- puller: got a chunk ----
void FwOtaTransfer::onChunk(const uint8_t* b, uint32_t len) {
  if (_phase != PH_DOWNLOADING || !_erased) return;
  // MSG_CHUNK layout (v1.17.28): [0]magic [1]type [2..5]session [6..7]idx [8..9]clen [10..11]req seq [12..]data (AES-128-CTR) [.]crc16 [.]mac
  // onPacket checked the session and the MAC (session key) already.
  uint16_t idx;  memcpy(&idx,  &b[6], 2);
  uint16_t clen; memcpy(&clen, &b[8], 2);
  if (!_sess_set || clen == 0 || clen > XFER_CHUNK || 12u + clen + 2u + XFER_MAC_LEN > len || idx >= _cm.n) return;
  uint8_t data[XFER_CHUNK]; memcpy(data, &b[12], clen);
  ctrXor(_sess_enc, _session, idx, data, clen);          // decrypt
  uint16_t cc; memcpy(&cc, &b[12 + clen], 2);
  if (crc16(data, clen) != cc) { _crc_bad++; _crc_bad_idx = (int32_t)idx; return; }
  if (_cm.has(idx)) return;                                         // dup
  uint32_t off = (uint32_t)idx * XFER_CHUNK;
  if (clen != umin(XFER_CHUNK, _man.payload_len - off)) return;
  if (!slotProgramAt(_man.dst_off + off, data, clen)) { fail("slot write failed"); return; }
  _cm.set(idx);
  _last_req_ms = millis();   // progress: reset the window timeout
  if (!_win_credited) { _win_credited = true; routeResult(true, _last_req_ms - _win_req_ms); }   // the route in use answered
  if (_cm.complete()) { _phase = PH_VERIFYING; return; }
  if (_cm.missingMask(_win_start, (uint8_t)_win_count) == 0) {      // the window is complete (with our own or overheard chunks): next one
    if (_retries == 0 && _win_target < XFER_WINDOW) _win_target++;   // channel's keeping up - ask for more
    _retries = 0;
    requestWindow();
  }
}

// ---- puller: all chunks in, verify + finalize ----
void FwOtaTransfer::finishVerify() {
  const bool bl = _man.kind == KIND_BOOTLOADER;
  // 1) CRC-32 over the payload in flash must match the manifest
  uint32_t pcrc = crc32_flash(dstAddr(0), _man.payload_len);
  if (pcrc != _man.payload_crc32) { retryOrFail("payload CRC32 mismatch"); return; }

  // 2) header write (turns the slot VALID) - only after CRC passes. A bootloader never gets a slot header: the bootloader must not apply it as an app.
  if (!bl) {
    SlotHeader h; memset(&h, 0, sizeof(h));
    h.raw_len     = _man.raw_len;
    h.raw_crc32   = _man.raw_crc32;
    h.compression = _man.compression;
    h.fw_version  = _man.fw_version;
    memcpy(h.lzma_props, _man.lzma_props, sizeof(h.lzma_props));
    if (!_writer.finishSparse(h, _man.payload_len, pcrc)) { fail("finish/header failed"); return; }
  }

  // 3) SHA-256 over the payload in flash must match the manifest
  SHA256 sh; static uint8_t buf[512]; uint8_t out[32];
  sh.reset();
  uint32_t plen = _man.payload_len;
  for (uint32_t off = 0; off < plen; ) {
    uint32_t n = (plen - off) < sizeof(buf) ? (plen - off) : (uint32_t)sizeof(buf);
    flashRead(buf, dstAddr(off), n);
    sh.update(buf, n);
    off += n; if ((off & 0x1FFF) == 0) yield();
  }
  sh.finalize(out, sizeof(out));
  if (memcmp(out, _man.sha256, 32) != 0) {
    if (!bl) eraseSlotHeader();          // invalidate - do not leave a bad slot VALID
    retryOrFail("SHA-256 mismatch");
    return;
  }

  // 4) a bootloader is also checked as what it is (vectors, identity descriptor, flash layout, board) before the MBR may copy it over ours
  if (bl) {
    BlDesc d;
    BlCheck c = blImageCheck(rdFlash, (void*)dstAddr(0), plen, g_fwpull_tag.board, uicrBl(), uicrMbr(), &d);
    if (c == BL_OK && d.bl_rev != _man.fw_version) c = BL_NO_DESC;
    if (c != BL_OK) { char m[sizeof(_last_err)]; snprintf(m, sizeof(m), "bootloader refused: %s", blCheckText(c)); fail(m); return; }
    _phase = PH_COMPLETE; _resuming = false;   // state stays until the copy starts: a reboot before that verifies again (needs no server) and applies
    _apply_armed = true; _apply_at_ms = millis() + 5000;
    return;
  }

  _phase = PH_COMPLETE;
  _resuming = false;
  clearState(); clearPullKey();
  SlotHeader ih;
  if (readSlotHeader(ih) && installedImage(ih))         // (v1.17.28, C1) the node already RUNS this image: fill the slot, serve it, no re-flash
    strncpy(_last_err, "already installed: slot filled, served", sizeof(_last_err) - 1);
  else if (!_noapply) { _apply_armed = true; _apply_at_ms = millis() + 5000; }   // verified: apply without anyone babysitting it
  _next_autoserve_ms = millis();     // become a server right away
  MESH_DEBUG_PRINTLN("fw_ota: pull COMPLETE, slot valid");
}

// ======================= discovery / peer pull ==========================
void FwOtaTransfer::sendListReq() {
  uint8_t b[8 + XFER_MAC_LEN]; uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_LIST_REQ;
  memcpy(&b[n], &_scan_sess, 4); n += 4;
  _scan_seq++; memcpy(&b[n], &_scan_seq, 2); n += 2;
  n += XFER_MAC_LEN; macFill(b, n); send(b, n, 50);
}

bool FwOtaTransfer::answerOnce(uint32_t sess, bool flood) { return answerAllowed(_ans, ANS_N, _ans_wr, sess, flood, millis()); }

// `fw list`: prepare now, transmit after SCAN_START_MS (delay_ms), so the command's reply is on its way before the scan's own packets
void FwOtaTransfer::startScan(uint32_t delay_ms) {
  memset(_peers, 0, sizeof(_peers));
  uint32_t now = millis();
  _scan_sess = (now << 8) ^ 0x77A1; _scan_seq = 0; _scan_active = true; _scan_resends = 0; _scan_resend_ms = 0;
  _scan_start_ms = now + delay_ms; _scan_pending = true;
  _scan_end_ms = _scan_start_ms + SCAN_MS;
  if (!delay_ms) scanSend();
}
void FwOtaTransfer::scanSend() {
  _scan_pending = false;
  sendListReq();                                        // direct neighbours (also older servers and the Pager)
  _disc_sess = _scan_sess; sendDiscovery("*");          // and everything within MAX_HOPS relays, once (one flood per scan)
  _scan_resend_ms = scanResendAt(0, millis(), (uint32_t)_mesh->getRNG()->nextInt(0, SCAN_REQ_SPREAD_MS + 1), _scan_end_ms);
}

bool FwOtaTransfer::cmdList(char* reply) {
  if (!_circle_set) { strcpy(reply, "ERR - set 'fw passphrase <phrase>' first: only nodes of your circle are listed"); return true; }
  // allowed while a pull WAITS for its server (PH_OFFERING): the scan is independent and shows what else is in reach
  if (_phase == PH_DOWNLOADING || _phase == PH_VERIFYING || _staging) { strcpy(reply, "ERR - busy (download/stage in progress)"); return true; }
  startScan(SCAN_START_MS);
  if (_flood_host && !_flood_host->regionSet())   // relays that only forward region floods drop an unscoped discovery
    strcpy(reply, "scanning ~25 s, then: fw peers. No default region set: region-only relays drop the discovery ('region default <name>')");
  else
    strcpy(reply, "scanning for peers that offer firmware (direct and through relays) - wait ~25 s, then type: fw peers");
  return true;
}

void FwOtaTransfer::onListReq(const uint8_t* b, uint32_t len) {
  if (len < 8 + XFER_MAC_LEN) return;
  uint32_t sess; memcpy(&sess, &b[2], 4);
  if ((_serving && imageStillServed()) || _bl_serving) { if (!answerOnce(sess, false)) return; }   // (v1.17.28) at most twice per scan
  if (_serving && imageStillServed()) sendListRsp(b, _serve_man, 300 + _mesh->getRNG()->nextInt(0, 2500));   // spread the answers of many servers over a few seconds
  if (_bl_serving)                    sendListRsp(b, _bl_man,    300 + _mesh->getRNG()->nextInt(0, 2500));   // a node with both offers answers twice (kind byte differs)
}

static const uint32_t LIST_RSP_MAX = 2 + 4 + 2 + 4 + 4 + 4 + 16 + 1 + 8 + 1 + MAX_ROUTE_BYTES + XFER_MAC_LEN;

// The exact length of a LIST_RSP (with kind, board and route) inside `avail` bytes, 0 = none. A channel message arrives DECRYPTED WITH ITS AES
// PADDING (MeshCore rounds the length up to 16 bytes), so a binary answer carried in a flood must be cut to its own length before the MAC
// check - v1.17.23 did not, and dropped every flooded answer and heal notice (found 2026-09-27 on the bench: "fw peers" stayed empty).
static uint32_t listRspLen(const uint8_t* b, uint32_t avail) {
  if (avail < 46 + XFER_MAC_LEN || !rtValid(b[45])) return 0;
  uint32_t n = 46 + rtBytes(b[45]) + XFER_MAC_LEN;
  return n <= avail ? n : 0;
}

// LIST_RSP: [magic][type][session 4][seq 2][id 4][fw_version 4][payload_len 4][name 16][kind][board 8][route: enc + hashes][mac 8]
uint32_t FwOtaTransfer::buildListRsp(uint8_t* r, const uint8_t* sess_seq, const XferManifest& m, const uint8_t* fwd, uint8_t enc) const {
  uint32_t n = 0;
  r[n++] = XFER_MAGIC; r[n++] = MSG_LIST_RSP;
  memcpy(&r[n], sess_seq, 4); n += 4;                 // session
  memcpy(&r[n], sess_seq + 4, 2); n += 2;             // seq echoed (keeps repeated replies distinct)
  memcpy(&r[n], _pub, 4); n += 4;
  memcpy(&r[n], &m.fw_version, 4); n += 4;
  memcpy(&r[n], &m.payload_len, 4); n += 4;
  memset(&r[n], 0, 16); if (_name_src) strncpy((char*)&r[n], _name_src, 16); n += 16;
  r[n++] = m.kind;                                    // (2026-09-20) kind + board; older servers do not send them
  memcpy(&r[n], g_fwpull_tag.board, 8); n += 8;
  r[n++] = enc;                                       // (2026-09-21) the path the discovery took, puller -> here: encoded length + hashes (0 = direct neighbour)
  if (rtBytes(enc)) { memcpy(&r[n], fwd, rtBytes(enc)); n += rtBytes(enc); }
  n += XFER_MAC_LEN; macFill(r, n);
  return n;
}

void FwOtaTransfer::sendListRsp(const uint8_t* req, const XferManifest& m, uint32_t delay_ms, const uint8_t* fwd, uint8_t enc, const uint8_t* back) {
  uint8_t r[LIST_RSP_MAX];
  uint32_t n = buildListRsp(r, &req[2], m, fwd, enc);
  send(r, n, delay_ms, back, enc);                    // back along the reversed path (stock repeaters relay direct packets)
}

// A LIST_RSP flooded on the discovery channel (stock repeaters relay channel floods, never RAW floods). The path the flood collects on its
// way is a way from here to the puller that WORKS in that direction - the reversed discovery path may not (one-way links).
void FwOtaTransfer::floodListRsp(const uint8_t* r, uint32_t n, uint32_t delay_ms, bool heal) {
  if (!_mesh || !_circle_set) return;
  uint8_t d[5 + 6 + LIST_RSP_MAX]; uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
  memcpy(d, &ts, 4); d[4] = 0;                        // like a channel text: timestamp, flags, then our marker and the binary answer
  memcpy(&d[5], heal ? FLOOD_HEAL : FLOOD_RSP, 6);
  memcpy(&d[11], r, n);
  floodOut(_mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _disc_ch, d, 11 + n), delay_ms);
}

// Server: a puller that repeats the same request (the offer, or a window from the same chunk) has not received our answers. Requests come
// in, answers get lost = the way back is broken (a one-way link). Flood a heal notice for its session: it reaches the puller over paths that
// work in OUR direction, and the path it collects becomes the return route of the puller's next requests. Rate-limited and capped.
void FwOtaTransfer::noteReq(const uint8_t* req, bool offer, uint16_t start, const XferManifest& m, const uint8_t* who) {
  uint32_t sess; memcpy(&sess, &req[2], 4);
  uint32_t key = offer ? 0xFFFFFFFFu : start;
  HealRec* h = nullptr;
  for (uint8_t i = 0; i < HEAL_N && !h; i++) if (_heal[i].reps + _heal[i].heals && _heal[i].sess == sess) h = &_heal[i];
  if (!h && who)                                     // (v1.17.28) a resume runs under a new session: the same PULLER keeps its heal history
    for (uint8_t i = 0; i < HEAL_N && !h; i++) if (_heal[i].reps + _heal[i].heals && memcmp(_heal[i].who, who, 4) == 0) { h = &_heal[i]; h->sess = sess; }
  if (!h) { h = &_heal[_heal_wr]; _heal_wr = (_heal_wr + 1) % HEAL_N; memset(h, 0, sizeof(*h)); h->sess = sess; h->key = key; }
  if (who) memcpy(h->who, who, 4);
  if (!offer && start > h->top) {                   // how far the puller has got (a window starts at its first missing chunk) ...
    if (h->top) h->heals = 0;                        // ... further than before: the way back works again (the first one seen is only where it is)
    h->top = start;
  }
  if (h->key != key) { h->key = key; h->reps = 0; }
  if (++h->reps < HEAL_REPS) return;
  h->reps = 0;
  uint32_t now = millis();
  if (h->heals >= HEAL_MAX || (h->heals && now - h->last_ms < HEAL_NOPROG_MS)) return;   // the last heal did not help: once an hour at most
  if (!floodBudget(now)) return;                                      // (v1.17.27) node-wide: 4 automatic floods per rolling hour, all kinds
  h->heals++; h->last_ms = now ? now : 2; _heals_sent++;
  uint8_t r[LIST_RSP_MAX];
  uint32_t n = buildListRsp(r, &req[2], m, nullptr, 0);   // the puller's session and seq; no forward path (the puller keeps its own)
  floodListRsp(r, n, 200 + _mesh->getRNG()->nextInt(0, 800), true);
}

// ---- auto routing (modelled on meshcore-open's path history, see docs/AUTO-ROUTING-DESIGN.md) ----
void FwOtaTransfer::routeReset() {
  memset(_routes, 0, sizeof(_routes)); _rt_cur = -1; _rt_recent[0] = _rt_recent[1] = -1; _rt_fail_run = 0;
  int i = routeAdd(_route, _route_len, _ret_set ? _ret : nullptr, _ret_len);   // the route we start with (direct, the discovered one, or a typed one)
  if (i >= 0) { _rt_cur = (int8_t)i; _rt_recent[0] = (int8_t)i; }
}

int FwOtaTransfer::routeAdd(const uint8_t* path, uint8_t enc, const uint8_t* ret, uint8_t ret_enc) {
  if (!rtValid(enc) || (ret && !rtValid(ret_enc))) return -1;
  for (int i = 0; i < MAX_ROUTES; i++)                       // a pair: the same way there AND the same way back (or both "reverse")
    if (_routes[i].used && _routes[i].enc == enc && memcmp(_routes[i].path, path, rtBytes(enc)) == 0
        && _routes[i].has_ret == (ret != nullptr) && (!ret || (_routes[i].ret_enc == ret_enc && memcmp(_routes[i].ret, ret, rtBytes(ret_enc)) == 0))) return i;
  int slot = -1;
  for (int i = 0; i < MAX_ROUTES && slot < 0; i++) if (!_routes[i].used) slot = i;
  if (slot < 0) {                                           // full: replace the weakest (never the one in use)
    float worst = 1e9f;
    for (int i = 0; i < MAX_ROUTES; i++) if (i != _rt_cur) { float s = routeScore(i); if (s < worst) { worst = s; slot = i; } }
    if (slot < 0) return -1;
    if (_rt_recent[0] == slot) _rt_recent[0] = -1;
    if (_rt_recent[1] == slot) _rt_recent[1] = -1;
  }
  RouteRec& r = _routes[slot];
  memset(&r, 0, sizeof(r)); r.used = true; r.enc = enc; r.weight = 1.0f;
  if (rtBytes(enc)) memcpy(r.path, path, rtBytes(enc));
  if (ret) { r.has_ret = true; r.ret_enc = ret_enc; if (rtBytes(ret_enc)) memcpy(r.ret, ret, rtBytes(ret_enc)); }
  return slot;
}

void FwOtaTransfer::routeUse(int i) {
  if (i < 0 || i >= MAX_ROUTES || !_routes[i].used) return;
  const RouteRec& r = _routes[i];
  if (rtBytes(r.enc)) memcpy(_route, r.path, rtBytes(r.enc));
  _route_len = r.enc; _rt_cur = (int8_t)i; _rt_fail_run = 0;
  if (r.has_ret) setRet(r.ret, r.ret_enc, _ret_typed && _ret_set && r.ret_enc == _ret_len && !memcmp(r.ret, _ret, rtBytes(r.ret_enc)));
  else setRet(nullptr, 0, false);
  if (_rt_recent[0] != i) { _rt_recent[1] = _rt_recent[0]; _rt_recent[0] = (int8_t)i; }
}

// meshcore-open's ranking: 0.45 reliability ((ok+1)/(n+2)) + 0.25 speed (fastest / this) + 0.1 freshness + 0.2 weight; fewer hops break ties
float FwOtaTransfer::routeScore(int i) const {
  const RouteRec& r = _routes[i];
  if (!r.used) return -1.0f;
  uint16_t best = 0; float wmax = 0.1f;
  for (int k = 0; k < MAX_ROUTES; k++) if (_routes[k].used) {
    if (_routes[k].rtt_ms && (!best || _routes[k].rtt_ms < best)) best = _routes[k].rtt_ms;
    if (_routes[k].weight > wmax) wmax = _routes[k].weight;
  }
  float rel = (r.ok + 1.0f) / (r.ok + r.fail + 2.0f);
  float lat = (!best || !r.rtt_ms) ? 0.6f : (float)best / (float)r.rtt_ms;
  float fresh = r.last_ok_ms ? 1.0f / (1.0f + (float)(millis() - r.last_ok_ms) / 86400000.0f) : 0.0f;
  return rel * 0.45f + lat * 0.25f + fresh * 0.1f + (r.weight / wmax) * 0.2f - rtCount(r.enc) * 0.005f;
}

void FwOtaTransfer::routeResult(bool ok, uint32_t rtt_ms) {
  if (_rt_cur < 0 || !_routes[_rt_cur].used) return;
  RouteRec& r = _routes[_rt_cur];
  if (ok) {
    if (r.ok < 65535) r.ok++;
    uint32_t t = rtt_ms > 65000 ? 65000 : rtt_ms;
    r.rtt_ms = r.rtt_ms ? (uint16_t)((r.rtt_ms * 3u + t) / 4u) : (uint16_t)(t ? t : 1);
    r.last_ok_ms = millis() ? millis() : 1;
    r.weight = r.weight + 0.5f > 5.0f ? 5.0f : r.weight + 0.5f;
    _rt_fail_run = 0;
  } else {
    if (r.fail < 65535) r.fail++;
    r.weight = r.weight - 0.5f < 0.1f ? 0.1f : r.weight - 0.5f;
  }
}

// (v1.17.28-rc4, S4) "ans 12m ago" / "no answer yet": when the server's last discovery answer for this pull arrived (remote diagnosis of a stall)
FwOtaTransfer::Txt16 FwOtaTransfer::lastAnsText() const {
  Txt16 t;
  if (!_last_ans_ms) { strcpy(t.c, "ans none"); return t; }
  uint32_t m = (millis() - _last_ans_ms) / 60000;
  if (m < 120) snprintf(t.c, sizeof(t.c), "ans %lum ago", (unsigned long)m); else snprintf(t.c, sizeof(t.c), "ans %luh ago", (unsigned long)(m / 60));
  return t;
}

// (v1.17.28-rc4, S3) the server answered from behind relays: the "direct" pair a pull starts with never reaches it - drop it (unless it ever
// answered), so an unanswered route does not fail over to zero-hop offers that only the neighbours hear (board 2, 2026-09-30).
void FwOtaTransfer::routeDropDirect(int keep) {
  for (int i = 0; i < MAX_ROUTES; i++) {
    RouteRec& r = _routes[i];
    if (!r.used || i == keep || rtCount(r.enc) || r.ok || (r.has_ret && rtCount(r.ret_enc))) continue;
    if (i == _rt_cur) routeUse(keep);
    memset(&r, 0, sizeof(r));
    if (_rt_recent[0] == i) _rt_recent[0] = -1;
    if (_rt_recent[1] == i) _rt_recent[1] = -1;
  }
}

bool FwOtaTransfer::routeFailover() {
  int best = -1; float bs = -1e9f;
  for (int pass = 0; pass < 2 && best < 0; pass++)          // first avoid the last two tried (diversity), then accept them
    for (int i = 0; i < MAX_ROUTES; i++) {
      if (!_routes[i].used || i == _rt_cur) continue;
      if (pass == 0 && (i == _rt_recent[0] || i == _rt_recent[1])) continue;
      float s = routeScore(i); if (s > bs) { bs = s; best = i; }
    }
  if (best < 0) return false;
  routeUse(best); saveState();
  return true;
}

// ---- "fw pull AABB[CC] pw": find the server by a short id ----
int FwOtaTransfer::matchPeers(const uint8_t* id, uint8_t len, bool bl, int* first, char* list, size_t cap) const {
  int n = 0; size_t l = 0; if (list && cap) list[0] = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    const PeerInfo& p = _peers[i];
    if (!p.used || (p.kind == KIND_BOOTLOADER) != bl || memcmp(p.id, id, len) != 0) continue;
    if (n++ == 0 && first) *first = i;
    if (list && l + 30 < cap) l += snprintf(list + l, cap - l, "%s%02X%02X%02X%02X %.12s", l ? ", " : "", p.id[0], p.id[1], p.id[2], p.id[3], p.name);
  }
  return n;
}

void FwOtaTransfer::saveResolve() {
  if (!_fs) return;
  _fs->remove(RESOLVE_FILE);
  File f = _fs->open(RESOLVE_FILE, FILE_O_WRITE); if (!f) return;
  uint32_t m = RESOLVE_MAGIC; uint8_t bl = _res_bl;
  f.write((uint8_t*)&m, 4); f.write(_res_id, 3); f.write(&_res_len, 1); f.write(&bl, 1); f.write(_res_key, 16); f.close();
}
void FwOtaTransfer::clearResolve() { _resolving = false; _res_scanning = false; if (_fs) _fs->remove(RESOLVE_FILE); }

void FwOtaTransfer::startResolve(const uint8_t* id, uint8_t len, bool bl, const char* pw, char* reply) {
  memcpy(_res_id, id, len); if (len < 3) _res_id[2] = 0; _res_len = len; _res_bl = bl;
  deriveKey(pw, _res_key);
  _resolving = true; _res_scanning = false; _res_scans = 0; _res_interval_ms = 0; _res_next_ms = millis(); _res_msg[0] = 0;
  saveResolve();
  char hex[8]; snprintf(hex, sizeof(hex), "%02X%02X", id[0], id[1]); if (len == 3) snprintf(hex + 4, 4, "%02X", id[2]);
  snprintf(reply, 160, "OK - looking for %s (scan ~20 s); the %s starts by itself when exactly one node matches ('fw pull-status')",
           hex, bl ? "bootloader update" : "pull");
}

void FwOtaTransfer::resolveTick(uint32_t now) {
  if (!_resolving) return;
  if (!_res_scanning) {
    if ((int32_t)(now - _res_next_ms) < 0 || _scan_active || _staging || _phase == PH_DOWNLOADING || _phase == PH_OFFERING || _phase == PH_VERIFYING) return;
    startScan(0); _res_scanning = true; if (_res_scans < 65535) _res_scans++;
    return;
  }
  if (_scan_active) return;                               // our scan is still listening
  _res_scanning = false;
  char hex[8]; snprintf(hex, sizeof(hex), "%02X%02X", _res_id[0], _res_id[1]); if (_res_len == 3) snprintf(hex + 4, 4, "%02X", _res_id[2]);
  int first = -1; char list[90];
  int n = matchPeers(_res_id, _res_len, _res_bl, &first, list, sizeof(list));
  if (n > 1) {                                            // COLLISION: refuse - the user must be exact
    clearResolve();
    snprintf(_res_msg, sizeof(_res_msg), "%d nodes match %s: %s - give %s", n, hex, list, _res_len == 2 ? "3 bytes or the full id" : "the full 8-hex id");
    _phase = PH_FAILED; _last_err[0] = 0;
    return;
  }
  if (n == 0) {                                           // not in reach (yet): look again later, unattended, with backoff
    _res_interval_ms = _res_interval_ms ? umin(_res_interval_ms * 2, RES_MAX_MS) : RES_FIRST_MS;
    _res_next_ms = now + _res_interval_ms;
    return;
  }
  const PeerInfo& p = _peers[first];
  if (_res_bl) {                                          // same pre-flight as a listed bootloader offer
    if (!_bl_probed) armBlServe();
    if (_own_bl_rev && p.fw_version <= _own_bl_rev) {
      clearResolve();
      snprintf(_res_msg, sizeof(_res_msg), "%s offers bootloader rev %lu, this node runs rev %lu (not newer): nothing done",
               hex, (unsigned long)p.fw_version, (unsigned long)_own_bl_rev);
      _phase = PH_FAILED; _last_err[0] = 0;
      return;
    }
  }
  if (!_route_manual) {
    if (rtBytes(p.route_len)) memcpy(_route, p.route, rtBytes(p.route_len));
    _route_len = p.route_len; setRet(p.has_ret ? p.ret : nullptr, p.ret_len, false);
  }
  memcpy(_pull_key, _res_key, sizeof(_pull_key)); _pull_key_set = true; writePullKey();
  char target[16]; snprintf(target, sizeof(target), "@%02X%02X%02X%02X%s", p.id[0], p.id[1], p.id[2], p.id[3], _res_bl ? "!" : "");
  const bool bl = _res_bl;
  clearResolve();
  char tmp[160]; _phase = PH_IDLE; startPullAt(target, tmp, bl);
  snprintf(_last_err, sizeof(_last_err), "found %s by short id", target + 1);
}

// ---- discovery through stock repeaters ----
int FwOtaTransfer::channelMatch(const uint8_t* hash, mesh::GroupChannel* out, int max) const {
  if (!_circle_set) return 0;                              // (v1.17.28) the discovery channel is the circle's
  if (max < 1 || hash[0] != _disc_ch.hash[0]) return 0;
  out[0] = _disc_ch; return 1;
}

void FwOtaTransfer::sendDiscovery(const char* target) {
  if (!_mesh || !_circle_set) return;
  uint8_t d[5 + 48]; uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
  memcpy(d, &ts, 4); d[4] = 0;                        // like a channel text: timestamp, flags, text
  _disc_seq++;
  // " F" (v1.17.23): answer with a FLOOD, not direct along the reversed path. Older servers parse three fields and ignore it (they answer direct).
  int tl = snprintf((char*)&d[5], sizeof(d) - 5, "LOTA1 %08lX%04X %s F", (unsigned long)_disc_sess, (unsigned)_disc_seq, target);
  if (tl <= 0 || tl >= (int)sizeof(d) - 5) return;
  floodOut(_mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _disc_ch, d, 5 + tl), 100);
}

bool FwOtaTransfer::pullTargetId(uint8_t id[4]) const {
  return _man.name[0] == '@' && strlen(_man.name) >= 9 && hexN(id, 4, _man.name + 1);   // "@ID8" (app) or "@ID8!" (bootloader)
}

// A discovery flood reached us (any hop count). Answer if it is for everyone, for our id, or for our exact name, and we serve
// something: a LIST_RSP sent direct back along the reversed path, carrying the path so the puller learns its route.
void FwOtaTransfer::onDiscovery(mesh::Packet* pkt, const uint8_t* data, size_t len) {
  if (len >= 5 + 6 + 2 + XFER_MAC_LEN && (memcmp(data + 5, FLOOD_RSP, 6) == 0 || memcmp(data + 5, FLOOD_HEAL, 6) == 0)) {
    // a flooded answer (or heal notice) from a server: its collected path is the way from that server to here
    const uint8_t* b = data + 11; uint32_t bl = listRspLen(b, (uint32_t)len - 11);   // drop the AES padding
    uint8_t renc = (uint8_t)(((pkt->getPathHashSize() - 1) << 6) | pkt->getPathHashCount());
    if (!bl || b[0] != XFER_MAGIC || b[1] != MSG_LIST_RSP || !rtValid(renc) || !macOk(_list_key, b, bl)) return;
    noteRssi(0);                                        // RSSI/SNR here are the last relay's, not the server's
    onListRsp(b, bl, (int8_t)(pkt->getSNR() * 4), pkt->path, renc, data[10] == 'H');
    return;
  }
  if (len < 5 + 14 || len > 5 + 60) return;
  char txt[64]; size_t tl = len - 5; memcpy(txt, data + 5, tl); txt[tl] = 0;
  unsigned long sess = 0; unsigned seq = 0; char target[33] = {0}; char fl = 0;
  int nf = sscanf(txt, "LOTA1 %8lx%4x %32s %c", &sess, &seq, target, &fl);
  if (nf < 3) return;
  const bool flood = (nf == 4 && fl == 'F');           // the puller asks for a flooded answer (v1.17.23)
  uint8_t hops = pkt->getPathHashCount();
  uint8_t enc = (uint8_t)(((pkt->getPathHashSize() - 1) << 6) | hops);
  if (!rtValid(enc)) return;                           // too far to be pulled from anyway
  char id8[9]; snprintf(id8, sizeof(id8), "%02X%02X%02X%02X", _pub[0], _pub[1], _pub[2], _pub[3]);
  bool mine = strcmp(target, "*") == 0
           || (target[0] == '@' && strncasecmp(target + 1, id8, 8) == 0)
           || (_name_src && strcasecmp(target, _name_src) == 0);
  if (!mine) return;
  uint8_t req[8]; req[0] = XFER_MAGIC; req[1] = MSG_LIST_REQ;
  uint32_t s32 = (uint32_t)sess; uint16_t s16 = (uint16_t)seq;
  memcpy(&req[2], &s32, 4); memcpy(&req[6], &s16, 2);
  uint8_t back[MAX_ROUTE_BYTES]; rtReverse(back, pkt->path, enc);
  uint32_t d = DISC_RSP_MIN_MS + _mesh->getRNG()->nextInt(0, DISC_RSP_SPREAD_MS) + (uint32_t)hops * 300;
  const uint8_t how = discAnswer(target, hops, flood, _serving && imageStillServed(), _bl_serving);   // (v1.17.28, C2/C6/C7)
  if (!how || !answerOnce(s32, (how & 4) != 0)) return;   // (v1.17.28) once per scan / pull session
  if (how & 8) {                                        // direct, back along the reversed discovery path (C6: a pull's target also floods)
    if (how & 1) sendListRsp(req, _serve_man, d, pkt->path, enc, back);
    if (how & 2) sendListRsp(req, _bl_man, d + 700, pkt->path, enc, back);
  }
  if (how & 4) {                                        // flooded: works even where the reversed path is one-way; carries the path it came by
    const uint32_t fd = (how & 8) ? d + 1500 : d;       //   after the direct answer, so the two do not meet on the same relays
    uint8_t r[LIST_RSP_MAX];
    if (how & 1) floodListRsp(r, buildListRsp(r, &req[2], _serve_man, pkt->path, enc), fd, false);
    if (how & 2) floodListRsp(r, buildListRsp(r, &req[2], _bl_man, pkt->path, enc), fd + 700, false);
  }
}

// ret/ret_enc: the path a FLOODED answer took from the server to here = a way back that works (v1.17.23); nullptr = a direct answer.
// heal: the server's heal notice for OUR pull session (its requests arrive, its answers do not): take ret as the way back.
void FwOtaTransfer::onListRsp(const uint8_t* b, uint32_t len, int8_t snr4, const uint8_t* ret, uint8_t ret_enc, bool heal) {
  if (len < 2 + 4 + 2 + 4 + 4 + 4 + 16 + XFER_MAC_LEN) return;
  uint32_t sess; memcpy(&sess, &b[2], 4);
  const uint8_t* id = &b[8];
  uint8_t kind = KIND_APP;
  const uint8_t* rt = nullptr; uint8_t rl = 0;                    // route puller -> server (from a discovery answer)
  if (len >= 2 + 4 + 2 + 4 + 4 + 4 + 16 + 1 + 8 + XFER_MAC_LEN) {
    kind = b[36];
    if (memcmp(&b[37], g_fwpull_tag.board, 8) != 0) return;     // a server of another board has nothing we could use: not listed
    if (len >= 46 + XFER_MAC_LEN) {
      rl = b[45];                                                   // encoded route length
      if (!rtValid(rl) || len < 46u + rtBytes(rl) + XFER_MAC_LEN) return;
      rt = &b[46];
    }
  }
  if (heal) {
    if (!ret || sess != _session || !(_phase == PH_OFFERING || _phase == PH_DOWNLOADING) || kind != _man.kind) return;
    uint8_t tid[4];
    if (pullTargetId(tid) && memcmp(tid, id, 4) != 0) return;
    if (_ret_set && _ret_len == ret_enc && memcmp(_ret, ret, rtBytes(ret_enc)) == 0) return;   // already using it
    if (_ret_typed) {                                               // the operator typed the way back: keep it while it works
      if (!_typed_failed) return;
      // (v1.17.26, N1) the typed route failed (like a MeshCore saved path: a couple of unanswered attempts, then flood). 2026-09-28: board 2
      // stalled 25 min on a typed pair while the server's heals offered a working way back. Use the healed way back as a NEW pair - the
      // typed pair stays in the table ('fw route' shows both).
      int ri = routeAdd(_route, _route_len, ret, ret_enc);
      setRet(ret, ret_enc, false);
      if (ri >= 0) routeUse(ri);
      saveState();
      snprintf(_last_err, sizeof(_last_err), "typed route failed: way back healed, %u hop(s)", (unsigned)rtCount(ret_enc));
      if (_phase == PH_OFFERING) { _last_req_ms = 0; _retry_timeout_ms = 0; }
      return;
    }
    setRet(ret, ret_enc, false);
    if (_rt_cur >= 0 && _routes[_rt_cur].used) {                   // the pair in use gets the working way back
      RouteRec& r = _routes[_rt_cur]; r.has_ret = true; r.ret_enc = ret_enc; if (rtBytes(ret_enc)) memcpy(r.ret, ret, rtBytes(ret_enc));
    }
    saveState();
    snprintf(_last_err, sizeof(_last_err), "way back healed: %u hop(s)", (unsigned)rtCount(ret_enc));
    if (_phase == PH_OFFERING) { _last_req_ms = 0; _retry_timeout_ms = 0; }   // ask again right away
    return;
  }
  if (!_scan_active && sess == _disc_sess && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING)) {
    // an answer to the re-discovery of the server we are pulling from: adopt the path (any change while waiting, a shorter one while downloading)
    uint8_t tid[4];
    if (!pullTargetId(tid) || memcmp(tid, id, 4) != 0 || kind != _man.kind) return;
    _last_ans_ms = millis() ? millis() : 1;                 // (rc4, S4)
    int ri = routeAdd(rt ? rt : _route, rl, ret, ret_enc);  // every answer is a route (pair) to the server (the table keeps the best 4)
    if (ri >= 0 && rtCount(rl) && !_route_manual) routeDropDirect(ri);   // (rc4, S3) the server is behind relays: no zero-hop offers any more
    if (ri < 0 || (_route_manual && !_typed_failed) || ri == _rt_cur) return;   // a typed route is replaced only after it failed (N1)
    if (_route_manual) {                                    // (v1.17.26, N1) the typed route failed and the re-discovery found one: take it, automatic from here
      _route_manual = false; _ret_typed = false;
      routeUse(ri); saveState(); _last_req_ms = 0; _retry_timeout_ms = 0;
      snprintf(_last_err, sizeof(_last_err), "typed route failed: rediscovered, %u hop(s)", (unsigned)rtCount(rl));
      return;
    }
    const bool cur_bad = _rt_cur < 0 || _routes[_rt_cur].fail > _routes[_rt_cur].ok;
    if (_phase == PH_OFFERING || rtCount(rl) < rtCount(_route_len) || cur_bad) {   // waiting: try it now; downloading: if shorter or ours is failing
      routeUse(ri); saveState();
      snprintf(_last_err, sizeof(_last_err), "route updated: %u hop(s)", (unsigned)rtCount(rl));
      if (_phase == PH_OFFERING) { _last_req_ms = 0; _retry_timeout_ms = 0; }   // ask the server right away along the new path
    }
    return;
  }
  if (!_scan_active || sess != _scan_sess) return;
  PeerInfo* p = nullptr;
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used && memcmp(_peers[i].id, id, 4) == 0 && _peers[i].kind == kind) { p = &_peers[i]; break; }
  if (p) {                                                        // heard again (zero-hop and flooded answers): keep the shortest path
    if (rtCount(rl) < rtCount(p->route_len)) {
      if (rtBytes(rl)) memcpy(p->route, rt, rtBytes(rl));
      p->route_len = rl; p->has_ret = ret != nullptr; p->ret_len = ret ? ret_enc : 0;
      if (ret && rtBytes(ret_enc)) memcpy(p->ret, ret, rtBytes(ret_enc));
    } else if (ret && !p->has_ret && rtCount(rl) == rtCount(p->route_len)) {   // same way there, now with a known way back
      p->has_ret = true; p->ret_len = ret_enc; if (rtBytes(ret_enc)) memcpy(p->ret, ret, rtBytes(ret_enc));
    }
    if (rl == 0 && !ret) { p->rssi = _rx_rssi; p->snr4 = snr4; }
    return;
  }
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (!_peers[i].used) { p = &_peers[i]; break; }
  if (!p) return;
  p->used = true; memcpy(p->id, id, 4); p->kind = kind;
  memcpy(&p->fw_version, &b[12], 4); memcpy(&p->payload_len, &b[16], 4);
  memcpy(p->name, &b[20], 16); p->name[16] = 0;
  p->rssi = _rx_rssi; p->snr4 = snr4;
  if (rtBytes(rl)) memcpy(p->route, rt, rtBytes(rl));
  p->route_len = rl;
  p->has_ret = ret != nullptr; p->ret_len = ret ? ret_enc : 0;
  if (ret && rtBytes(ret_enc)) memcpy(p->ret, ret, rtBytes(ret_enc));
}

bool FwOtaTransfer::cmdPeers(const char* arg, char* reply) {
  // "fw peers [n]" (v1.17.23: one block per host): "AABBCCDD NAME -37dBm snr12.75 [2hop] #1 fw-210 #2 bl-rev-3 | next host ...". The #n numbers
  // are the same as before (every offer in table order), so 'fw pull #n' / 'fw blupdate #n' are unchanged. A page never splits a host and never
  // drops an entry: what does not fit opens the next page, which the reply names (lesson of v1.17.18: #3 used to vanish between pages).
  if (_scan_active) { snprintf(reply, 160, "still scanning (%lu s left)...", (unsigned long)((_scan_end_ms - millis()) / 1000 + 1)); return true; }
  while (*arg == ' ') arg++;
  int from = atoi(arg); if (from < 1) from = 1;
  uint8_t num[MAX_PEERS] = {0}, total = 0;                     // #n of each table entry
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used) num[i] = ++total;
  if (!total) { strcpy(reply, "no peers offering firmware (run 'fw list' first)"); return true; }
  if (from > total) { snprintf(reply, 160, "no peer #%d (%u found)", from, (unsigned)total); return true; }
  // hosts in order of their first offer; the page starts at the host that holds #from
  int8_t host_of[MAX_PEERS]; uint8_t hosts[MAX_PEERS], nh = 0;   // host_of[i] = index into hosts[] ; hosts[h] = table index of the host's first offer
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    host_of[i] = -1; if (!_peers[i].used) continue;
    for (uint8_t h = 0; h < nh && host_of[i] < 0; h++) if (memcmp(_peers[hosts[h]].id, _peers[i].id, 4) == 0) host_of[i] = (int8_t)h;
    if (host_of[i] < 0) { host_of[i] = (int8_t)nh; hosts[nh++] = i; }
  }
  uint8_t h0 = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used && num[i] == from) h0 = (uint8_t)host_of[i];
  const size_t ROOM = 160 - 24;                                 // keep room for " | ...'fw peers NN'"
  size_t l = 0; reply[0] = 0; uint8_t shown = 0, next = 0;
  for (uint8_t h = h0; h < nh; h++) {
    const PeerInfo& p = _peers[hosts[h]];
    char e[140];
    char rs[10]; if (p.rssi < 0) snprintf(rs, sizeof(rs), "%ddBm", (int)p.rssi); else strcpy(rs, "rssi?");   // radio reported no RSSI (or a flooded answer)
    char via[12] = "";
    if (rtCount(p.route_len)) snprintf(via, sizeof(via), " %uhop", (unsigned)rtCount(p.route_len));
    int w = snprintf(e, sizeof(e), "%s%02X%02X%02X%02X %.16s %s snr%s%d.%02d%s", shown ? " | " : "", p.id[0], p.id[1], p.id[2], p.id[3], p.name, rs,
                     p.snr4 < 0 ? "-" : "", abs(p.snr4) / 4, (abs(p.snr4) % 4) * 25, via);   // sign printed separately: -0.5 dB used to show as 0.50
    for (uint8_t i = 0; i < MAX_PEERS && w > 0 && (size_t)w < sizeof(e); i++) {
      if (host_of[i] != (int8_t)h) continue;
      const PeerInfo& o = _peers[i];
      w += snprintf(e + w, sizeof(e) - (size_t)w, " #%u %s-%lu", (unsigned)num[i], o.kind == KIND_BOOTLOADER ? "bl-rev" : "fw", (unsigned long)o.fw_version);
    }
    if (w < 0) continue;
    if ((size_t)w >= sizeof(e)) w = (int)sizeof(e) - 1;
    if (l + (size_t)w > ROOM && shown) {                        // does not fit: the whole host opens the next page instead of being lost
      for (uint8_t i = 0; i < MAX_PEERS && !next; i++) if (host_of[i] == (int8_t)h) next = num[i];
      break;
    }
    if (l + (size_t)w > ROOM) w = (int)(ROOM - l);               // (only a pathological single host is cut)
    memcpy(reply + l, e, (size_t)w); l += (size_t)w; reply[l] = 0;
    shown++;
  }
  if (next) { size_t n = strlen(reply); snprintf(reply + n, 160 - n, " | ...'fw peers %u'", (unsigned)next); }
  return true;
}

// "AA,BB" | "AABB,.." | "AABBCC,.." (1/2/3-byte relay hashes, same size, max MAX_HOPS) up to *end -> tmp + encoded length; false = bad input
static bool parseHops(const char* arg, const char* end, uint8_t* tmp, uint8_t* enc) {
  uint8_t n = 0, hs = 0, nb = 0;
  while (arg < end) {
    const char* e = arg; while (e < end && isxdigit((unsigned char)*e)) e++;
    uint8_t hl = (uint8_t)(e - arg);
    if (!(hl == 2 || hl == 4 || hl == 6) || (hs && hl / 2 != hs) || n >= MAX_HOPS || nb + hl / 2 > MAX_ROUTE_BYTES || !hexN(tmp + nb, hl / 2, arg)) return false;
    hs = hl / 2; nb += hs; n++; arg = e;
    if (arg < end && *arg == ',') { arg++; if (arg >= end) return false; }   // a trailing comma is a typo, not an empty hop
    else if (arg < end) return false;
  }
  if (!n) return false;
  *enc = (uint8_t)(((hs - 1) << 6) | n);
  return true;
}

// hops as typed: "3C4D5E,6F7081", or "direct"
static size_t printHops(char* out, size_t cap, const uint8_t* path, uint8_t enc) {
  size_t l = 0; uint8_t s = rtSize(enc), c = rtCount(enc);
  if (!c) return (size_t)snprintf(out, cap, "direct");
  for (uint8_t h = 0; h < c && l + 8 < cap; h++) {
    if (h) out[l++] = ',';
    for (uint8_t j = 0; j < s; j++) l += (size_t)snprintf(out + l, cap - l, "%02X", path[h * s + j]);
  }
  return l;
}

// "fw route" = the routes of the pull + statistics; "fw route AA,BB.." (1/2/3-byte hashes) = typed = manual override (also during a pull);
// "fw route <there>/<back>" (v1.17.23) = a typed PAIR: requests go <there>, the server answers along <back> (one-way links);
// "fw route auto" = automatic again (keeps the current route as the starting point); "fw route off" = direct, automatic.
bool FwOtaTransfer::cmdRoute(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  const bool in_pull = (_phase == PH_DOWNLOADING || _phase == PH_OFFERING);
  if (strcmp(arg, "auto") == 0) {
    _route_manual = false; if (_ret_typed) _ret_typed = false; _typed_failed = false;
    if (!in_pull) routeReset();
    strcpy(reply, "route: automatic (discovered route pairs, failover on unanswered requests, heal of the way back)");
    return true;
  }
  if (strcmp(arg, "off") == 0) { _route_len = 0; _route_manual = false; setRet(nullptr, 0, false); routeUse(routeAdd(_route, 0)); if (in_pull) saveState(); }
  else if (*arg) {
    const char* end = arg + strlen(arg); while (end > arg && end[-1] == ' ') end--;
    const char* sl = (const char*)memchr(arg, '/', (size_t)(end - arg));
    uint8_t tmp[MAX_ROUTE_BYTES], enc = 0, rtmp[MAX_ROUTE_BYTES], renc = 0;
    bool ok = parseHops(arg, sl ? sl : end, tmp, &enc);
    if (ok && sl) { const char* r = sl + 1; ok = (r < end) && (strncmp(r, "direct", 6) == 0 ? (renc = 0, r + 6 == end) : parseHops(r, end, rtmp, &renc)); }
    if (!ok) { snprintf(reply, 160, "ERR - usage: fw route AA[,BB..] | AABB[,..] | AABBCC[,..] [/<way back>] (relay hashes, max %u) | auto | off", (unsigned)MAX_HOPS); return true; }
    int ri = routeAdd(tmp, enc, sl ? rtmp : nullptr, renc);
    if (ri < 0) { strcpy(reply, "ERR - route table full"); return true; }
    routeUse(ri); _route_manual = true; _ret_typed = sl != nullptr; _typed_failed = false;
    if (in_pull) { saveState(); _last_req_ms = 0; _retry_timeout_ms = 0; }   // use it right away
  }
  // the table (the one in use is marked *): hops there[/back], answered/unanswered, reply time
  size_t l = (size_t)snprintf(reply, 160, "route %s:", _route_manual ? "TYPED" : "auto");
  bool any = false;
  for (int i = 0; i < MAX_ROUTES && l < 150; i++) {
    const RouteRec& r = _routes[i];
    if (!r.used) continue;
    any = true;
    l += (size_t)snprintf(reply + l, 160 - l, " %s%d ", i == _rt_cur ? "*" : "", i + 1);
    if (l < 150) l += printHops(reply + l, 160 - l, r.path, r.enc);
    if (r.has_ret && l < 146) { reply[l++] = '/'; reply[l] = 0; l += printHops(reply + l, 160 - l, r.ret, r.ret_enc); }
    if (l < 150) l += (size_t)snprintf(reply + l, 160 - l, " %u/%u", (unsigned)r.ok, (unsigned)r.fail);
    if (r.rtt_ms && l < 150) l += (size_t)snprintf(reply + l, 160 - l, " %ums", (unsigned)r.rtt_ms);
  }
  if (!any) snprintf(reply + l, 160 - l, " direct (no pull yet; a listed peer's discovered route is used automatically)");
  return true;
}

// "fw pull <id|#n> [password]"
bool FwOtaTransfer::cmdPullPeer(const char* args, char* reply, bool bl, bool allow_prompt) {
  if (!_circle_set) { strcpy(reply, "ERR - set 'fw passphrase <phrase>' first (the same on every node of your circle)"); return true; }
  // During a pull the same command SWITCHES the source (checked below, once the peer is known): the chunks are kept when the new server
  // offers the identical image. That is how a pull survives a server that went away for good.
  const bool in_pull = (_phase == PH_DOWNLOADING || _phase == PH_OFFERING);
  if (in_pull && (_man.kind == KIND_BOOTLOADER) != bl) { strcpy(reply, "ERR - a pull of the other kind is in progress ('fw pull-abort' to cancel)"); return true; }
  while (*args == ' ') args++;
  _bl_force = false;
  if (bl && strncmp(args, "force ", 6) == 0) { _bl_force = true; args += 6; while (*args == ' ') args++; }
  char tok[12]; uint8_t tl = 0;
  while (*args && *args != ' ' && tl < sizeof(tok) - 1) tok[tl++] = *args++;
  tok[tl] = 0;
  while (*args == ' ') args++;
  int idx = -1;
  if (tok[0] == '#' || (tl <= 2 && tok[0] >= '0' && tok[0] <= '9')) {
    int want = atoi(tok[0] == '#' ? tok + 1 : tok), k = 0;
    for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used && ++k == want) { idx = i; break; }
  } else if (tl >= 4) {
    int hits = 0; char list[90]; size_t ll = 0; list[0] = 0;
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
      if (!_peers[i].used) continue;
      char hx[9]; snprintf(hx, sizeof(hx), "%02X%02X%02X%02X", _peers[i].id[0], _peers[i].id[1], _peers[i].id[2], _peers[i].id[3]);
      if (strncasecmp(hx, tok, tl) == 0 && (_peers[i].kind == KIND_BOOTLOADER) == bl) {
        if (hits++ == 0) idx = i;
        if (ll + 30 < sizeof(list)) ll += (size_t)snprintf(list + ll, sizeof(list) - ll, "%s%s %.12s", ll ? ", " : "", hx, _peers[i].name);
      }
    }
    if (hits > 1) { snprintf(reply, 160, "ERR - %d peers match %s: %s - give more of the id (or #n)", hits, tok, list); return true; }   // collision: refuse
  }
  if (idx < 0 && tl >= 3) {         // by node name (prefix, case-insensitive), as listed by fw peers
    for (uint8_t i = 0; i < MAX_PEERS; i++)
      if (_peers[i].used && strncasecmp(_peers[i].name, tok, tl) == 0 && (_peers[i].kind == KIND_BOOTLOADER) == bl) { idx = i; break; }
  }
  if (idx < 0 && tl == 8) {         // a full 8-hex id that fw list could not see (e.g. behind relays): pull from it directly
    uint8_t id[4];
    if (bl && !_route_len) { strcpy(reply, "ERR - a bootloader update needs a compatible offer in reach: run 'fw list' first (behind relays: 'fw route ..' then the full 8-hex id)"); return true; }
    if (hexN(id, 4, tok)) {                              // (v1.17.28) hexN: Utils::fromHex() takes 'status' for 00A000
      for (uint8_t i = 0; i < MAX_PEERS && idx < 0; i++) if (!_peers[i].used) idx = i;
      if (idx < 0) idx = MAX_PEERS - 1;
      memset(&_peers[idx], 0, sizeof(PeerInfo)); memcpy(_peers[idx].id, id, 4); strcpy(_peers[idx].name, "(not listed)"); _peers[idx].used = true;
      _peers[idx].kind = bl ? KIND_BOOTLOADER : KIND_APP;
    }
  }
  if (idx < 0 && (tl == 4 || tl == 6) && !in_pull) {       // a 2/3-byte id nobody listed yet: find it by a scan, unattended
    uint8_t sid[3];
    if (hexN(sid, tl / 2, tok)) {                        // (v1.17.28) a typo ('fw pull status') is not a short id to search for forever
      startResolve(sid, tl / 2, bl, "-", reply); return true;   // (v1.17.28) no password: the circle authenticates both ends
    }
  }
  if (idx < 0) { strcpy(reply, "ERR - unknown peer: 'fw list' first, or give 2/3 bytes of its id (AABB / AABBCC) or the full 8-hex id"); return true; }
  const PeerInfo& p = _peers[idx];
  if (in_pull && !_switching) {
    memcpy(_switch_prev_route, _route, sizeof(_switch_prev_route)); _switch_prev_route_len = _route_len;
    memcpy(_switch_prev_ret, _ret, sizeof(_switch_prev_ret)); _switch_prev_ret_len = _ret_len; _switch_prev_ret_set = _ret_set;
  }
  if (!_route_manual && strcmp(p.name, "(not listed)") != 0) {   // use the path the discovery found (empty = direct) and the way back it learnt
    if (rtBytes(p.route_len)) memcpy(_route, p.route, rtBytes(p.route_len));
    _route_len = p.route_len; setRet(p.has_ret ? p.ret : nullptr, p.ret_len, false);
  }
  if ((p.kind == KIND_BOOTLOADER) != bl) {
    strcpy(reply, bl ? "ERR - that peer offers firmware, not a bootloader: use 'fw pull'" : "ERR - that is a bootloader offer: use 'fw blupdate'");
    return true;
  }
  if (in_pull) {                                          // source switch pre-flight: must be the same image version, and another server
    char want[16]; snprintf(want, sizeof(want), "@%02X%02X%02X%02X%s", p.id[0], p.id[1], p.id[2], p.id[3], bl ? "!" : "");
    const uint32_t cur_ver = _man.fw_version ? _man.fw_version : _saved_man.fw_version;
    if (strcmp(want, _man.name) == 0) { strcpy(reply, "ERR - that is the server this pull already uses ('fw pull-status')"); return true; }
    if (cur_ver && strcmp(p.name, "(not listed)") == 0) { strcpy(reply, "ERR - to switch, pick the new server from 'fw list' / 'fw peers' (its version is checked first)"); return true; }
    if (cur_ver && p.fw_version != cur_ver) {
      snprintf(reply, 160, "ERR - that peer offers %s-%lu, this pull is %s-%lu: not the same image ('fw pull-abort' to start over)",
               bl ? "rev" : "fw", (unsigned long)p.fw_version, bl ? "rev" : "fw", (unsigned long)cur_ver);
      return true;
    }
  }
  if (bl && !in_pull) {                                   // pre-flight: nothing is touched unless this offer is one we may take
    if (!_bl_probed) armBlServe();                        // learns the running bootloader's revision
    if (!_bl_force && _own_bl_rev && p.fw_version <= _own_bl_rev) {
      snprintf(reply, 160, "ERR - this node already runs bootloader rev %lu; the offer is rev %lu (not newer). 'fw blupdate force ..' overrides",
               (unsigned long)_own_bl_rev, (unsigned long)p.fw_version);
      return true;
    }
  }
  _await_peer = (uint8_t)idx; _await_bl = bl;
  (void)allow_prompt;                                      // (v1.17.28) no password any more (the circle authenticates both ends);
  return submitPassword("-", reply);                       // a word typed after the id (an old habit: the password) is ignored
}

bool FwOtaTransfer::submitPassword(const char* pw, char* reply) {
  _await_pw = false;
  if (!*pw) { _await_res = false; strcpy(reply, "cancelled"); return true; }
  if (_await_res) { _await_res = false; startResolve(_res_id, _res_len, _res_bl, pw, reply); return true; }
  const PeerInfo& p = _peers[_await_peer];
  char target[16]; snprintf(target, sizeof(target), "@%02X%02X%02X%02X%s", p.id[0], p.id[1], p.id[2], p.id[3], _await_bl ? "!" : "");
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { switchSource(target, pw, reply); return true; }
  deriveKey(pw, _pull_key); _pull_key_set = true; writePullKey();
  startPullAt(target, reply, _await_bl);
  return true;
}

// Re-target a running pull to another server. It is treated like a resume after an outage: the new server is asked for its offer, and if
// the image is identical (sameImage: sizes, CRCs, SHA-256, version, LZMA props) the download continues from the first missing chunk - the
// sparse chunk store is keyed by image, not by server. A different image restores the previous server and key; nothing is erased.
void FwOtaTransfer::revertSwitch(const char* why) {   // undo switchSource(): previous server + key, keep every chunk, keep waiting/resuming
  memset(_man.name, 0, sizeof(_man.name)); strncpy(_man.name, _switch_prev_name, sizeof(_man.name) - 1);
  memcpy(_pull_key, _switch_prev_key, sizeof(_pull_key)); writePullKey();
  memcpy(_route, _switch_prev_route, sizeof(_switch_prev_route)); _route_len = _switch_prev_route_len;
  setRet(_switch_prev_ret_set ? _switch_prev_ret : nullptr, _switch_prev_ret_len, false);
  routeReset();                                            // routes of the previous server again (from its route)
  _switching = false; _resuming = true; _bad_auth = 0; _phase = PH_OFFERING;
  _prev_session = _session; _session = ((uint32_t)millis() << 8) ^ 0x3C3C ^ _cm.have; _sess_set = false; _attempts = 0; _backoff_ms = 0; _wait_started_ms = millis();
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
  saveState();
}

void FwOtaTransfer::switchSource(const char* target, const char* pw, char* reply) {
  if (!_saved_man.n_chunks && !_man.n_chunks) {           // still at first contact: nothing downloaded to keep -> simply a new pull
    _phase = PH_IDLE; deriveKey(pw, _pull_key); _pull_key_set = true; writePullKey();
    startPullAt(target, reply, _man.kind == KIND_BOOTLOADER);
    return;
  }
  if (!_switching) {                                      // keep what to go back to (a second switch before the first answered keeps the original)
    strncpy(_switch_prev_name, _man.name, sizeof(_switch_prev_name) - 1); _switch_prev_name[sizeof(_switch_prev_name) - 1] = 0;
    memcpy(_switch_prev_key, _pull_key, sizeof(_switch_prev_key));
  }
  if (_phase == PH_DOWNLOADING) _saved_man = _man;       // identity of what is in the slot now
  if (_next_pull_duty) { _pull_duty_pct = _next_pull_duty; _next_pull_duty = 0; }   // "fw pull <other> pw duty x" re-targets at that duty
  deriveKey(pw, _pull_key); _pull_key_set = true; writePullKey();
  memset(_man.name, 0, sizeof(_man.name)); strncpy(_man.name, target, sizeof(_man.name) - 1);
  routeReset();                                            // another server: its routes start from the one fw list found
  _switching = true; _resuming = true; _bad_auth = 0; _mac_fail = 0;
  _prev_session = _session; _session = ((uint32_t)millis() << 8) ^ 0x5A5A ^ _cm.have; _sess_set = false;
  _win_count = 0; _retries = 0; _last_req_ms = 0; _attempts = 0; _backoff_ms = 0; _retry_timeout_ms = 0;
  _wait_started_ms = millis();
  _phase = PH_OFFERING;
  strncpy(_last_err, "switching source", sizeof(_last_err) - 1);
  saveState();
  snprintf(reply, 160, "OK - switching the pull to %s: %lu/%lu chunks are kept if it serves the same image ('fw pull-status')",
           target, (unsigned long)_cm.have, (unsigned long)_saved_man.n_chunks);
}

bool FwOtaTransfer::cmdAutoApply(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (strcmp(arg, "off") == 0) {
    _noapply = true; _apply_armed = false;
    if (_fs) { _fs->remove(NOAPPLY_FILE); File f = _fs->open(NOAPPLY_FILE, FILE_O_WRITE); if (f) { f.write((const uint8_t*)"1", 1); f.close(); } }
  } else if (strcmp(arg, "on") == 0) {
    _noapply = false; if (_fs) _fs->remove(NOAPPLY_FILE);
  }
  snprintf(reply, 160, "auto-apply after a verified pull: %s ('fw autoapply on|off')", _noapply ? "OFF" : "on");
  return true;
}

}  // namespace fw_ota
