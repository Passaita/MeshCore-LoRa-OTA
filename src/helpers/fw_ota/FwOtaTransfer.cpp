#ifndef FW_OTA_STUDY                // (the simulator's sanitizer build is 14x slower with it)
#pragma GCC optimize ("Os")   // (rc19, size) protocol + CLI logic: size over speed (the framework builds -Ofast: 32 KB more here; the crypto libraries keep it)
#endif
#include "FwOtaTransfer.h"
#include <string.h>
#include <SHA256.h>
#include <AES.h>        // (v1.17.28) AES-128-CTR of the chunk data
#include <nrf_mbr.h>    // sd_mbr_command(COPY_BL)
#include <nrf_sdm.h>    // sd_softdevice_disable()
#include <nrf_soc.h>    // sd_power_gpregret_*() (v1.17.28: `fw pull-ble` restart)
#include "Watchdog.h"   // (rc8, C23)

namespace fw_ota {

FwOtaTransfer g_xfer;
Watchdog      g_wdt;    // (rc8, C23) started in setup(), fed from the main loop

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
static const char*    NOAPPLY_FILE     = "/fwota.noapply";        // present = do NOT auto-apply a completed pull
static const char*    WDT_FILE         = "/fwota.wdt";            // (rc8) number of watchdog resets (written only when one happened)
static const char*    WDD_FILE         = "/fwota.wdd";            // (rc10, C27) a planned restart is pending: the old firmware's timeout runs out
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
static const char*    RESOLVE_FILE     = "/fwota.resolve";        // "fw pull AABB[CC]" still looking for its server (survives a reboot)
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
static const char     FLOOD_ANN[]      = "LOTA1A";                // (swarm v1) discovery-channel flood: "I now hold this image" (session 0)
static const char*    MINBATT_FILE     = "/fwota.minb";           // (swarm v1) 'fw minbatt' (2 bytes, mV)
static const char*    ANN_FILE         = "/fwota.ann";            // (swarm v1) announce the served image after the next boot (an install)
static const uint32_t SWITCH_MIN_MS    = 120000;                  // (swarm v1, owner 8.3) give a holder up after 6 missed windows, never < 2 min
static const uint16_t POWER_CRIT_MV    = 150;                     // (swarm v1) CRITICAL = this far below 'fw minbatt'
static const uint32_t DEMAND_LIVE_MS   = 300000;                  // (rc15) an overheard pull counts as demand while it was heard in the last 5 min
static const uint32_t ADV_FIRST_IVL_MS = 1800000;                 // (rc15) idle holder + demand: quiet 30 min after an advert ...
static const uint32_t ADV_MAX_IVL_MS   = 7200000;                 //        ... doubling to 2 h while nobody comes (owner 2026-10-06)
static const uint32_t MOVE_HOLD_MS     = 600000;                  // (rc15) at most one move to a faster holder per 10 min ...
static const uint32_t MOVE_JITTER_MS   = 180000;                  //        ... after a random 0-3 min wait (no herd) ...
static const uint32_t MOVE_EVAL_MS     = 900000;                  // (rc20, C48) a move is judged by its measured rate after 15 min ...
static const uint32_t MOVE_BAR_MS      = 7200000;                 //        ... a slower holder is not moved to again for 2 h
static const uint32_t MOVE_CHECK_MS    = 600000;                  //        ... checked every 10 min while downloading and on every holder notice
static const uint32_t ANS_HOP_MS       = 1500;                    // (rc15) a flooded answer waits 1.5 s per hop: near holders answer first ...
static const uint8_t  ANS_SUPPRESS_K   = 2;                       //        ... and drops when 2 answers for the same image were heard
static const uint32_t STREAM_T_MIN_MS  = 30000;                   // (rc15) a shared stream counts as live while a chunk of it came within
static const uint32_t STREAM_T_MAX_MS  = 300000;                  //        2x its largest gap (30 s .. 5 min)
static const uint32_t DISC_FLOOD_AFTER_MS = 8000;                 // (rc15) a search asks the neighbours first; the flood only if nobody answered
static const char*    CIRCLE_FILE      = "/fwota.circle";         // (v1.17.28) the circle key C (32 B, PBKDF2 of the owner's passphrase; never the phrase)
static const uint32_t PBKDF2_ITER      = 50000;                   // ~2-4 s once on the nRF52840 (`fw passphrase`)
static const uint8_t  PASS_MIN         = 8;
static const char*    PAUSE_FILE       = "/fwota.pause";          // (v1.17.28, C8) present = the pull is paused; 1 byte: hours (0 = until fw pull-resume)
static const char*    BLEFILL_FILE     = "/fwota.blefill";        // (v1.17.28) payload CRC-32 of the slot image uploaded over BLE (`fw pull-ble`)
static const uint32_t LOTA_HDR         = 160;                     // .lota header (lora-ota-serve doc/LOTA-FORMAT.md)
static const uint8_t  GPREGRET_SLOT_DFU = 0xA9;                   // bootloader rev 5: BLE DFU into the slot
static const uint16_t BLE_WARN_MV      = 3450;                    // (rc19, C46) below this on battery a Bluetooth update may brown out (B4)
static const char*    DUTY_FILE        = "/fwota.duty";           // (v1.17.25) the node's LoRa-OTA duty in percent (absent = DUTY_DEFAULT_PCT)
static const uint32_t CHUNK_GAP_MS     = 90;                      // spacing of the chunks of one window at duty 1.0 (as before v1.17.25)
static const uint32_t CHUNK_AIR_FALLBACK_MS = 650;                // a chunk's airtime when the radio gives no estimate (SF7/62.5 kHz: ~610 ms)
static const uint32_t DUTY_WIN_MS      = 10000;                   // a paced window lasts at most ~10 s (fewer chunks per window at a low duty)
static const uint32_t DUTY_ACTIVE_MS   = 60000;                   // server: the SHORTEST time a session holds its share without a request (rc11: 3x its interval)
static const uint32_t SERVE_ACTIVE_MS  = 600000;                  // (rc10, C22) server: a puller counts as active in 'fw pull-status' for 10 min after its last request
static const uint32_t REAGREE_FIRST_MS = 900000;                  // (rc10, C26) puller: granted less than it asked -> ask again after 15 min ...
static const uint32_t REAGREE_MAX_MS   = 7200000;                 //     ... doubling up to 2 h while the share does not grow
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

static const uint32_t FANN_IVL_MS      = 300000;                  // (rc17) a puller announces its stream every 5 min (first: when its slot is erased)
static const uint32_t FILL_ARM_MS      = 600000;                  // (rc17) an accepted announce waits this long for a chunk of its stream
static const uint32_t GAP_SPREAD_MS    = 8000;                    // (rc17) a gap round starts after a random 0-8 s (another node's request first)
static const uint32_t GAP_COVER_MS     = 30000;                   //        chunks another node asked for: not asked again for 30 s
static const char     FLOOD_TREQ[]     = "LOTA1Q";                // (rc21, C57) zero-hop time request (MSG_TIME_REQ inside); older nodes ignore it
static const char     FLOOD_TANS[]     = "LOTA1T";                // (rc21, C57) zero-hop time answer (MSG_TIME_RSP inside)
// (rc21, C57) time from a swarm member: a node without an RTC asks its neighbours 20-40 s after boot, again after 2 / 10 / 30 min while
// nobody answered, then every 6 h (drift). Nodes with a valid RTC answer in a random 0.5 s slot (0.3-3.8 s); one that hears another
// answer first drops its own.
static const uint32_t TQ_FIRST_MS      = 20000;
static const uint32_t TQ_PERIOD_MS     = 6UL * 3600UL * 1000UL;
static const uint32_t TA_MIN_MS        = 300, TA_SLOT_MS = 500, TA_SLOTS = 8;   // answer in one of 8 slots 0.5 s apart (> an answer's airtime)
static const int32_t  CLK_STEP_MIN_S   = 2;                       // behind by this much: step forward
static const int32_t  CLK_HOLD_MIN_S   = 3;                       // ahead by this much: hold (the answer left 0-1 s before it arrived)
static const int32_t  CLK_BOUND_S      = 3600;                    // never hold more than 1 h; after the first sync, never step more than 1 h
static const uint32_t CLOCK_APPLY_S    = 15;                      // (rc21, C58) the shortest apply measured (2026-09-19: 15.5 s)
static const char     FLOOD_CLAIM[]    = "LOTA1V";                // (study C/D) discovery-channel flood: an election claim (MSG_CLAIM inside)

static const uint32_t PREV_FLEET = 0xFFFFFFFEu;   // (rc18) OFFER_REQ2 'session it replaces' of a fresh fleet pull
#ifdef FW_OTA_STUDY
SwarmTune g_tune;                                                            // (study) the gap-fill algorithm and its parameters (SwarmTune)
#endif
static uint8_t repCap() { return g_tune.gap_max > GAP_REP_CAP ? GAP_REP_CAP : g_tune.gap_max; }   // (rc17: 24 per request and per repair job)

static inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
// (rc15) time elapsed since t, 0 when t is (just) in the future: tick() reads 'now' once, a stamp taken later with millis() can be 1+ ms
// newer - 'now - t' then wrapped to ~49 days (found by the simulator: a move to a faster holder was undone by the failover in the same tick)
static inline uint32_t since(uint32_t now, uint32_t t) { return (int32_t)(now - t) > 0 ? now - t : 0; }

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
  const bool tgt = target[0] == '@' || target[0] == '=';   // (swarm v1) an image search is targeted too
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
  if (type == MSG_CHUNK || type == MSG_PACE) return _sess_set ? _sess_mac : nullptr;   // (rc11) PACE: our session key
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
  uint8_t d = 0;                                           // (rc7, C18) constant time: no early exit on the first differing byte
  for (uint8_t i = 0; i < XFER_MAC_LEN; i++) d |= (uint8_t)(full[i] ^ pkt[len - XFER_MAC_LEN + i]);
  return d == 0;
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
  if (_img_set || _fill) {                // (swarm v1) a pull by image: the image and its holders (trailing - older firmware stops before it)
    uint8_t mk = 0xA5; f.write(&mk, 1); f.write(&_img_kind, 1); f.write((uint8_t*)&_img_ver, 4); f.write(_img_digest, 8);
    f.write(&_nholders, 1); f.write((uint8_t*)_holders, sizeof(_holders));
    uint8_t fm = _fill ? (uint8_t)(0xF0 | _fill_mode) : _fleet_mode == FLEET_REPLACE ? 0xE2 : 0; f.write(&fm, 1);   // (rc17) a fleet fill
  } else if (_fleet_mode == FLEET_REPLACE) {   // (rc18) a fleet pull by id before its first answer: only the flag (older firmware ignores it)
    uint8_t mk = 0xA6, fm = 0xE2; f.write(&mk, 1); f.write(&fm, 1);   //   (never installed) and its mode; E2 = a fleet pull
  }
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
  _img_set = false; _nholders = 0; _holders_tried = 0; _fill = false; _fill_mode = 0; _fleet_mode = FLEET_FILL_EMPTY;
  if (ok && magic == STATE_MAGIC) {                   // v8: the pull's duty override
    uint8_t d = 0;
    if (f.read(&d, 1) == 1 && d >= DUTY_MIN_PCT && d <= 100) _pull_duty_pct = d;
    uint8_t mk = 0;                                    // (swarm v1) the image and its holders
    if (f.read(&mk, 1) == 1 && mk == 0xA5 && f.read(&_img_kind, 1) == 1 && f.read((uint8_t*)&_img_ver, 4) == 4 && f.read(_img_digest, 8) == 8
        && f.read(&_nholders, 1) == 1 && f.read((uint8_t*)_holders, sizeof(_holders)) == (int)sizeof(_holders) && _nholders <= SWARM_HOLDERS) {
      _img_set = true;
      uint8_t fm = 0;                                  // (rc17) a fleet fill (absent in files of rc16 and older)
      if (f.read(&fm, 1) == 1 && (fm & 0xF0) == 0xF0) { _fill = true; _fill_mode = fm & 0x0F; }
      else if (fm == 0xE2) _fleet_mode = FLEET_REPLACE;   // (rc18) our fleet pull
    }
    else { _nholders = 0; uint8_t fm = 0; if (mk == 0xA6 && f.read(&fm, 1) == 1 && fm == 0xE2) _fleet_mode = FLEET_REPLACE; }
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
  // (rc21, C58) an RTC chip kept the real time across the apply: nothing to do (rc20 added 30 s and WROTE the chip ahead on every apply)
  if (t == 0 || (_flood_host && _flood_host->clockSource())) return;
  t += CLOCK_APPLY_S + millis() / 1000;                         // the shortest apply + this boot so far: behind rather than ahead (C57 corrects)
  if (t > _mesh->getRTCClock()->getCurrentTime()) _mesh->getRTCClock()->setCurrentTime(t);
}

void FwOtaTransfer::fail(const char* why) {
  strncpy(_last_err, why, sizeof(_last_err) - 1);
  if (_paused) setPaused(false, 0);                      // (v1.17.28, C8)
  _phase = PH_FAILED;
  _resuming = false;
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

// (rc10, C26) a share is agreed once per (re)start: one granted while the server was busy (the 0.10 floor) stayed so after the other
// pullers had gone (2026-10-05: 33 h instead of 11 h). The puller re-offers under a new session (chunks kept, the old share freed - the
// existing resume path, so older servers take part) while it holds less than it asked: after 15 min, doubling to 2 h while nothing grows.
static uint32_t reagreeNextMs(uint32_t prev_ms, uint8_t before_pct, uint8_t now_pct) {
  if (!prev_ms || now_pct > before_pct) return REAGREE_FIRST_MS;
  return prev_ms * 2 > REAGREE_MAX_MS ? REAGREE_MAX_MS : prev_ms * 2;
}

// (swarm v1, found on hardware 2026-10-06) the rate a stalled pull had, frozen at the stall: measured "since the download started" it kept
// falling while the pull waited, so the switch time grew faster than the wait and the pull never moved on (board 3: 396 s at 26 chunks)
static uint32_t stallRate(uint32_t got, uint32_t el_s) { return el_s >= 60 && got >= 3 ? (uint32_t)((uint64_t)got * 3600 / el_s) : 0; }

// (rc11, C29, owner) FAIR SHARE: the server's duty split evenly over its active pulls that follow PACE, each capped at what it asked (what
// one does not need goes to the others); pulls of older firmware (fixed) keep what they were given. Never below DUTY_MIN_PCT each.
// Owner's case: duty 1 serving one pull at 1, a second asks 1 -> both 0.5 (the running one from its next window); one leaves -> 1 again.
static void dutyFair(uint8_t n, const uint8_t* ask, const bool* fixed, const uint8_t* fixed_pct, uint8_t ceiling, uint8_t* out) {
  int rem = ceiling; uint8_t open = 0; bool done[8] = {false};
  for (uint8_t i = 0; i < n && i < 8; i++) {
    if (fixed[i]) { out[i] = fixed_pct[i]; rem -= fixed_pct[i]; done[i] = true; }
    else { out[i] = 0; open++; }
  }
  if (rem < 0) rem = 0;
  while (open) {
    const int each = rem / open; bool capped = false;
    for (uint8_t i = 0; i < n && i < 8; i++) {
      if (done[i]) continue;
      const uint8_t a = (ask[i] && ask[i] <= ceiling) ? ask[i] : ceiling;
      if (a <= each) { out[i] = a; rem -= a; done[i] = true; open--; capped = true; }
    }
    if (capped) continue;
    for (uint8_t i = 0; i < n && i < 8; i++) if (!done[i]) out[i] = (uint8_t)each;
    break;
  }
  for (uint8_t i = 0; i < n && i < 8; i++) if (!fixed[i] && out[i] < DUTY_MIN_PCT) out[i] = DUTY_MIN_PCT;
}
// (rc11, C29) a pull keeps its share while it requests within 3x its own measured interval (1 min .. 15 min) - a slow pull through
// relays asks only every few minutes and was dropped after 60 s (2026-10-05: twin 1.30 > 1.00)
static uint32_t shareHoldMs(uint32_t ivl_ms) { const uint32_t h = ivl_ms * 3; return h < DUTY_ACTIVE_MS ? DUTY_ACTIVE_MS : (h > 900000 ? 900000 : h); }
// (rc11, C29) the pace of one window: a PACE-following pull at what its request says it expects, or faster when its share was raised
// (a lowered share takes effect from its next window, after the PACE); an older pull at what it was given
static uint8_t paceFor(bool capable, uint8_t target, uint8_t reported, uint8_t given) {
  if (!capable) return given;
  return reported > target ? reported : target;
}

// (rc10, C22) server status: a puller is "active" for 10 min after its last request; "2h05m" style ages
static bool serveActive(uint32_t now, uint32_t last_ms) { return last_ms && now - last_ms < SERVE_ACTIVE_MS; }
static void agoText(uint32_t secs, char* t, size_t n) {
  if (secs < 120) snprintf(t, n, "%lu s", (unsigned long)secs);
  else if (secs < 7200) snprintf(t, n, "%lu min", (unsigned long)(secs / 60));
  else if (secs < 172800) snprintf(t, n, "%luh%02lum", (unsigned long)(secs / 3600), (unsigned long)((secs / 60) % 60));
  else snprintf(t, n, "%lu d", (unsigned long)(secs / 86400));
}

// The server went quiet / the link dropped mid-pull: do NOT give up. Keep the partial slot and the state, and go back to
// asking for an OFFER with exponential backoff; if the server comes back with the same image the pull continues from
// the first missing chunk.
void FwOtaTransfer::enterWait(const char* why) {
  if (_phase == PH_DOWNLOADING)                         // (swarm v1) the rate up to the stall decides when a pull by image moves on
    _stall_cph = stallRate(_cm.have - _rate_have0, ((_last_chunk_ms ? _last_chunk_ms : millis()) - _rate_t0_ms) / 1000);   // (C34) up to the LAST chunk
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
  _saved_man = _man;
  _win_count = 0;
  _prev_session = _session;                             // (v1.17.25) the resume runs under a NEW session, announced in its offers (the duty is agreed
  _session = newSess(0xC3C3 ^ _cm.have); _sess_set = false;   // for it; the old one's share is freed) - before, it changed only at the answer
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
  _session = newSess(0xA5A5); _sess_set = false;
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
void FwOtaTransfer::begin(mesh::Mesh* mesh, FILESYSTEM* fs, const char* node_name, const uint8_t* pub_key, const uint8_t* path_hash_mode) {
  _mesh = mesh; _fs = fs;
  _name_src = node_name; _pub = pub_key; _hash_mode = path_hash_mode;
  _move_to = -1; _fleet_mode = FLEET_FILL_EMPTY; _rt_cur = -1; _rt_recent[0] = _rt_recent[1] = -1;   // (rc19) the non-zero starting values
  _disc_fail_mark = 0xFFFF; _win_target = XFER_WIN_INIT; _crc_bad_idx = -1;                           //   (the rest is zero)
  memset(_peers, 0, sizeof(_peers));
  _noapply = _fs && _fs->exists(NOAPPLY_FILE);
  _wdt_resets = 0;                                         // (rc8, C23) how often the watchdog had to restart this node
  if (_fs && _fs->exists(WDT_FILE)) { File f = _fs->open(WDT_FILE); if (f) { f.read((uint8_t*)&_wdt_resets, 2); f.close(); } }
  const bool planned = _fs && _fs->exists(WDD_FILE);       // (rc10, C27) we let another firmware's watchdog run out on purpose
  _wdt_planned = planned && g_wdt.lastResetWasWatchdog();
  if (g_wdt.lastResetWasWatchdog() && _fs && !_wdt_planned) {   //   one small write per watchdog reset only
    if (_wdt_resets < 65535) _wdt_resets++;
    _fs->remove(WDT_FILE); File f = _fs->open(WDT_FILE, FILE_O_WRITE); if (f) { f.write((const uint8_t*)&_wdt_resets, 2); f.close(); }
  }
  if (g_wdt.draining()) {                                  // mark it once (the drain may span several soft resets)
    if (_fs && !planned) { File f = _fs->open(WDD_FILE, FILE_O_WRITE); if (f) { f.write((const uint8_t*)"1", 1); f.close(); } }
  } else if (planned) _fs->remove(WDD_FILE);              // done (or a power cycle cleared the old watchdog first)
  _serve_disabled = _fs && _fs->exists(NOSERVE_FILE);    // auto-serve is the default; only an explicit "fw serve off" disables it
  _minbatt_mv = 0;                                         // (swarm v1) 'fw minbatt'
  if (_fs && _fs->exists(MINBATT_FILE)) { File f = _fs->open(MINBATT_FILE); if (f) { f.read((uint8_t*)&_minbatt_mv, 2); f.close(); } }
  if (_fs && _fs->exists(ANN_FILE)) _fs->remove(ANN_FILE);   // (rc15) merged into the boot advert
  _ann_pending = true; _ann_at_ms = 30000 + ((uint32_t)_pub[0] * 256u + _pub[1]) % 90000u;   // (rc15) boot advert, zero-hop, 30-120 s after start
  _serving = false; _next_autoserve_ms = 4000; _probe_valid = false;
  _resuming = false; _erased = false;
  _duty_pct = DUTY_DEFAULT_PCT;                            // (v1.17.25) the node's LoRa-OTA duty ("fw duty"), default 0.5
  if (_fs && _fs->exists(DUTY_FILE)) { File f = _fs->open(DUTY_FILE); uint8_t d = 0; if (f) { if (f.read(&d, 1) == 1 && d >= DUTY_MIN_PCT && d <= 100) _duty_pct = d; f.close(); } }
  _circle_set = false;                                     // (v1.17.28) the owner's circle: without it LoRa-OTA is hidden (serves/answers/pulls nothing)
  if (_fs && _fs->exists(CIRCLE_FILE)) { File f = _fs->open(CIRCLE_FILE); if (f) { _circle_set = f.read(_circle, 32) == 32; f.close(); } }
  circleKeys();                                            // discovery channel, list key, offer key - all from C
  _next_disc_ms = millis() + DISC_FIRST_MS;
  _clk_restore = true;                                     // (rc21, C58) in the first tick: it needs the FloodHost (RTC or not)
  slotFromLota();                                          // (v1.17.28) a .lota just uploaded over BLE (`fw pull-ble`) becomes the slot
  if (loadState() && stateUsable()) {
    // a pull was in progress when we rebooted (or lost power): pick it up again once a server answers
    _saved_man = _man;
    bool fresh = (_man.n_chunks == 0);                       // no manifest yet: nothing to resume, only the request
    _resuming = !fresh;
    _cm.reset(_man.n_chunks);
    if (!fresh && _erased)                                   // which chunks made it into flash before the reboot / power cut
      _cm.rebuild([&](uint16_t i) { uint32_t off = (uint32_t)i * XFER_CHUNK; return slotPayloadPresent(_man.dst_off + off, umin(XFER_CHUNK, _man.payload_len - off)); });
    _phase = PH_OFFERING;
    if (!fresh && _erased && _cm.complete()) { _phase = PH_VERIFYING; _resuming = false; }   // every chunk made it before the reboot: verify (and apply) without a server
    _prev_session = _session;                                  // (v1.17.25) the one before the reboot (from the state file): its share is freed
    _session = newSess(0x5A5A); _sess_set = false;              // fresh session id for this boot
    _seq = 0; _retries = 0; _attempts = 0; _backoff_ms = 0; _last_req_ms = 0; _retry_timeout_ms = 0;
    _wait_started_ms = millis();
    strncpy(_last_err, fresh ? "request restored after reboot" : "resuming after reboot", sizeof(_last_err) - 1);
    if (_fill && !fresh && _erased && _phase == PH_OFFERING) {   // (rc17) a fleet fill goes back to collecting: the stream may still run
      useImgKeys(_man.sha256);                               //   (no session needed until the path)
      _phase = PH_DOWNLOADING; _resuming = false; _listening = true; _stream_ms = millis() | 1; _stream_gap_ms = 0; _gap_at_ms = 0;
      _gap_phase = false; _rate_t0_ms = millis(); _rate_have0 = _cm.have; _next_disc_ms = millis() + DISC_DOWNLOAD_MAX_MS;
      _fann_heard_ms = _fill_mode == FLEET_REPLACE ? millis() | 1 : 0; _fann_have = 0; _img_heard_ms = millis() | 1;   // a fleet fill: as if just
      //   announced (gaps after 3 min without its stream); (rc19, C42) a plain pull's listener pauses unless its puller is heard again
      strcpy(_last_err, "fleet fill resumed after reboot");
    }
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
              && f.read(&bl, 1) == 1 && (_res_len == 2 || _res_len == 3);   // (rc7) files of rc1-rc6 carry 16 more bytes: ignored
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
  if (crc32_flash(dstAddr(0), len) != _man.payload_crc32) { fail("bootloader changed before the copy: not applied"); return; }
  clearState();                                          // from here the copy is the MBR's business; a reboot must not start another pull/verify/apply
  saveClock();
  Serial.println("fw_ota: bootloader verified - copying it (MBR), the node restarts"); Serial.flush();
  uint8_t sd = 0; sd_softdevice_is_enabled(&sd);
  if (sd) sd_softdevice_disable();
  g_wdt.feedAlways();                                    // (rc10, C27) a full period for the copy, also while an old timeout runs out
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
  char app[48], bl[32];
  if (_serving && imageStillServed()) snprintf(app, sizeof(app), "app %s", _serve_man.name);
  else if (slotInUse()) strcpy(app, "no app (the slot is being pulled into)");
  else {
    SlotHeader h; const bool v = readSlotHeader(h) && slotValid();
    // (rc16, 5.3) right after a boot / a new slot image the install check has not run yet: say so (was "not installed here")
    if (v && (!_probe_valid || probeKey(h) != _probe_key)) strcpy(app, "app (checking the slot)");
    else strcpy(app, !v ? "no app (slot empty)" : "no app (the bootloader refused it here)");   // (rc17) the only valid image not served
  }
  if (_bl_serving) snprintf(bl, sizeof(bl), "bootloader rev %lu", (unsigned long)_bl_man.fw_version);
  else strcpy(bl, _bl_probed ? "no bootloader" : "bootloader (not checked yet)");   // (rc16) checked when no pull runs
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
  // (rc17, owner 2026-10-08) a VALID slot image is served whether it is installed here or not (its SHA-256 below is what pullers check,
  // chunk by chunk and as a whole) - a fleet-filled node serves before its 'fw apply'. Not served: an image the bootloader refused here.
  CtrlBlock cb;
  if (!force && readCtrl(cb) && cb.apply_state == APPLY_FAILED && !installedImage(h) && !bleFilled(h)) {
    stopServing();
    strcpy(reply, "OK - serving the bootloader only: the bootloader refused the slot image here ('fw erase', then a new pull)");
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

#ifdef FW_OTA_DEV
// dev form: '*' or a served name
bool FwOtaTransfer::cmdPull(const char* name, char* reply) {
  if (!_circle_set) { strcpy(reply, "ERR - set 'fw passphrase <phrase>' first (the same on every node of your circle)"); return true; }
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - pull already in progress ('fw pull-abort' to cancel)"); return true; }
  startPullAt(name, reply, false);
  return true;
}
#endif

void FwOtaTransfer::startPullAt(const char* name, char* reply, bool bl) {
  dropPausedFill();
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - pull already in progress ('fw pull-abort' to cancel)"); return; }
  if (_staging) { strcpy(reply, "ERR - stage in progress"); return; }
  _apply_armed = false;
  _resuming = false; _verify_retries = 0; _cm.reset(0); _erased = false;
  if (!bl) stopServing();   // we are about to rewrite the slot (a bootloader goes to its tail and leaves the app copy alone: decided when the manifest arrives)
  memset(&_man, 0, sizeof(_man));
  strncpy(_man.name, name && *name ? name : "*", sizeof(_man.name) - 1);
  _man.kind = bl ? KIND_BOOTLOADER : KIND_APP;
  _session   = newSess(0xA5A5);   _sess_set = false;
  _seq = 0; _retries = 0; _last_req_ms = 0; _win_start = 0;
  _attempts = 0; _backoff_ms = 0; _retry_timeout_ms = 0; _wait_started_ms = millis();
  _mac_fail = 0; _next_disc_ms = millis() + DISC_FIRST_MS; _disc_interval_ms = 0;
  _pull_duty_pct = _next_pull_duty; _next_pull_duty = 0; _agreed_pct = 0;   // "fw pull ... duty x" (0 = the node's duty); agreed at the offer
  _reagree_at_ms = 0; _reagree_ivl_ms = 0; _reagree_from = 0;   // (rc10, C26)
  _fair_server = false;                                  // (rc11)
  _holders_tried = 0; _switches = 0; _stall_cph = 0;     // (swarm v1) the image + holders are set by cmdPullPeer before this
  _last_chunk_ms = 0; _last_switch_ms = 0; _move_to = -1; _last_move_ms = 0; _move_check_ms = 0;   // (rc15)
  _mv_eval_ms = 0; _mv_bar_until = 0;                   // (rc20, C48) (_sw_*: the last holder change stays for 'fw route')
  _stream_ms = 0; _stream_gap_ms = 0; _gap_at_ms = 0; _listening = false;
  _prev_session = 0; _typed_failed = false; _last_ans_ms = 0;
  _fill = false; _fill_mode = 0; _fc.set = false; _gap_phase = false; _fann_next_ms = 0; _fann_heard_ms = 0; _esc_pick_ms = 0;   // (rc17)
  _el_phase = 0; _el_rounds = 0; _el_puller = false; _e_wait = false;                                                            // (study)
  _strm_top = 0; _cxl_n = 0;                                                                                                       // (rc19)
  _fleet_mode = _next_fleet ? FLEET_REPLACE : FLEET_FILL_EMPTY; _next_fleet = false;
  const uint8_t ph = _next_pause_h; _next_pause_h = 0;
  _phase = PH_OFFERING;
  _last_err[0] = 0; _res_msg[0] = 0;
  routeReset();
  saveState();   // the request itself survives a reboot / power cut: it is retried until a server answers
  if (ph) {                                              // (rc17, C37) set up and paused BEFORE its first offer: nothing is sent until then
    setPaused(true, ph);
    snprintf(reply, 160, "OK - pull%s set up and paused: the first offer goes out in %u h (or 'fw pull-resume')",
             _fleet_mode == FLEET_REPLACE ? " (fleet)" : "", (unsigned)ph);
    return;
  }
  // (rc19, C45) the reply tells what happens when it is verified: with 'fw autoapply off' it waits for 'fw apply' (rc18 said "applies itself")
  const char* fin = _noapply ? "auto-apply is off: 'fw apply' after it completes" : "applies itself when verified";
  if (bl) strcpy(reply, "OK - fetching the bootloader; it runs unattended, and once verified the node copies it and restarts ('fw pull-status')");
  else if (_fleet_mode == FLEET_REPLACE) snprintf(reply, 160, "OK - pulling (fleet: nodes that hear the stream replace older slot images, never install); %s", fin);
  else snprintf(reply, 160, "OK - pulling; it runs unattended, %s ('fw pull-status')", fin);
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
  if (_fill_paused && _phase == PH_DOWNLOADING) {         // (rc19, C42)
    snprintf(reply, 160, "PAUSED filling 'fw-%lu' (no install): %lu/%lu chunks (%lu%%) - %s; 'fw pull-resume' finishes it", (unsigned long)_man.fw_version,
             (unsigned long)_cm.have, (unsigned long)_man.n_chunks, (unsigned long)(_man.n_chunks ? 100UL * _cm.have / _man.n_chunks : 0), _last_err);
    return true;
  }
  if (_phase == PH_OFFERING && _resuming) {
    snprintf(reply, 160, "WAITING for server '%s' : %lu/%lu chunks kept, attempt %u, waited %lus, hops %u/%u, %s%s%s",
             _man.name, (unsigned long)_cm.have, (unsigned long)_man.n_chunks, _attempts,
             (unsigned long)((millis() - _wait_started_ms) / 1000), (unsigned)rtCount(_route_len), (unsigned)rtCount(_ret_set ? _ret_len : _route_len),
             lastAnsText().c, _last_err[0] ? " - " : "", _last_err);   // (rc11, reply test) no trailing space without a reason
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
        const uint32_t cpm10 = (uint32_t)((uint64_t)got * 600 / el_s);   // (rc7, C16) chunks per minute x10: LoRa speeds read 0.0 in c/s
        if (eta_s >= 3600) snprintf(eta, sizeof(eta), " %lu.%luc/m ETA %luh%02lum", (unsigned long)(cpm10 / 10), (unsigned long)(cpm10 % 10), (unsigned long)(eta_s / 3600), (unsigned long)((eta_s / 60) % 60));
        else               snprintf(eta, sizeof(eta), " %lu.%luc/m ETA %lum%02lus", (unsigned long)(cpm10 / 10), (unsigned long)(cpm10 % 10), (unsigned long)(eta_s / 60), (unsigned long)(eta_s % 60));
      } else strcpy(eta, " ETA measuring");
    }
    const uint8_t dp = _agreed_pct ? _agreed_pct : askedPct();
    // (rc17, C36, owner) the first words say what the node does: a listener "saving shared stream" (no duty: it holds no share), a fleet
    // fill "filling" (never installed here), the requester "downloading" with its duty; counters only when they are not zero
    const bool lis = _phase == PH_DOWNLOADING && (_listening || _gap_phase || (_fill && !_sess_set));
    char what[48], nm[20];
    // (rc21, C56) a listener follows whichever stream carries the image (the server may have changed since it first targeted one): it
    // names the image, not the server it once asked
    if (_fill || lis) snprintf(nm, sizeof(nm), "fw-%lu", (unsigned long)_man.fw_version); else { strncpy(nm, _man.name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0; }
    if (_phase == PH_DOWNLOADING && _fill) snprintf(what, sizeof(what), "filling '%s' (%s, no install)", nm,   // (rc20, C52) mode 1 is
                                                    _fill_mode == FLEET_FILL_EMPTY ? "passive" : "fleet");   //   no 'fleet' pull
    else if (lis) snprintf(what, sizeof(what), "saving shared stream '%s'", nm);
    else snprintf(what, sizeof(what), "%s '%s'", ph, nm);
    char dw[24] = "";
    if (_phase == PH_DOWNLOADING && !lis) snprintf(dw, sizeof(dw), " duty %u.%02u win=%u", dp / 100, dp % 100, _win_target);
    char cnt[48] = ""; size_t cl = 0;
    if (_retries) cl += snprintf(cnt + cl, sizeof(cnt) - cl, " retries=%u", _retries);
    if (_crc_bad && cl < sizeof(cnt)) cl += snprintf(cnt + cl, sizeof(cnt) - cl, " crc_bad=%lu@%ld", (unsigned long)_crc_bad, (long)_crc_bad_idx);
    if (_mac_fail && cl < sizeof(cnt)) cl += snprintf(cnt + cl, sizeof(cnt) - cl, " mac_fail=%lu", (unsigned long)_mac_fail);
    int l = snprintf(reply, 160, "%s : %lu/%lu chunks (%lu%%)%s%s%s hops %u/%u",
             what, (unsigned long)_cm.have, (unsigned long)_man.n_chunks, (unsigned long)pct, eta, dw, cnt,
             (unsigned)rtCount(_route_len), (unsigned)rtCount(_ret_set ? _ret_len : _route_len));
    // (rc16, F1) the reason only when it fits whole - a cut reason reads as a different one
    if (_last_err[0] && l > 0 && l + 1 + strlen(_last_err) < 159) snprintf(reply + l, 160 - l, " %s", _last_err);
    if (_phase == PH_COMPLETE && _serving) { size_t l = strlen(reply); snprintf(reply + l, l < 159 ? 160 - l : 0, " | serving"); }
  } else if (_serving) {
    // (rc10, C22) transfer details only while a puller is active (a request in the last 10 min), one short part per puller; else idle.
    // (rc16, sim text audit) listed: the pulls that hold a share (or are starting / done); a session that went quiet (paused, a starter
    // that yielded and now listens, a lost link) is counted as "quiet" - it holds no share, so its old duty is not shown. Parts that do
    // not fit are counted ("+N more"), never cut.
    const uint32_t now = millis(); size_t l = 0; uint8_t na = 0, nl = 0, nq = 0, nm = 0;
    auto fits = [&](size_t add, size_t keep) { return l + add + keep < 159; };
    char part[80];            // (rc16) room for the longest counters: a part is cut here, never in the reply (rc17: + repairs)
    for (uint8_t i = 0; i < DUTY_SESS_N; i++) {
      const DutySess& d = _dsess[i];
      if (d.top_bl || !d.keyed || !serveActive(now, d.last_ms)) continue;
      if (d.listen) nl++;                                    // (rc15) listeners of the shared stream: counted apart, no share
      else if (sessActive(d, now) || d.top >= _serve_man.n_chunks) na++;   // "starting" too only while it holds its share
      else nq++;
    }
    if (na || nl || nq) {
      l = snprintf(reply, 160, "serving '%s' to %u pull%s:", _serve_man.name, (unsigned)na, na == 1 ? "" : "s");
      for (uint8_t i = 0; i < DUTY_SESS_N; i++) {
        const DutySess& d = _dsess[i];
        if (d.top_bl || !d.keyed || d.listen || !serveActive(now, d.last_ms)) continue;
        if (!(sessActive(d, now) || d.top >= _serve_man.n_chunks)) continue;
        const uint32_t pct = _serve_man.n_chunks ? 100UL * d.top / _serve_man.n_chunks : 0;
        if (!d.top) { const uint8_t sp = d.target ? d.target : d.pct;   // (rc11, C28) offered, no request yet; (rc16) its share after a re-split
          snprintf(part, sizeof(part), " starting at %u.%02u", sp / 100, sp % 100); }
        else if (d.top >= _serve_man.n_chunks) strcpy(part, " done");                                  // (rc11, C28) its last window was the end
        else if (d.target && d.target != d.pct)              // (rc16) a re-split takes effect at its next window: show both
          snprintf(part, sizeof(part), " %u/%lu (%lu%%) at %u.%02u->%u.%02u", (unsigned)d.top, (unsigned long)_serve_man.n_chunks, (unsigned long)pct,
                   d.pct / 100, d.pct % 100, d.target / 100, d.target % 100);
        else snprintf(part, sizeof(part), " %u/%lu (%lu%%) at %u.%02u", (unsigned)d.top, (unsigned long)_serve_man.n_chunks, (unsigned long)pct,
                      d.pct / 100, d.pct % 100);
        if (fits(strlen(part), 30)) { strcpy(reply + l, part); l += strlen(part); } else nm++;
      }
      if (nm) l += snprintf(reply + l, 160 - l, " +%u more", (unsigned)nm);
      if (nl && fits(14, 0)) l += snprintf(reply + l, 160 - l, " +%u listening", (unsigned)nl);   // (rc15) they store the same stream
      if (nq && fits(10, 0)) l += snprintf(reply + l, 160 - l, " +%u quiet", (unsigned)nq);
    } else {
      char ago[16];
      if (_serve_last_req_ms) { agoText((now - _serve_last_req_ms) / 1000, ago, sizeof(ago)); }
      l = snprintf(reply, 160, "%s | serving '%s', no active pull (%s%s%s)", ph, _serve_man.name,   // (rc11, C28) "ago" inside the brackets
                   _serve_last_req_ms ? "last request " : "none since boot", _serve_last_req_ms ? ago : "", _serve_last_req_ms ? " ago" : "");
    }
    snprintf(part, sizeof(part), " | since boot: offers=%lu reqs=%lu heals=%lu", (unsigned long)_serve_offer_reqs,
             (unsigned long)_serve_reqs, (unsigned long)_heals_sent);
    if (stats.repairs) { size_t pl = strlen(part); snprintf(part + pl, sizeof(part) - pl, " repairs=%lu", (unsigned long)stats.repairs); }   // (rc17) zero-hop
    if (fits(strlen(part), 0)) { strcpy(reply + l, part); l += strlen(part); }
    snprintf(part, sizeof(part), " mac_fail=%lu", (unsigned long)_mac_fail);
    if (_mac_fail && fits(strlen(part), 0)) { strcpy(reply + l, part); l += strlen(part); }
  } else {
    const bool ble_msg = _phase == PH_IDLE && strncmp(_last_err, "BLE upload", 10) == 0;   // (v1.17.28) the result of the last `fw pull-ble`
    snprintf(reply, 160, "%s%s%s%s", ph, _serve_disabled ? " | auto-serve off" : "", ble_msg ? " | " : "", ble_msg ? _last_err : "");
  }
  if (_bl_offer_reqs && _phase != PH_DOWNLOADING && _phase != PH_VERIFYING) {   // (v1.17.26, A14) the bootloader offer's own counters
    size_t l = strlen(reply);
    char part[48]; snprintf(part, sizeof(part), " | bl offers=%lu up to %lu/%lu", (unsigned long)_bl_offer_reqs, (unsigned long)_bl_last_idx, (unsigned long)_bl_man.n_chunks);
    if (l + strlen(part) < 159) strcpy(reply + l, part);   // (rc16) whole or not at all
  }
  return true;
}

bool FwOtaTransfer::cmdBlInfo(char* reply) {
  if (!_bl_probed) armBlServe();
  uint32_t len = blImageLength(rdFlash, (void*)BL_ADDR);
  snprintf(reply, 160, "bootloader rev %lu, %lu B, crc %08lX, uicr %lX/%lX, %s", (unsigned long)_own_bl_rev, (unsigned long)len,
           (unsigned long)crc32_flash(BL_ADDR, len), (unsigned long)uicrBl(), (unsigned long)uicrMbr(),
           _bl_serving ? "offered to neighbours" : _serve_disabled ? "not offered (fw serve off)"
           : !_circle_set ? "not offered (no passphrase)" : "not offered (no valid identity descriptor)");   // (rc7, C16)
  return true;
}

// (rc17, C39, owner) "fw slot" in the form of "fw bl": what, size, CRC (of the app = the release .bin), install state, offer state
bool FwOtaTransfer::cmdSlot(char* reply) {
  if (_fill_paused && _phase == PH_DOWNLOADING) {         // (rc19, C42, owner) "xx% chunks of fw-##"
    snprintf(reply, 160, "slot: partial fw-%lu, %lu%% of its chunks (fill paused) - 'fw pull-resume' finishes it", (unsigned long)_man.fw_version,
             (unsigned long)(_man.n_chunks ? 100UL * _cm.have / _man.n_chunks : 0));
    return true;
  }
  // the slot is written once a server answered (its header is cleared then); a pull still asking (or paused before its first offer) left it as is
  if (_phase == PH_DOWNLOADING || _phase == PH_VERIFYING || (_phase == PH_OFFERING && (_erased || _man.n_chunks))) {
    snprintf(reply, 160, "slot: being pulled into, %s %lu/%lu chunks ('fw pull-status')", _fill ? "fleet fill" : "pull",
             (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
    return true;
  }
  SlotHeader h;
  if (!readSlotHeader(h)) { strcpy(reply, "slot empty"); return true; }
  const bool valid = slotValid();
  CtrlBlock cb; const bool cv = readCtrl(cb);
  const bool inst = valid && installedImage(h);
  const char* is = !valid ? "(crc mismatch)" : inst ? "installed here" : bleFilled(h) ? "BLE-filled, not installed"
                 : (cv && cb.apply_state == APPLY_PENDING) ? "waiting for apply" : "not installed here";
  const char* os = !valid ? "not offered" : (_serving && imageStillServed()) ? "offered to neighbours" : _serve_disabled ? "not offered (fw serve off)"
                 : !_circle_set ? "not offered (no passphrase)" : (cv && cb.apply_state == APPLY_FAILED && !inst) ? "not offered (the bootloader refused it)"
                 : "offered shortly (checking)";
  snprintf(reply, 160, "slot: %sapp fw-%lu, %lu B, crc %08lX, %s, %s", valid ? "" : "INVALID ", (unsigned long)h.fw_version,
           (unsigned long)h.raw_len, (unsigned long)h.raw_crc32, is, os);
  return true;
}

bool FwOtaTransfer::cmdAbort(char* reply) {
  uint8_t sid[4];
  // (rc19, C42, owner) our FLEET pull (not a fill) that has met its server: listeners collect its stream - they stop too (cancel, repeated)
  const bool fleet = (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) && !_fill && _fleet_mode == FLEET_REPLACE && _man.n_chunks
                     && _man.kind == KIND_APP && pullTargetId(sid);
  if ((_phase == PH_DOWNLOADING || _phase == PH_OFFERING) && _sess_set) {
    sendAbort();
    _abt_sess = _session; memcpy(_abt_key, _sess_mac, 16); memcpy(_abt_route, _route, MAX_ROUTE_BYTES); _abt_rlen = _route_len;
    _abt_n = fleet ? 3 : 1; _abt_at_ms = millis() + 120000;   // (rc20, C49) repeated: +2 min (a fleet pull: + 10 and 30 min too)
  }
  if (fleet) {
    buildFann(_cxl, _man, FLEET_REPLACE | FLEET_CANCEL, sid, (uint16_t)_cm.have, _pub);
    memcpy(_cxl_route, _route, MAX_ROUTE_BYTES); _cxl_rlen = _route_len; _cxl_n = 4; _cxl_at_ms = millis();
    memset(_spread_tag, 0, 4);
  }
  const bool was_res = _resolving;
  _phase = PH_IDLE; _resuming = false; clearState(); clearResolve(); _res_msg[0] = 0;
  _fill = false; _fc.set = false; _gap_phase = false; _listening = false; _fill_paused = false;   // (rc17)
  setPaused(false, 0);                                   // (v1.17.28, C8)
  strcpy(reply, was_res ? "OK - search for the short id stopped" : fleet ? "OK - aborted; the fleet stops too (the cancel is repeated for 30 min)" : "OK - aborted");
  return true;
}

// (v1.17.28, C8) pause / resume a pull. Paused = a stall the owner chose: the chunks and the pull state are kept, the server's duty share
// is freed (ABORT for our session), and the node sends nothing until 'fw pull-resume' or the timer. One flash write each way.
void FwOtaTransfer::dropPausedFill() {
  if (!_fill_paused) return;
  _fill_paused = false; _fill = false; _phase = PH_IDLE; clearState();
}
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
  const bool again = _paused;                            // (rc20, C55) a retry whose first reply was lost (or a new timer): no error
  if (_phase == PH_DOWNLOADING) {                       // free the server's share now; the pull resumes later like after a stall
    sendAbort();
    enterWait("paused");
  }
  saveState();
  setPaused(true, (uint8_t)h);
  if (again && h) snprintf(reply, 160, "OK - already paused at %lu/%lu chunks; now resumes by itself in %d h", (unsigned long)_cm.have, (unsigned long)_man.n_chunks, h);
  else if (again) snprintf(reply, 160, "OK - already paused at %lu/%lu chunks, until 'fw pull-resume'", (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
  else if (h) snprintf(reply, 160, "OK - paused at %lu/%lu chunks: nothing is sent; resumes by itself in %d h (or 'fw pull-resume')", (unsigned long)_cm.have, (unsigned long)_man.n_chunks, h);
  else   snprintf(reply, 160, "OK - paused at %lu/%lu chunks: nothing is sent until 'fw pull-resume' ('fw pull-abort' drops it)", (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
  return true;
}
bool FwOtaTransfer::cmdResume(char* reply) {
  if (_fill_paused && _phase == PH_DOWNLOADING) {       // (rc19, C42, owner) a paused fill: finish it from the nearest holder (a pull by image,
    _fill_paused = false; gapEscalate(millis());          //   never installed - 'fw apply' as for any fill)
    snprintf(reply, 160, "OK - finishing the fill at %lu/%lu chunks from the nearest holder (no install)", (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
    return true;
  }
  if (!_paused) {                                        // (rc20, C55) a retry whose first reply was lost: say what runs, not an error
    if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING)
      snprintf(reply, 160, "OK - not paused: the pull runs at %lu/%lu chunks ('fw pull-status')", (unsigned long)_cm.have, (unsigned long)_man.n_chunks);
    else strcpy(reply, "ERR - no paused pull");
    return true;
  }
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
#ifdef FW_OTA_DEV
  if (_wdt_hang_at && (int32_t)(now - _wdt_hang_at) >= 0) { for (;;) { } }   // `fw wdt hang`: the loop stops, the watchdog must restart the node
#endif

  // auto-serve: any node holding a valid image in its slot offers it to its neighbours - after a boot, right after its own pull
  // completed, after a local stage. Not while pulling/staging (the slot is being rewritten). Cheap: header probe every 15 s.
  if (!_serving && !_serve_disabled && !_staging && (int32_t)(now - _next_autoserve_ms) >= 0
      && (_phase == PH_IDLE || _phase == PH_COMPLETE || _phase == PH_FAILED)) {
    _next_autoserve_ms = now + AUTOSERVE_POLL_MS;
    SlotHeader ah;
    if (readSlotHeader(ah) && ah.magic == SLOT_MAGIC) {
      // probe each distinct slot image once per boot (the CRC over slot + app is not free)
      const uint32_t key = probeKey(ah);
      if (!_probe_valid || key != _probe_key) {
        _probe_valid = true; _probe_key = key;
        char tmp[160]; cmdServe(tmp, false, false);   // arms serving only if the slot is valid AND installed
        _probe_ok = _serving;
      } else if (_probe_ok) { char tmp[160]; cmdServe(tmp, false, false); }
    }
  }

  if (!_bl_probed && !_staging && now > 6000 && (_phase == PH_IDLE || _phase == PH_COMPLETE || _phase == PH_FAILED)) armBlServe();
  if (_clk_restore && _flood_host) { _clk_restore = false; restoreClock(); }   // (rc21, C58)
  timeTick(now);                                                               // (rc21, C57)
  if (_reset_at_ms && (int32_t)(now - _reset_at_ms) >= 0) {   // (v1.17.28) `fw pull-ble` / `fw apply`: the reply has gone out, restart now
    saveClock();
    g_wdt.feedAlways();                                    // (rc8) a full watchdog period for the bootloader (it reloads it on every event)
    uint8_t sd = 0; sd_softdevice_is_enabled(&sd);
    if (sd) { sd_power_gpregret_clr(0, 0xffffffff); if (_reset_magic) sd_power_gpregret_set(0, _reset_magic); }
    else NRF_POWER->GPREGRET = _reset_magic;
    Serial.flush(); delay(100);
    NVIC_SystemReset();
  }
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
  repTick(now);                                           // (rc17) gap repairs for the neighbours, one at a time in quiet moments
  trickleTick(now);                                       // (study E) summaries to the neighbours
  cxlTick(now);                                           // (rc19, C42) the repeats of a fleet cancel
  abtTick(now);                                           // (rc20, C49) the repeats of the session ABORT
  if (_fc.set && since(now, _fc.ms) > FILL_ARM_MS) _fc.set = false;   // (rc17) an announce whose stream never came here
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

  if (_ann_pending && (_serving || _bl_serving) && (int32_t)(now - _ann_at_ms) >= 0) {   // (rc15) the boot advert: zero-hop, once
    _ann_pending = false; sendAnnounce(true);
  }
  advertTick(now);                                          // (rc15) idle holder + demand
  pendAnsTick(now);                                         // (rc15) delayed / suppressed discovery answers
  if (_disc_flood_ms && (int32_t)(now - _disc_flood_ms) >= 0) {   // (rc15) the search's flood step (nobody near answered)
    _disc_flood_ms = 0; sendDiscovery(_disc_flood_target);
  }
  moveEval(now);                                          // (rc20, C48) a move that turned out slower is taken back
  if (_move_to >= 0 && (int32_t)(now - _move_at_ms) >= 0) moveCheck(now, false);   // (rc15) the jitter is over: re-check, then move
  else if (_phase == PH_DOWNLOADING && _img_set && _sess_set && !_gap_phase && (int32_t)(now - _move_check_ms) >= 0) { _move_check_ms = now + MOVE_CHECK_MS; moveCheck(now, true); }
  // (swarm v1, phase 2) the holder went silent (a stalled pull, a new pull whose holder never answered, or a holder we switched to that
  // is silent too): the next holder after switchAfterMs() - every switch restarts the wait (switchSource)
  { const uint32_t quiet_since = umax(umax(_last_chunk_ms, _last_switch_ms), _last_chunk_ms ? 0 : _wait_started_ms);   // (C34) silence from the
    if (_phase == PH_OFFERING && !_paused && _img_set && _nholders > 1                                                     // last chunk / switch
        && since(now, quiet_since) > switchAfterMs() && switchToNextHolder(now)) return; }
  if (_esc_pick_ms && (int32_t)(now - _esc_pick_ms) >= 0) {   // (rc17) a collector going along the path: the best (nearest) holder now
    _esc_pick_ms = 0; uint8_t cur[4];
    if (_phase == PH_OFFERING && _resuming && _img_set && pullTargetId(cur)) {
      const int bi = bestHolder(_img_kind, _img_ver, _img_digest, nullptr, 0);
      if (bi >= 0 && memcmp(_peers[bi].id, cur, 4) != 0) { switchToHolder(bi, "gaps: nearest holder"); return; }
    }
  }
  if (_paused) {                                            // (v1.17.28, C8) paused: no offer, no request, no discovery - nothing at all
    if (_pause_until_ms && (int32_t)(now - _pause_until_ms) >= 0) resumePull("timed pause over: resumed");
    else return;
  }
  if (g_tune.batt_rest && powerState() == 3 && (_phase == PH_OFFERING || (_phase == PH_DOWNLOADING && _erased && !_listening))) {
    if (!_resting) { _resting = true; stats.rests++; }   // (study, owner) CRITICAL: no requests, claims or summaries - the battery recovers
    _last_req_ms = now; _req_pending = false; return;     //   first (stored chunks heard meanwhile are kept)
  }
  _resting = false;
  uint8_t did[4];
  const bool asks = _phase == PH_OFFERING || (_phase == PH_DOWNLOADING && !_listening && !_gap_phase && _sess_set);   // (rc17) a collector: no search
  if (asks && (int32_t)(now - _next_disc_ms) >= 0
      && pullTargetId(did) && !floodBudget(now)) _next_disc_ms = now + 60000;   // (v1.17.27) flood budget used up: try again in a minute
  else if (asks && (int32_t)(now - _next_disc_ms) >= 0) {
    uint8_t id[4];
    if (pullTargetId(id)) {                               // ask the mesh for a (new) path to exactly this server
      char t[20];
      if (_img_set) {                                     // (swarm v1) a pull by image asks for the IMAGE: every holder answers (holder list)
        t[0] = '='; for (uint8_t k = 0; k < 8; k++) snprintf(t + 1 + 2 * k, 3, "%02X", _img_digest[k]);
      } else snprintf(t, sizeof(t), "@%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
      _disc_sess = newSess(0x3D3D);
      if (_img_set) {                                     // (rc15) neighbours first: zero-hop, direct answers; the flood 8 s later if none came
        sendDiscovery(t, true); strncpy(_disc_flood_target, t, sizeof(_disc_flood_target) - 1); _disc_flood_ms = now + DISC_FLOOD_AFTER_MS + _mesh->getRNG()->nextInt(0, 3000);
      } else sendDiscovery(t);
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
      b[n++] = (uint8_t)(0x80 | askedPct());               // (rc15) bit 7: "image chunks OK" (an rc14 server reads > 100 = its own duty)
      { const uint8_t* tg = _img_set ? _img_digest : (_man.n_chunks ? _man.sha256 : nullptr);   // (rc15) a stream of our image is on the air:
        const bool lis = tg && _heard_tag_ms && since(now, _heard_tag_ms) < STREAM_T_MIN_MS * 2 && memcmp(_heard_tag, tg, 4) == 0;   // listen
        const uint32_t ps = lis ? 0xFFFFFFFFu : (!_prev_session && !_fill && _fleet_mode == FLEET_REPLACE) ? PREV_FLEET : _prev_session;   //   only,
        //   take no share (replaced session FFFFFFFF = "listener"); (rc18) FFFFFFFE = a fresh FLEET pull (the source may spread the image)
        memcpy(&b[n], &ps, 4); n += 4; }                   // the session this one replaces (0 = none): the server frees its share
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
    if (_cm.complete()) { sendDone(); _phase = PH_VERIFYING; return; }
    if (listening(now)) {                               // (rc15) a stream of our image for another puller: store, do not ask
      if (!_listening) { _listening = true; _move_to = -1; _last_err[0] = 0; _el_phase = 0; _e_wait = false; _fill_paused = false; }   // (rc16) the status
      _last_req_ms = now; _retries = 0; _req_pending = false; _reagree_at_ms = 0;                                         //   says "listening"; (rc19) a paused
      return;                                                                                                              //   fill continues
    }
    if (_fill_paused) return;                           // (rc19, C42) a paused fill sends nothing (it stores what it hears)
    if (_listening && streamStillPulled(now)) {          // (rc17) only a pause: its requester still pulls - keep listening (checked again
      _stream_ms = now | 1; _gap_at_ms = 0; return;      //   after the next stream time)
    }
    if (_listening) {                                   // the stream ended: our missing chunks now
      _listening = false; _retries = 0;
      // (rc17, owner 9.3) local first: the gaps are asked from the neighbours (zero-hop, the best link repairs, everyone near stores it); what
      // nobody near holds was lost upstream -> along the path. A mode-1 fill missing more than 20 % keeps its chunks and waits for the next
      // stream instead (a routine pull never turns into hidden full pulls)
      const uint32_t miss = _cm.n - _cm.have;
      if (_fill && _fill_mode == FLEET_FILL_EMPTY && !_sess_set && (miss * 5 > _cm.n || !reqHeard(now))) {
        pauseFill("passive fill paused");               // (rc19, C42) a plain pull's listener: no gaps when more than 20 % is missing or
                                                          //   its puller went quiet - the next stream of the image continues it
      } else if (g_tune.algo == ALGO_C && _fill && !_sess_set) {
        enterElect(now);                                  // (study C) collectors stay quiet: the inventory decides who pulls next
      } else {
        _gap_phase = true; _gap_round = 0; _gap_dry = 0; _gap_have0 = _cm.have; _e_prog_ms = now | 1; _e_wait = false;
        _gapr_at_ms = now + (uint32_t)_mesh->getRNG()->nextInt(0, GAP_SPREAD_MS);
        strcpy(_last_err, "gaps: asking the neighbours");
      }
      return;
    }
    if (_gap_phase) { gapTick(now); return; }
    if (_el_phase) { electTick(now); return; }
    if (_fill && !_sess_set) return;                    // (rc17) a collector without a session: nothing to ask (waits for a stream)
    if (_fann_next_ms && (!_fill || _el_puller) && _img_ok && _man.kind == KIND_APP && (int32_t)(now - _fann_next_ms) >= 0) {   // (rc17) the requester announces its
      sendFleetAnn(); _fann_next_ms = now + (_fann_seq < 2 ? 30000 : FANN_IVL_MS);                   //   stream to the nodes along its path
                                                                                                      //   (the first one twice: a lost one costs 5 min)
    }
    if (_req_pending) { requestWindow(); return; }      // (rc6, C14) a request waiting for a quiet moment - not a retry
    if (_reagree_at_ms && (int32_t)(now - _reagree_at_ms) >= 0) {   // (rc10, C26) re-offer for a larger share (chunks kept)
      _reagree_from = _agreed_pct; _reagree_at_ms = 0;
      enterWait(_reagree_why ? "duty changed: asking the server again" : "asking for a larger duty share");
      _reagree_why = false;
      return;
    }
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
  if (listening(millis())) { _last_req_ms = millis(); _req_pending = false; return; }   // (rc15) a shared stream runs: no own request
  if (_fill && !_sess_set) { _last_req_ms = millis(); _req_pending = false; return; }   // (rc17) a collector has no session to ask with
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
  if (start >= _cm.n) { sendDone(); _phase = PH_VERIFYING; return; }
  uint16_t count;
  if (relays) {                                           // (v1.17.27) a fixed batch: RELAY_BATCH missing chunks (widened up to 8 slots when some
    uint16_t left = (uint16_t)(_cm.n - start);            //     in between have arrived); fewer only at the end of the image
    count = left < RELAY_BATCH ? left : RELAY_BATCH;      //     (rc20 study, A16: a batch growing to 8 on clean windows gained nothing on
    while (count < XFER_WINDOW && count < left) {         //     chains with 10 % loss per link - a clean window is rare - not built)
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
  uint8_t b[8 + 4 + 1 + MAX_ROUTE_BYTES + 1 + XFER_MAC_LEN];
  uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_CHUNK_REQ;
  memcpy(&b[n], &_session, 4); n += 4;
  memcpy(&b[n], &_seq, 2); n += 2;
  memcpy(&b[n], &start, 2); n += 2;
  b[n++] = (uint8_t)count | (_man.kind == KIND_BOOTLOADER ? 0x80 : 0);   // bit 7 = "of the bootloader offer"
  b[n++] = _cm.missingMask(start, (uint8_t)count);      // bit i set = chunk (start+i) still needed: a retry never re-asks for what arrived
  n = addReturnRoute(b, n);
  b[n++] = (uint8_t)(0x80 | (_agreed_pct ? _agreed_pct : askedPct()));   // (rc11, C29) "I follow PACE" + the duty this window is timed for
  n += XFER_MAC_LEN;                                    //   (older servers ignore the byte: they read the fields and the route only)
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
  // (rc15) image chunks: image key, any puller of the image (not counted as a MAC fail). (rc17) they, fleet announces and gap requests are
  // handled in noteRx(): every packet the radio hears, also one a relay only forwards (this hook sees only the last hop's)
  if (b[1] == MSG_CHUNK_IMG || b[1] == MSG_FLEET_ANN || b[1] == MSG_GAP_REQ) return;
  if ((b[1] == MSG_CHUNK || b[1] == MSG_PACE) && len >= 6) {   // chunks carry the session: another pull's chunks are not ours (no MAC fail)
    uint32_t cs; memcpy(&cs, &b[2], 4);                           // (rc14, C33) the same for PACE: one overheard for a neighbour's pull
    if (cs != _session || (b[1] == MSG_CHUNK && _phase != PH_DOWNLOADING)) return;   //   counted as mac_fail (twin, 2026-10-06)
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
    case MSG_OFFER_NAK: {                               // (rc7) only a NAK of THIS pull's session counts: a recorded one replayed later must not fail a new pull
      uint32_t ns; if (len < 6 + XFER_MAC_LEN) break; memcpy(&ns, &b[2], 4);
      if (ns == _session && _phase == PH_OFFERING && !_resuming) fail("server NAK");   // while waiting, keep trying
      break; }
    case MSG_CHUNK_REQ: onChunkReq(b, len); break;
    case MSG_CHUNK:      onChunk(b, len); break;
    case MSG_PACE:       onPace(b, len); break;
    case MSG_LIST_REQ: onListReq(b, len); break;
    case MSG_LIST_RSP: onListRsp(b, len, (int8_t)(pkt->getSNR() * 4)); break;
    case MSG_ABORT: {
      // only OUR session: another puller's ABORT (zero-hop broadcast) must not kill this pull
      uint32_t asess; memcpy(&asess, &b[2], 4);
      { DutySess* ad = dutySess(asess, false);
        if (ad) {                                       // (v1.17.25) server: that pull no longer holds a share
          memset(ad, 0, sizeof(*ad));
          const uint32_t na = millis(); dutyRebalance(na);   // (rc11, C29) the others get it back at once
          for (uint8_t i = 0; i < DUTY_SESS_N; i++) { DutySess& o = _dsess[i]; if (o.capable && o.target > o.rep && sessActive(o, na)) sendPace(o, o.target); }
        } }
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
  const bool img = (b[8] & 0x80) != 0;            // (rc15) the puller reads image chunks (the shared stream)
  const uint8_t asked = b[8] & 0x7F;
  uint32_t prev; memcpy(&prev, &b[9], 4);
  const bool listen = img && prev == 0xFFFFFFFFu;  // (rc15) it listens to a stream of this image: keys, no share until it requests
  const bool fleet = prev == PREV_FLEET;            // (rc18) a fresh fleet pull: no session to replace
  uint16_t ptop = 0;                              // (rc19, C47) a resume under a new session: the status keeps its position (was "starting")
  { DutySess* pd = prev && prev != sess && !listen && !fleet ? dutySess(prev, false) : nullptr; if (pd) { ptop = pd->top_bl ? 0 : pd->top; memset(pd, 0, sizeof(*pd)); } }
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
  if (powerState() == 3) return;                  // (swarm v1) CRITICAL battery: no new pulls (its list answers say why - pullers go elsewhere)
  const XferManifest& m = bl ? _bl_man : _serve_man;
  if (!bl) _serve_offer_reqs++; else _bl_offer_reqs++;   // (v1.17.26, A14) bootloader offers counted separately
  bool match = bl || (wl == 1 && want[0] == '*') || (wl > 0 && want[0] == '@') ||
               (wl > 0 && wl < 32 && strncmp(want, m.name, wl) == 0);
  if (!match) _serve_offer_nomatch++;
  else noteReq(b, true, 0, m, ppub);            // the same offer again and again: our OFFER_ACKs do not get back (heal)
  if (match && !bl && fleet) spreadStart(m.sha256);   // (rc18) the source of a fleet pull gossips the image from now on (answers may be lost)
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
  // (rc11, C29) the duty agreed for this session: its FAIR SHARE of ours (was: what the others had left over - often the 0.10 floor)
  DutySess* ds = dutySess(sess, true);
  if (!ds) return;
  if (!ds->top) ds->top = ptop;
  const uint32_t nowo = millis();
  ds->ask = (asked >= DUTY_MIN_PCT && asked <= 100) ? asked : _duty_pct; ds->last_ms = nowo ? nowo : 1; ds->next_tx_ms = 0;
  ds->img = img; ds->listen = listen;
  if (!ds->ivl_ms) ds->ivl_ms = 60000;
  dutyRebalance(nowo);
  const uint8_t ag = ds->target ? ds->target : dutyAgree(ds->ask, _duty_pct, dutyOthers(sess));
  ds->pct = ag;
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) {          // the running pulls learn their lowered share NOW: their next window is timed for it
    DutySess& o = _dsess[i];                            // (the newcomer's chunks wait for that window - serveTick / decreasePending)
    if (&o != ds && o.capable && o.target && o.rep > o.target && sessActive(o, nowo)) sendPace(o, o.target);
  }
  r[n++] = ag;
  // (v1.17.28) session keys: the same server nonce for every repeat of this session's offer (the puller keeps the first ACK it accepts)
  if (!ds->keyed || memcmp(ds->np, pnonce, 8) != 0) {
    memcpy(ds->np, pnonce, 8); _mesh->getRNG()->random(ds->ns, 8);
    ds->keyed = sessionKeys(ppub, sess, ds->np, ds->ns, ds->kmac, ds->kenc);
  }
  if (!ds->keyed) return;
  memcpy(&r[n], _pub, 32); n += 32;                // our identity ...
  memcpy(&r[n], ds->ns, 8); n += 8;                // ... and nonce
  r[n++] = (uint8_t)(0x01 | (img ? 0x02 : 0));     // (rc11) flags: bit 0 = fair share + PACE; (rc15) bit 1 = image chunks (older pullers ignore it)
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
  _fair_server = len >= kp + 32 + 8 + 1 + XFER_MAC_LEN && (b[kp + 40] & 0x01);   // (rc11) it will tell us (PACE) when our share changes
  _img_ok = len >= kp + 32 + 8 + 1 + XFER_MAC_LEN && (b[kp + 40] & 0x02);          // (rc15) it sends image chunks (the shared stream)
  _fann_next_ms = _img_ok ? (millis() | 1) : 0;          // (rc17) image chunks = others can collect them: announce the stream (tick)
  if (g_tune.req_adopt && !_img_set && !_fill && _img_ok && nm.kind == KIND_APP) {   // (study) a pull by id: from now on a pull of this IMAGE
    uint8_t tid[4];                                                                     //   (nearer holders, moves, failover)
    if (pullTargetId(tid)) { _img_set = true; _img_kind = KIND_APP; _img_ver = nm.fw_version; memcpy(_img_digest, nm.sha256, 8);
                             memcpy(_holders[0], tid, 4); _nholders = 1; _holders_tried = 0;
                             if (memcmp(_origin_tag, nm.sha256, 4) != 0) { memcpy(_origin, tid, 4); memcpy(_origin_tag, nm.sha256, 4); } }
  }
  useImgKeys(nm.sha256);                                 // (rc15) image key: any stream of THIS image
  if (_heard_tag_ms && since(millis(), _heard_tag_ms) < STREAM_T_MIN_MS && memcmp(_heard_tag, nm.sha256, 4) == 0) {
    _stream_ms = _heard_tag_ms; _gap_at_ms = 0;          // (rc15) that stream runs now: listen from the start (no own request after the erase)
  }
  if (_agreed_pct < askedPct() && !_fair_server) {      // (rc10, C26) less than asked: ask again later (backs off while it does not grow)
    _reagree_ivl_ms = _reagree_from ? reagreeNextMs(_reagree_ivl_ms, _reagree_from, _agreed_pct) : REAGREE_FIRST_MS;
    _reagree_at_ms = millis() + _reagree_ivl_ms;
  } else { _reagree_at_ms = 0; _reagree_ivl_ms = 0; }
  if (_reagree_from) snprintf(_last_err, sizeof(_last_err), "duty re-agreed %u.%02u -> %u.%02u", _reagree_from / 100, _reagree_from % 100, _agreed_pct / 100, _agreed_pct % 100);
  _reagree_from = 0;
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
      if (strncmp(_last_err, "duty re-agreed", 14) != 0)   // (rc11, C28) keep the re-agreement's result visible
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
      _man = nm; _phase = PH_COMPLETE;
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
  _cm.reset(nm.n_chunks); _erased = false; _erase_next = 0; _strm_top = 0;
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
// (rc15) image chunks only (session chunks are readable by their puller alone), and only along the SAME way (a direct chunk reaches the
// pullers on its route; another route's puller would not hear it)
bool FwOtaTransfer::recentlySent(uint16_t idx, uint32_t sess, uint32_t now, uint8_t rlen, const uint8_t* rpath) const {
  for (uint8_t i = 0; i < RECENT_N; i++)
    if (_recent[i].ms && _recent[i].idx == idx && _recent[i].sess != sess && since(now, _recent[i].ms) < RECENT_MS
        && _recent[i].rlen == rlen && memcmp(_recent[i].rpath, rpath, rtBytes(rlen)) == 0) return true;
  return false;
}
void FwOtaTransfer::noteSent(uint16_t idx, uint32_t sess, uint32_t now, uint8_t rlen, const uint8_t* rpath) {
  Recent& r = _recent[_recent_wr]; r.idx = idx; r.sess = sess; r.ms = now ? now : 1; r.rlen = rlen;
  memset(r.rpath, 0, sizeof(r.rpath)); if (rtBytes(rlen)) memcpy(r.rpath, rpath, rtBytes(rlen));
  _recent_wr = (_recent_wr + 1) % RECENT_N;
}

// (rc15) the image key of an image: HMAC(C, "lota2-img" | SHA-256) -> MAC key | encryption key
void FwOtaTransfer::imgKey(const uint8_t* sha256, uint8_t k[32]) const { hmac(_circle, 32, (const uint8_t*)"lota2-img", 9, sha256, 32, k); }
void FwOtaTransfer::useImgKeys(const uint8_t* sha256) {
  uint8_t k[32]; imgKey(sha256, k); memcpy(_ik_mac, k, 16); memcpy(_ik_enc, k + 16, 16); memset(k, 0, sizeof(k));
}
// ... of a served image (cached per kind)
const uint8_t* FwOtaTransfer::imageKeys(const XferManifest& m, uint8_t slot) {
  ImgKeys& c = _ikc[slot & 1];
  if (!c.set || memcmp(c.dg, m.sha256, 8) != 0) {
    imgKey(m.sha256, c.k); memcpy(c.dg, m.sha256, 8); c.set = true;
  }
  return c.k;
}

// ---- server: send a run of chunks ----
void FwOtaTransfer::onChunkReq(const uint8_t* b, uint32_t len) {
  bool bl = (b[10] & 0x80) != 0;                       // bit 7 of the count byte: a chunk of the bootloader offer
  if (bl ? !_bl_serving : !_serving) return;
  if (powerState() == 3) return;                       // (swarm v1) CRITICAL battery: stop serving (the puller moves to another holder)
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
  last_idx = (uint32_t)start + count;                  // (rc7, C16) the latest request's position (was the highest of any pull, ever)
  noteReq(b, false, start, m);                         // the same window again and again: our chunks do not get back (heal)
  // (v1.17.25) pace this session at its agreed duty (a session we do not know - older puller, or we rebooted - at our own duty)
  DutySess* ds = dutySess(sess, true);
  if (ds && !ds->pct) ds->pct = _duty_pct;
  if (ds && ds->listen) ds->listen = false;            // (rc15) a listener asks now (the stream ended): it holds a share from here
  const uint32_t now0 = millis();
  bool pace_msg = false;
  if (ds) {
    ds->last_ms = now0 ? now0 : 1; ds->top = (uint16_t)(start + count); ds->top_bl = bl;   // (rc10, C22) per puller: where it is
    // (rc11, C29) its interval (the share is held 3x that), whether it follows PACE (+ the duty its window is timed for), fair share
    if (ds->last_req_ms) { const uint32_t iv = now0 - ds->last_req_ms; ds->ivl_ms = ds->ivl_ms ? (ds->ivl_ms * 3 + iv) / 4 : iv; }
    ds->last_req_ms = now0 ? now0 : 1;
    const uint32_t tb = 13u + rtBytes(rlen);
    if (len >= tb + 1 + XFER_MAC_LEN && (b[tb] & 0x80)) { ds->capable = true; ds->rep = b[tb] & 0x7F; }
    ds->known = true;
    if (!ds->ask) ds->ask = _duty_pct;
    dutyRebalance(now0);
    if (ds->capable && ds->target) {
      ds->pct = paceFor(true, ds->target, ds->rep, ds->pct);
      pace_msg = ds->rep != ds->target;                // tell it (raised: from now; lowered: from its next window)
      if (decreasePending(ds)) ds->q_hold_until = now0 + 60000;   // another pull is still above its lowered share: wait for its next window
      else ds->q_hold_until = 0;
    }
  }
  const uint8_t pct = ds ? ds->pct : _duty_pct;
  if (!bl) _serve_last_req_ms = now0 ? now0 : 1;
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
    if (ds->img && recentlySent((uint16_t)idx | (bl ? 0x8000 : 0), sess, now0, rlen, rpath)) continue;   // (rc15) just sent that way: heard
    ds->q_idx[ds->q_n++] = (uint16_t)idx;
  }
  ds->q_bl = bl; ds->q_rlen = rlen; if (rtBytes(rlen)) memcpy(ds->q_rpath, rpath, rtBytes(rlen));
  memcpy(ds->q_rseq, rseq, 2); ds->q_paced = paced; ds->q_sp = paced ? sp : CHUNK_GAP_MS; ds->q_relays = relays;
  ds->q_due = now0 + t; ds->q_wait0 = 0;
  if (pace_msg) sendPace(*ds, ds->target);
}

// (rc6, C14) server: the queued chunks go out one at a time, at the agreed pace, each only when the channel has been quiet for
// YIELD_QUIET_MS (other traffic first) or after YIELD_MAX_MS of waiting. The quiet gap follows the last chunk of a window.
void FwOtaTransfer::serveTick(uint32_t now) {
  for (uint8_t k = 0; k < DUTY_SESS_N; k++) {
    DutySess& d = _dsess[k];
    if (!d.sess || d.q_i >= d.q_n || (int32_t)(now - d.q_due) < 0) continue;
    if (d.q_bl ? !_bl_serving : !_serving) { d.q_n = 0; continue; }
    if (d.q_hold_until && (int32_t)(now - d.q_hold_until) < 0 && decreasePending(&d)) continue;   // (rc11, C29) after the other pull's window
    d.q_hold_until = 0;
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
  const uint8_t* ik = d.img ? imageKeys(m, bl ? 1 : 0) : nullptr;   // (rc15) the shared stream: the image's key, its tag in place of the session
  uint32_t nonce = sess; if (ik) memcpy(&nonce, m.sha256, 4);
  r[n++] = XFER_MAGIC; r[n++] = ik ? MSG_CHUNK_IMG : MSG_CHUNK;
  memcpy(&r[n], &nonce, 4); n += 4;             // (v1.17.28) the session: the data is encrypted with its key (rc15: or the image tag)
  memcpy(&r[n], &idx, 2); n += 2;
  uint16_t wl = (uint16_t)clen;  memcpy(&r[n], &wl, 2);   n += 2;
  memcpy(&r[n], d.q_rseq, 2); n += 2;           // request seq echoed: a re-sent chunk must not be byte-identical (relays drop duplicates)
  flashRead(&r[n], bl ? BL_ADDR + off : SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off, clen); n += clen;
  uint16_t cc = crc16(&r[n - clen], clen); memcpy(&r[n], &cc, 2); n += 2;   // CRC of the plain data (checked after decryption)
  ctrXor(ik ? ik + 16 : d.kenc, nonce, idx, &r[n - 2 - clen], clen);   // AES-128-CTR, same length
  n += XFER_MAC_LEN;
  macFillKey(r, n, ik ? ik : d.kmac);
  if (ik) noteSent(rkey, sess, now, d.q_rlen, d.q_rpath);
  send(r, n, 0, d.q_rpath, d.q_rlen);
}

// (rc6, C14) every packet the node receives (MyMesh::logRx): anything that is not LoRa-OTA is "other traffic" the transfer yields to
void FwOtaTransfer::noteRx(uint8_t payload_type, const uint8_t* payload, uint16_t len, int8_t snr4) {
  if (payload_type == PAYLOAD_TYPE_RAW_CUSTOM && len && payload[0] == XFER_MAGIC) {
    noteDemand(payload, len);                                // (rc15) streams heard
    if (!_circle_set || len < 2 + XFER_MAC_LEN) return;
    if (payload[1] == MSG_CHUNK_IMG && len >= 8) {          // (rc17) any hop of a stream: collect it (fleet fill), cancel our repair of it
      uint16_t idx; memcpy(&idx, &payload[6], 2);
      repHeard(&payload[2], idx);
      if (_fc.set && memcmp(&payload[2], _fc.m.sha256, 4) == 0 && fillAllowed(_fc.m, _fc.mode)) startFill();
      onChunkImg(payload, len);
    } else if (payload[1] == MSG_FLEET_ANN) onFleetAnn(payload, len);
    else if (payload[1] == MSG_GAP_REQ) onGapReq(payload, len, snr4);
    else if (payload[1] == MSG_CLAIM) onClaim(payload, len);
    else if (payload[1] == MSG_SUMM) onSumm(payload, len);
    return;
  }
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
  dropPausedFill();                                       // (rc19) a paused fill gives way to the Bluetooth fill
  if (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) { strcpy(reply, "ERR - a pull is in progress ('fw pull-abort' first)"); return true; }
  if (_staging) { strcpy(reply, "ERR - stage in progress"); return true; }
  if (!_bl_probed) armBlServe();
  if (_own_bl_rev < 5) { snprintf(reply, 160, "ERR - needs bootloader rev 5+ (this node: rev %lu) - 'fw blupdate' first", (unsigned long)_own_bl_rev); return true; }
  _reset_magic = GPREGRET_SLOT_DFU; _reset_at_ms = millis() + 3000;
  // (rc19, C46, owner: warn only) a weak battery without USB power: board 3 restarted on DFU entry at 3.29-3.36 V (B4) - the reply says so
  if (_batt_mv && _batt_mv < BLE_WARN_MV && !(NRF_POWER->USBREGSTATUS & 1))
    snprintf(reply, 160, "OK - Bluetooth upload into the slot in 3 s (battery %u.%02u V, no USB: it may restart instead - USB power helps). "
             "DFU app -> 3401_DFU -> -slot.zip", _batt_mv / 1000, (_batt_mv % 1000) / 10);
  else strcpy(reply, "OK - Bluetooth upload into the slot in 3 s: DFU app -> 3401_DFU -> the release's -slot.zip. Off the mesh until done (5 min idle = back)");
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
    if (_dsess[i].sess != sess && sessActive(_dsess[i], now)) sum += _dsess[i].pct;
  return sum > 255 ? 255 : (uint8_t)sum;
}

bool FwOtaTransfer::sessActive(const DutySess& d, uint32_t now) const {
  return d.last_ms && !d.listen && now - d.last_ms < shareHoldMs(d.ivl_ms);   // (rc15) a listener holds no share
}

// (rc11, C29) fair share over the active pulls: a pull of older firmware that has requested (known, not following PACE) keeps its share;
// all others (new ones included) split the rest evenly, capped at their asks
void FwOtaTransfer::dutyRebalance(uint32_t now) {
  uint8_t idx[DUTY_SESS_N], ask[DUTY_SESS_N], fpct[DUTY_SESS_N], out[DUTY_SESS_N]; bool fixed[DUTY_SESS_N]; uint8_t n = 0;
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) {
    DutySess& d = _dsess[i];
    if (!d.sess || !sessActive(d, now)) { d.target = 0; continue; }
    idx[n] = i; ask[n] = d.ask ? d.ask : _duty_pct; fixed[n] = d.known && !d.capable; fpct[n] = d.pct ? d.pct : _duty_pct; n++;
  }
  if (!n) return;
  dutyFair(n, ask, fixed, fpct, _duty_pct, out);
  for (uint8_t j = 0; j < n; j++) _dsess[idx[j]].target = out[j];
}

bool FwOtaTransfer::decreasePending(const DutySess* except) const {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) {
    const DutySess& d = _dsess[i];
    if (&d == except || !d.capable || !d.target || !sessActive(d, now)) continue;
    if (d.rep > d.target) return true;
  }
  return false;
}

// (rc11, C29) server -> puller: "your share is now x" (session key; echoes the request seq, so a relay's duplicate filter does not
// drop a repeat and the puller can refuse an old one)
void FwOtaTransfer::sendPace(DutySess& d, uint8_t pct) {
  if (!d.keyed) return;
  uint8_t r[2 + 4 + 2 + 1 + XFER_MAC_LEN]; uint32_t n = 0;
  r[n++] = XFER_MAGIC; r[n++] = MSG_PACE;
  memcpy(&r[n], &d.sess, 4); n += 4;
  memcpy(&r[n], d.q_rseq, 2); n += 2;
  r[n++] = pct;
  n += XFER_MAC_LEN;
  macFillKey(r, n, d.kmac); send(r, n, 60, d.q_rpath, d.q_rlen);
}

// (rc11, C29) puller: the server changed our share; our next window is timed for it (lowered) - a raise is already being served
void FwOtaTransfer::onPace(const uint8_t* b, uint32_t len) {
  if (len < 9 + XFER_MAC_LEN || (_phase != PH_DOWNLOADING && _phase != PH_OFFERING)) return;
  uint32_t ps; memcpy(&ps, &b[2], 4); if (ps != _session) return;
  uint16_t rs; memcpy(&rs, &b[6], 2);
  if (rs != _seq && (uint16_t)(rs + 1) != _seq) return;   // only an answer to our latest request (or the one before): no replayed old PACE
  const uint8_t p = b[8];
  if (p < DUTY_MIN_PCT || p > 100 || p == _agreed_pct) return;
  strcpy(_last_err, p > _agreed_pct ? "duty raised by the server" : "duty shared by the server");   // the line shows the new duty itself
  _agreed_pct = p; _fair_server = true; _reagree_at_ms = 0;
}

// (rc11) all chunks are in: an ABORT of our session frees the server's share at once (it would otherwise wait for its hold to run out)
void FwOtaTransfer::sendDone() { sendAbort(); }

// (rc15) ABORT of our session, twice: one lost packet through a relay kept the server's share for its whole hold time (simulator K3). The
// repeat differs in byte 6 (a relay's duplicate filter would drop an identical one); the server reads the session only.
void FwOtaTransfer::sendAbort() { if (_sess_set) sendAbortOf(_session, _sess_mac, _route, _route_len); }
void FwOtaTransfer::sendAbortOf(uint32_t sess, const uint8_t* kmac, const uint8_t* route, uint8_t rlen) {
  for (uint8_t k = 0; k < 2; k++) {
    uint8_t b[16]; b[0] = XFER_MAGIC; b[1] = MSG_ABORT; memcpy(&b[2], &sess, 4); b[6] = k; b[7] = 0;
    macFillKey(b, 16, _circle_set ? kmac : nullptr); send(b, 16, k ? 2500 + _mesh->getRNG()->nextInt(0, 2000) : 100, route, rlen);
  }
}
void FwOtaTransfer::abtTick(uint32_t now) {
  if (!_abt_n || (int32_t)(now - _abt_at_ms) < 0) return;
  static const uint32_t AFTER_MS[] = { 0, 1200000, 480000 };   // the wait before the next one, by the repeats left: +2, (+10, +30) min
  _abt_n--; _abt_at_ms = now + AFTER_MS[_abt_n];
  sendAbortOf(_abt_sess, _abt_key, _abt_route, _abt_rlen);
}

uint8_t FwOtaTransfer::parseDuty(const char* s) { return dutyParse(s); }

// "fw duty" - show; "fw duty 0.3" - set (0.1 .. 1.0, persistent). As server: the most any pull of ours may use. As puller: what a pull asks for
// (unless "fw pull ... duty x"). Takes effect at the next start / restart of a pull (the pace is agreed once, at the offer).
bool FwOtaTransfer::cmdDuty(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (*arg) {
    uint8_t p = dutyParse(arg);
    if (!p) { strcpy(reply, "ERR - fw duty 0.10 .. 1.00 (share of airtime LoRa-OTA may use; 1.00 = no limit)"); return true; }
    _duty_pct = p; dutyRebalance(millis());               // (rc16, sim text audit) the new shares are known now (they take effect per window)
    if (_fs) { _fs->remove(DUTY_FILE); File f = _fs->open(DUTY_FILE, FILE_O_WRITE); if (f) { f.write(&p, 1); f.close(); } }
    // (rc14, owner) LIVE on both sides: a server re-splits its running pulls at their next window (rc11 fair share); a running pull of ours
    // (without its own 'duty x') asks its server again at once - the existing re-offer, chunks kept
    const bool live = _phase == PH_DOWNLOADING && !_pull_duty_pct;
    if (live) { _reagree_at_ms = millis() | 1; _reagree_why = true; }
    snprintf(reply, 160, "OK - duty %u.%02u saved (LoRa-OTA <= %u%% airtime): pulls we serve re-split at their next window%s", p / 100, p % 100, p,
             live ? "; our pull asks again now" : "");
    return true;
  }
  int w = snprintf(reply, 160, "duty %u.%02u (LoRa-OTA <= %u%% of the channel incl. relays, default 0.30)", _duty_pct / 100, _duty_pct % 100, _duty_pct);
  if (w > 0 && w < 160 && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING))
    w += snprintf(reply + w, 160 - w, " | pull asks %u.%02u%s, %s", askedPct() / 100, askedPct() % 100, _pull_duty_pct ? " (this pull)" : "",
                  _listening ? "listening (no share needed)" : _agreed_pct ? "agreed " : "agreed - (at the offer)");   // (rc16, F2)
  if (w > 0 && w < 160 && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING) && _agreed_pct && !_listening)
    w += snprintf(reply + w, 160 - w, "%u.%02u", _agreed_pct / 100, _agreed_pct % 100);
  const uint32_t now = millis(); uint8_t ns = 0; uint16_t sum = 0;
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) if (sessActive(_dsess[i], now)) { ns++; sum += _dsess[i].pct; }
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
  storeChunk(idx, data, clen, true);
}

// (rc15) SHARED IMAGE: a chunk of an image, encrypted + MAC'd with that image's key - from any holder, for any puller of the image. Ours
// (the answer to our latest request) or a stream for another puller: we store it either way; another's stream makes us a listener.
// MSG_CHUNK_IMG: [0]magic [1]type [2..5]image tag (SHA-256[0..3]) [6..7]idx [8..9]clen [10..11]req seq [12..]data [.]crc16 [.]mac
void FwOtaTransfer::onChunkImg(const uint8_t* b, uint32_t len) {
  if (_phase != PH_DOWNLOADING || len < 12 + 2 + XFER_MAC_LEN || memcmp(&b[2], _man.sha256, 4) != 0) return;
  uint16_t idx; memcpy(&idx, &b[6], 2);
  uint16_t clen; memcpy(&clen, &b[8], 2);
  uint16_t rs; memcpy(&rs, &b[10], 2);
  if (clen == 0 || clen > XFER_CHUNK || 12u + clen + 2u + XFER_MAC_LEN > len || idx >= _cm.n || !macOk(_ik_mac, b, len)) return;
  _img_heard_ms = millis() | 1;                          // (rc17) the stream is alive (repairs included)
  const bool own = _erased && !_listening && (uint16_t)(_seq - rs) <= 2 && idx >= _win_start && idx < _win_start + _win_count;
  // (rc17) a neighbour's gap repair (req seq 0xFxxx) is no stream for a requester: it goes on with its windows; a listener / collector waits
  const bool rep = (rs & 0xF000) == 0xF000;
  const bool rep_for_req = rep && !_listening && !_gap_phase && _sess_set && !own;
  if (!rep && idx > _strm_top) _strm_top = idx;          // (rc19, C44) how far the stream has come (repairs are not the stream)
  if (!own && !rep_for_req && !(rep && _fill_paused)) {  // (rc19) a paused fill wakes for a stream, not for a neighbour's repairs                            // a stream for another puller: live while chunks keep coming (its largest gap x 2)
    const uint32_t now = millis() | 1;                   //   (noted while our slot is still being erased too: no own request after it)
    if (_stream_ms) { const uint32_t g = since(now, _stream_ms); if (g < STREAM_T_MAX_MS) _stream_gap_ms = umax(g, _stream_gap_ms * 7 / 8); }
    _stream_ms = now; _gap_at_ms = 0;
  }
  if (!_erased) return;                                  // not yet erased: this one is missed (asked for after the stream)
  uint8_t data[XFER_CHUNK]; memcpy(data, &b[12], clen);
  uint32_t tag; memcpy(&tag, &b[2], 4);
  ctrXor(_ik_enc, tag, idx, data, clen);
  uint16_t cc; memcpy(&cc, &b[12 + clen], 2);
  if (crc16(data, clen) != cc) { _crc_bad++; _crc_bad_idx = (int32_t)idx; return; }
  storeChunk(idx, data, clen, own);
}

void FwOtaTransfer::storeChunk(uint16_t idx, const uint8_t* data, uint16_t clen, bool own) {
  if (_cm.has(idx)) return;                                         // dup
  uint32_t off = (uint32_t)idx * XFER_CHUNK;
  if (clen != umin(XFER_CHUNK, _man.payload_len - off)) return;
  if (!slotProgramAt(_man.dst_off + off, data, clen)) { fail("slot write failed"); return; }
  _cm.set(idx);
  const uint32_t now = millis();
  if (own) _last_req_ms = now;   // progress: reset the window timeout
  _last_chunk_ms = now ? now : 1;   // (rc15, C34)
  if (own && !_win_credited) { _win_credited = true; routeResult(true, now - _win_req_ms); }   // the route in use answered
  if (_cm.complete()) {                                             // (rc15) free the server's share now (was only in tick: never reached)
    if (_fann_next_ms && (!_fill || _el_puller) && _img_ok) sendFleetAnn();   // (rc17) "done": the collectors start on their gaps now
    if (g_tune.algo == ALGO_E) { _tr_inc_ms = now | 1; memcpy(_tr_tag, _man.sha256, 4); }   // (study E) tell the neighbours soon
    if ((_fill ? _fill_mode : _fleet_mode) == FLEET_REPLACE) memcpy(_spread_tag, _man.sha256, 4);   // (rc18) a fleet image: spread it too
    sendDone(); _phase = PH_VERIFYING; return;
  }
  if (!_listening && _win_count && _cm.missingMask(_win_start, (uint8_t)_win_count) == 0) {   // the window is complete (own or overheard): next
    if (_retries == 0 && _win_target < XFER_WINDOW) _win_target++;   // channel's keeping up - ask for more
    _retries = 0;
    requestWindow();
  }
}

// (rc15) listening: a stream of our image for another puller is live -> no own requests; when it ends, a random 0..T wait (another
// listener may start first - then we listen to its gap stream), then we ask for what is still missing
uint32_t FwOtaTransfer::streamT() const {
  const uint32_t t = _stream_gap_ms * 2;
  return t < STREAM_T_MIN_MS ? STREAM_T_MIN_MS : (t > STREAM_T_MAX_MS ? STREAM_T_MAX_MS : t);
}
bool FwOtaTransfer::listening(uint32_t now) {
  if (!_stream_ms) return false;
  const uint32_t t = streamT();
  if (since(now, _stream_ms) < t) return true;
  if (!_gap_at_ms) _gap_at_ms = (now + (uint32_t)_mesh->getRNG()->nextInt(0, (int)t)) | 1;
  if ((int32_t)(now - _gap_at_ms) < 0) return true;
  _stream_ms = 0; _gap_at_ms = 0; _stream_gap_ms = 0;
  return false;
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
  clearState();
  _gap_phase = false; _listening = false;
  SlotHeader ih;
  if (readSlotHeader(ih) && installedImage(ih))         // (v1.17.28, C1) the node already RUNS this image: fill the slot, serve it, no re-flash
    strncpy(_last_err, "already installed: slot filled, served", sizeof(_last_err) - 1);   // (rc15) no announce: it speaks when there is demand
  else if (_fill) strcpy(_last_err, "fleet fill done: served, not installed");   // (rc17, owner 9.1 b) never installs by itself
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
  _scan_sess = newSess(0x77A1); _scan_seq = 0; _scan_active = true; _scan_resends = 0; _scan_resend_ms = 0;
  _scanned = true;                                      // (rc17, C38)
  _scan_start_ms = now + delay_ms; _scan_pending = true;
  _scan_end_ms = _scan_start_ms + SCAN_MS;
  if (!delay_ms) scanSend();
}
void FwOtaTransfer::scanSend() {
  _scan_pending = false;
  sendListReq();                                        // direct neighbours (also older servers and the Pager)
  _disc_sess = _scan_sess;                              // (rc15) and everything farther: the flood 6 s later (near answers come first; far holders
  strcpy(_disc_flood_target, "*"); _disc_flood_ms = millis() + 6000 + _mesh->getRNG()->nextInt(0, 3000);   // spread: two lists at once collided   //   delay theirs by distance and drop it when 2 for the image were heard)
  _scan_resend_ms = scanResendAt(0, millis(), (uint32_t)_mesh->getRNG()->nextInt(0, SCAN_REQ_SPREAD_MS + 1), _scan_end_ms);
}

bool FwOtaTransfer::cmdList(char* reply) {
  if (!_circle_set) { strcpy(reply, "ERR - set 'fw passphrase <phrase>' first: only nodes of your circle are listed"); return true; }
  // allowed while a pull WAITS for its server (PH_OFFERING): the scan is independent and shows what else is in reach
  if ((_phase == PH_DOWNLOADING && !_fill_paused) || _phase == PH_VERIFYING || _staging) { strcpy(reply, "ERR - busy (download/stage in progress)"); return true; }
  startScan(SCAN_START_MS);
  if (_flood_host && !_flood_host->regionSet() && !_region_hint) {   // relays that only forward region floods drop an unscoped discovery
    _region_hint = true;                                 // (rc21, C60) once per boot, not on every scan
    strcpy(reply, "scanning ~25 s, then: fw list. No default region set: region-only relays drop the discovery ('region default <name>')");
  }
  else
    strcpy(reply, "scanning for peers that offer firmware (direct and through relays) - wait ~25 s, then type: fw list");
  return true;
}

void FwOtaTransfer::onListReq(const uint8_t* b, uint32_t len) {
  if (len < 8 + XFER_MAC_LEN) return;
  uint32_t sess; memcpy(&sess, &b[2], 4);
  if ((_serving && imageStillServed()) || _bl_serving) { if (!answerOnce(sess, false)) return; }   // (v1.17.28) at most twice per scan
  if (_serving && imageStillServed()) sendListRsp(b, _serve_man, 300 + _mesh->getRNG()->nextInt(0, 2500));   // spread the answers of many servers over a few seconds
  if (_bl_serving)                    sendListRsp(b, _bl_man,    300 + _mesh->getRNG()->nextInt(0, 2500));   // a node with both offers answers twice (kind byte differs)
}

static const uint32_t LIST_RSP_MAX = 2 + 4 + 2 + 4 + 4 + 4 + 16 + 1 + 8 + 1 + MAX_ROUTE_BYTES + SWARM_BLK + XFER_MAC_LEN;

// The exact length of a LIST_RSP (with kind, board and route) inside `avail` bytes, 0 = none. A channel message arrives DECRYPTED WITH ITS AES
// PADDING (MeshCore rounds the length up to 16 bytes), so a binary answer carried in a flood must be cut to its own length before the MAC
// check - v1.17.23 did not, and dropped every flooded answer and heal notice (found 2026-09-27 on the bench: "fw peers" stayed empty).
static uint32_t listRspLen(const uint8_t* b, uint32_t avail) {
  if (avail < 46 + XFER_MAC_LEN || !rtValid(b[45])) return 0;
  uint32_t n = 46 + rtBytes(b[45]) + ((b[36] & KIND_SWARM) ? SWARM_BLK : 0) + XFER_MAC_LEN;   // (swarm v1) kind bit 7: the swarm block follows
  return n <= avail ? n : 0;
}

// (swarm v1) expected chunks per HOUR from a holder: its fair share (its duty / (pulls it serves + 1), at most what we ask), on the channel
// model both ends pace by, divided by the AIRTIME COST of its relays (every relay re-sends each chunk: 2 relays = 3x the airtime - and the
// energy of solar relays), times a link factor from the answer's SNR margin over SF7's floor (-7.5 dB: fewer retries on a clean link).
// A LOW-power holder only as the last resort (/10), a CRITICAL one never (0). Holders of older firmware (no swarm block) count as idle 0.30.
static uint32_t holderRate(uint8_t ask, uint8_t hduty, uint8_t hpulls, uint32_t air_ms, uint8_t relays, int8_t snr4, uint8_t power) {
  if (power == 3 || !air_ms) return 0;
  uint32_t share = hduty ? hduty / (uint32_t)(hpulls + 1) : DUTY_DEFAULT_PCT;
  if (ask && share > ask) share = ask;
  if (share < DUTY_MIN_PCT) share = DUTY_MIN_PCT;
  uint32_t r = (uint32_t)chanPct((uint8_t)share) * 36000UL / (air_ms * (relays + 1UL));   // pct of an hour of channel / airtime per chunk
  int32_t margin10 = ((int32_t)snr4 * 10) / 4 + 75;      // SNR margin in 0.1 dB
  int32_t f = margin10 < 30 ? 30 : (margin10 > 100 ? 100 : margin10);
  r = r * (uint32_t)f / 100;
  if (power == 2) r /= 10;
  return r ? r : 1;
}

// (swarm v1) how long a pull by image waits on a silent holder before it moves to the next: 6 of its windows at the measured rate (8 chunks
// each), never below 2 min; no rate yet = 6 min (owner decision 8.3)
static uint32_t switchAfter(uint32_t chunks_per_hour) {
  if (!chunks_per_hour) return 360000;
  uint32_t w = 6UL * 8UL * 3600000UL / chunks_per_hour;
  return w < SWITCH_MIN_MS ? SWITCH_MIN_MS : w;
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
  r[n++] = (uint8_t)(m.kind | KIND_SWARM);            // (2026-09-20) kind + board; (swarm v1) bit 7: the swarm block follows the route
  memcpy(&r[n], g_fwpull_tag.board, 8); n += 8;
  r[n++] = enc;                                       // (2026-09-21) the path the discovery took, puller -> here: encoded length + hashes (0 = direct neighbour)
  if (rtBytes(enc)) { memcpy(&r[n], fwd, rtBytes(enc)); n += rtBytes(enc); }
  memcpy(&r[n], m.sha256, 8); n += 8;                 // (swarm v1) the image identity ...
  r[n++] = _duty_pct;                                 // ... our duty, the pulls we serve now and our power state: the puller ranks holders
  r[n++] = activePulls();
  r[n++] = powerState();
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
void FwOtaTransfer::floodListRsp(const uint8_t* r, uint32_t n, uint32_t delay_ms, bool heal, bool ann) {
  if (!_mesh || !_circle_set) return;
  uint8_t d[5 + 6 + LIST_RSP_MAX]; uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
  memcpy(d, &ts, 4); d[4] = 0;                        // like a channel text: timestamp, flags, then our marker and the binary answer
  memcpy(&d[5], ann ? FLOOD_ANN : heal ? FLOOD_HEAL : FLOOD_RSP, 6);
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

// ---- "fw pull AABB[CC]": find the server by a short id ----
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
  f.write((uint8_t*)&m, 4); f.write(_res_id, 3); f.write(&_res_len, 1); f.write(&bl, 1); f.close();
}
void FwOtaTransfer::clearResolve() { _resolving = false; _res_scanning = false; if (_fs) _fs->remove(RESOLVE_FILE); }

void FwOtaTransfer::startResolve(const uint8_t* id, uint8_t len, bool bl, char* reply) {
  memcpy(_res_id, id, len); if (len < 3) _res_id[2] = 0; _res_len = len; _res_bl = bl;
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

void FwOtaTransfer::sendDiscovery(const char* target, bool zero_hop) {
  if (!_mesh || !_circle_set) return;
  uint8_t d[5 + 48]; uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
  memcpy(d, &ts, 4); d[4] = 0;                        // like a channel text: timestamp, flags, text
  _disc_seq++;
  // " F" (v1.17.23): answer with a FLOOD, not direct along the reversed path. Older servers parse three fields and ignore it (they answer direct).
  // (rc15) a zero-hop search (neighbours only) asks for DIRECT answers (no " F"): one transmission, answers without a flood
  int tl = snprintf((char*)&d[5], sizeof(d) - 5, zero_hop ? "LOTA1 %08lX%04X %s" : "LOTA1 %08lX%04X %s F", (unsigned long)_disc_sess, (unsigned)_disc_seq, target);
  if (tl <= 0 || tl >= (int)sizeof(d) - 5) return;
  mesh::Packet* p = _mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _disc_ch, d, 5 + tl);
  if (!p) return;
  if (zero_hop) { stats.searches_zero++; if (!_flood_host || !_flood_host->sendOta(p, nullptr, 0, 100)) _mesh->sendZeroHop(p, 100); }
  else { stats.searches_flood++; floodOut(p, 100); }
}

bool FwOtaTransfer::pullTargetId(uint8_t id[4]) const {
  return _man.name[0] == '@' && strlen(_man.name) >= 9 && hexN(id, 4, _man.name + 1);   // "@ID8" (app) or "@ID8!" (bootloader)
}

// A discovery flood reached us (any hop count). Answer if it is for everyone, for our id, or for our exact name, and we serve
// something: a LIST_RSP sent direct back along the reversed path, carrying the path so the puller learns its route.
void FwOtaTransfer::onDiscovery(mesh::Packet* pkt, const uint8_t* data, size_t len) {
  if (len >= 5 + 6 + 2 && (memcmp(data + 5, FLOOD_TREQ, 6) == 0 || memcmp(data + 5, FLOOD_TANS, 6) == 0)) {   // (rc21, C57)
    onTime(data + 11, (uint32_t)len - 11, pkt->getPathHashCount()); return;
  }
  if (len >= 5 + 6 + 2 && memcmp(data + 5, FLOOD_CLAIM, 6) == 0) { onClaim(data + 11, (uint32_t)len - 11); return; }   // (study C/D)
  if (len >= 5 + 6 + 2 + XFER_MAC_LEN && (memcmp(data + 5, FLOOD_RSP, 6) == 0 || memcmp(data + 5, FLOOD_HEAL, 6) == 0
                                        || memcmp(data + 5, FLOOD_ANN, 6) == 0)) {   // (swarm v1) an announcement is a session-0 answer
    // a flooded answer (or heal notice) from a server: its collected path is the way from that server to here
    const uint8_t* b = data + 11; uint32_t bl = listRspLen(b, (uint32_t)len - 11);   // drop the AES padding
    uint8_t renc = (uint8_t)(((pkt->getPathHashSize() - 1) << 6) | pkt->getPathHashCount());
    if (!bl || b[0] != XFER_MAGIC || b[1] != MSG_LIST_RSP || !rtValid(renc) || !macOk(_list_key, b, bl)) return;
    answerHeard(b, bl);                                 // (rc15) another holder answered this search: count it (suppression)
    if (pkt->getPathHashCount()) noteRssi(0);          // relayed: RSSI/SNR here are the last relay's, not the server's (rc10, C24: at 0 hops
                                                        // they ARE the server's - MyMesh::onGroupDataRecv hands the radio RSSI over)
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
  // (swarm v1) '=<16 hex>': a pull looks for an IMAGE - every holder of exactly that image answers (only that offer)
  bool img_app = false, img_bl = false;
  if (target[0] == '=') {
    uint8_t dg[8];
    if (strlen(target) != 17 || !hexN(dg, 8, target + 1)) return;
    img_app = _serving && memcmp(dg, _serve_man.sha256, 8) == 0;
    img_bl = _bl_serving && memcmp(dg, _bl_man.sha256, 8) == 0;
  }
  bool mine = strcmp(target, "*") == 0
           || (target[0] == '@' && strncasecmp(target + 1, id8, 8) == 0)
           || img_app || img_bl
           || (_name_src && strcasecmp(target, _name_src) == 0);
  if (!mine) return;
  uint8_t req[8]; req[0] = XFER_MAGIC; req[1] = MSG_LIST_REQ;
  uint32_t s32 = (uint32_t)sess; uint16_t s16 = (uint16_t)seq;
  memcpy(&req[2], &s32, 4); memcpy(&req[6], &s16, 2);
  uint8_t back[MAX_ROUTE_BYTES]; rtReverse(back, pkt->path, enc);
  uint32_t d = DISC_RSP_MIN_MS + _mesh->getRNG()->nextInt(0, DISC_RSP_SPREAD_MS) + (uint32_t)hops * 300;
  const uint8_t how = target[0] == '='                // (swarm v1) an image search: answered like a targeted pull, that offer only
      ? discAnswer(target, hops, flood, img_app && imageStillServed(), img_bl)
      : discAnswer(target, hops, flood, _serving && imageStillServed(), _bl_serving);   // (v1.17.28, C2/C6/C7)
  if (!how || !answerOnce(s32, (how & 4) != 0)) return;   // (v1.17.28) once per scan / pull session
  if (how & 8) {                                        // direct, back along the reversed discovery path (C6: a pull's target also floods)
    if (how & 1) sendListRsp(req, _serve_man, d, pkt->path, enc, back);
    if (how & 2) sendListRsp(req, _bl_man, d + 700, pkt->path, enc, back);
  }
  if (how & 4) {                                        // flooded: works even where the reversed path is one-way; carries the path it came by
    // (rc15) near holders first (1.5 s per hop), dropped when 2 answers for the same image were heard - see answerLater()
    const uint32_t fd = ((how & 8) ? d + 1500 : d) + (uint32_t)hops * ANS_HOP_MS;
    uint8_t r[LIST_RSP_MAX];
    if (how & 1) answerLater(r, buildListRsp(r, &req[2], _serve_man, pkt->path, enc), fd);
    if (how & 2) answerLater(r, buildListRsp(r, &req[2], _bl_man, pkt->path, enc), fd + 700);
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
  const uint8_t* sw = nullptr;                                     // (swarm v1) digest, duty, pulls, power
  if (len >= 2 + 4 + 2 + 4 + 4 + 4 + 16 + 1 + 8 + XFER_MAC_LEN) {
    kind = b[36] & 0x7F;
    if (memcmp(&b[37], g_fwpull_tag.board, 8) != 0) return;     // a server of another board has nothing we could use: not listed
    if (len >= 46 + XFER_MAC_LEN) {
      rl = b[45];                                                   // encoded route length
      if (!rtValid(rl) || len < 46u + rtBytes(rl) + XFER_MAC_LEN) return;
      rt = &b[46];
      if ((b[36] & KIND_SWARM) && len >= 46u + rtBytes(rl) + SWARM_BLK + XFER_MAC_LEN) sw = &b[46 + rtBytes(rl)];
    }
  }
  uint8_t rev[MAX_ROUTE_BYTES];                                    // (rc15) a flooded advert carries no route: the way to the holder is the
  if (sess == 0 && rl == 0 && ret && rtCount(ret_enc)) { rtReverse(rev, ret, ret_enc); rt = rev; rl = ret_enc; }   // reverse of the path it came by
  // (swarm v1) a holder of the image this pull targets (answer to our image search, or an announcement): remember it as a holder
  const bool holder_ans = sw && _img_set && kind == _img_kind && memcmp(sw, _img_digest, 8) == 0 && !heal
                          && (sess == 0 || sess == _disc_sess) && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING);
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
  uint8_t tid0[4];
  const bool from_target = pullTargetId(tid0) && memcmp(tid0, id, 4) == 0 && kind == _man.kind;
  if (!_scan_active && sess == _disc_sess && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING) && (from_target || !holder_ans)) {
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
  if (!holder_ans && (!_scan_active || sess != _scan_sess)) return;
  PeerInfo* p = nullptr;
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used && memcmp(_peers[i].id, id, 4) == 0 && _peers[i].kind == kind) { p = &_peers[i]; break; }
  if (p && sw) { memcpy(p->digest, sw, 8); p->sduty = sw[8]; p->spulls = sw[9]; p->spower = sw[10]; p->swarm = true;   // the latest state
    memcpy(&p->fw_version, &b[12], 4); memcpy(&p->payload_len, &b[16], 4); }   // (rc15) incl. the image: a holder may have a new one now
  if (holder_ans && sess == _disc_sess && _disc_flood_ms && strcmp(_disc_flood_target, "*") != 0) _disc_flood_ms = 0;   // (rc15) found nearby
  if (holder_ans && _move_to < 0) _move_check_ms = millis();     // (rc15) a holder notice: is it clearly faster? (tick, once it is in the table)
  if (holder_ans) {                                               // (swarm v1) a holder of our image: in the holder list ...
    bool known = false;
    for (uint8_t h = 0; h < _nholders; h++) if (memcmp(_holders[h], id, 4) == 0) known = true;
    if (!known && _nholders < SWARM_HOLDERS) { memcpy(_holders[_nholders++], id, 4); saveState(); }
    _last_ans_ms = millis() ? millis() : 1;
  }
  if (p) {                                                        // heard again (zero-hop and flooded answers): keep the shortest path
    if (rtCount(rl) < rtCount(p->route_len)) {
      if (rtBytes(rl)) memcpy(p->route, rt, rtBytes(rl));
      p->route_len = rl; p->has_ret = ret != nullptr; p->ret_len = ret ? ret_enc : 0;
      if (ret && rtBytes(ret_enc)) memcpy(p->ret, ret, rtBytes(ret_enc));
    } else if (ret && !p->has_ret && rtCount(rl) == rtCount(p->route_len)) {   // same way there, now with a known way back
      p->has_ret = true; p->ret_len = ret_enc; if (rtBytes(ret_enc)) memcpy(p->ret, ret, rtBytes(ret_enc));
    }
    if (rl == 0 && (!ret || rtCount(ret_enc) == 0) && _rx_rssi < 0) { p->rssi = _rx_rssi; p->snr4 = snr4; }   // (rc10, C24) a 0-hop flooded answer too
    return;
  }
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (!_peers[i].used) { p = &_peers[i]; break; }
  if (!p && holder_ans) {                                         // (swarm v1) table full: a holder of our image replaces a non-holder
    for (uint8_t i = 0; i < MAX_PEERS && !p; i++) {
      bool is_h = false; for (uint8_t h = 0; h < _nholders; h++) if (memcmp(_holders[h], _peers[i].id, 4) == 0) is_h = true;
      if (!is_h) p = &_peers[i];
    }
  }
  if (!p) return;
  memset(p, 0, sizeof(*p));
  p->used = true; memcpy(p->id, id, 4); p->kind = kind;
  if (sw) { memcpy(p->digest, sw, 8); p->sduty = sw[8]; p->spulls = sw[9]; p->spower = sw[10]; p->swarm = true; }
  memcpy(&p->fw_version, &b[12], 4); memcpy(&p->payload_len, &b[16], 4);
  memcpy(p->name, &b[20], 16); p->name[16] = 0;
  p->rssi = _rx_rssi; p->snr4 = snr4;
  if (rtBytes(rl)) memcpy(p->route, rt, rtBytes(rl));
  p->route_len = rl;
  p->has_ret = ret != nullptr; p->ret_len = ret ? ret_enc : 0;
  if (ret && rtBytes(ret_enc)) memcpy(p->ret, ret, rtBytes(ret_enc));
}

// (swarm v1, owner decisions 8.1/8.2) "fw peers [n]": the circle's IMAGES, newest app first: "#1 fw-295 x3 (0hop) | #2 bl-rev-7 x4 (0hop)".
// 'fw pull #n' / 'fw blupdate #n' take an image and choose its best holder; "fw peers hosts [n]" is the per-host view of rc1-rc11.
bool FwOtaTransfer::cmdPeers(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (strncmp(arg, "hosts", 5) == 0) return cmdPeersHosts(arg + 5, reply);
  if (_scan_active) { snprintf(reply, 160, "still scanning (%lu s left)...", (unsigned long)((_scan_end_ms - millis()) / 1000 + 1)); return true; }
  ImgInfo im[MAX_PEERS]; const int n = imageList(im, MAX_PEERS);
  if (!n) { strcpy(reply, _scanned ? "the last scan found no firmware in your circle ('fw peers' scans again)" : "no scan yet - 'fw peers' scans ~25 s"); return true; }   // (rc17, C38) nothing sent
  int from = atoi(arg); if (from < 1) from = 1;
  if (from > n) { snprintf(reply, 160, "no image #%d (%d found)", from, n); return true; }
  size_t l = 0; reply[0] = 0; int next = 0;
  for (int k = from - 1; k < n; k++) {
    bool twin = false;                                   // two builds with the same version: tell them apart by the digest
    for (int j = 0; j < n; j++) if (j != k && im[j].kind == im[k].kind && im[j].ver == im[k].ver) twin = true;
    char e[48], dg[8] = "";
    if (twin && im[k].swarm) snprintf(dg, sizeof(dg), " [%02X%02X]", im[k].digest[0], im[k].digest[1]);
    const int w = snprintf(e, sizeof(e), "%s#%d %s-%lu%s x%u (%uhop)", l ? " | " : "", k + 1, im[k].kind == KIND_BOOTLOADER ? "bl-rev" : "fw",
                           (unsigned long)im[k].ver, dg, (unsigned)im[k].n, (unsigned)(im[k].minhops == 0xFF ? 0 : im[k].minhops));
    if (w < 0) continue;
    if (l + (size_t)w > 160 - 24) { next = k + 1; break; }
    memcpy(reply + l, e, (size_t)w); l += (size_t)w; reply[l] = 0;
  }
  static const char HINT[] = " | holders: 'fw list hosts'";
  if (next) snprintf(reply + l, 160 - l, " | ...'fw list %d'", next);
  else if (l + sizeof(HINT) < 158) memcpy(reply + l, HINT, sizeof(HINT));    // (C31) only when it fits whole, with margin (it was cut at 160 B)
  return true;
}

bool FwOtaTransfer::cmdPeersHosts(const char* arg, char* reply) {
  // "fw peers hosts [n]" (v1.17.23: one block per host): "AABBCCDD NAME -37dBm snr12.75 [2hop] #1 fw-210 #2 bl-rev-3 | next host ...". The #n numbers
  // are the same as before (every offer in table order), so 'fw pull #n' / 'fw blupdate #n' are unchanged. A page never splits a host and never
  // drops an entry: what does not fit opens the next page, which the reply names (lesson of v1.17.18: #3 used to vanish between pages).
  if (_scan_active) { snprintf(reply, 160, "still scanning (%lu s left)...", (unsigned long)((_scan_end_ms - millis()) / 1000 + 1)); return true; }
  while (*arg == ' ') arg++;
  int from = atoi(arg); if (from < 1) from = 1;
  uint8_t num[MAX_PEERS] = {0}, total = 0;                     // #n of each table entry
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used) num[i] = ++total;
  if (!total) { strcpy(reply, _scanned ? "the last scan found no peers offering firmware ('fw peers' scans again)" : "no scan yet - 'fw peers' scans ~25 s"); return true; }
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
  if (next) { size_t n = strlen(reply); snprintf(reply + n, 160 - n, " | ...'fw list hosts %u'", (unsigned)next); }
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
  if (!any) l += (size_t)snprintf(reply + l, 160 - l, " direct (no pull yet; a listed peer's discovered route is used automatically)");
  if (_sw_why && l < 118) {                               // (rc20, C48) the last holder change of the pull, when it fits (37 B)
    static const char* const WHY[] = { "", "silent", "faster", "back", "nearer" };
    char ago[16]; agoText(since(millis(), _sw_ms) / 1000, ago, sizeof(ago));
    snprintf(reply + l, 160 - l, " | holder %02X%02X%02X%02X (%s) %s ago", _sw_id[0], _sw_id[1], _sw_id[2], _sw_id[3], WHY[_sw_why < 5 ? _sw_why : 0], ago);
  }
  return true;
}

// "fw pull <id|#n>" / "fw blupdate [force] <id|#n>"
bool FwOtaTransfer::cmdPullPeer(const char* args, char* reply, bool bl) {
  if (!_circle_set) { strcpy(reply, "ERR - set 'fw passphrase <phrase>' first (the same on every node of your circle)"); return true; }
  // During a pull the same command SWITCHES the source (checked below, once the peer is known): the chunks are kept when the new server
  // offers the identical image. That is how a pull survives a server that went away for good.
  const bool in_pull = (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) && !_fill_paused;   // (rc19) a paused fill gives way to a new pull
  if (in_pull && (_man.kind == KIND_BOOTLOADER) != bl) { strcpy(reply, "ERR - a pull of the other kind is in progress ('fw pull-abort' to cancel)"); return true; }
  while (*args == ' ') args++;
  _bl_force = false;
  if (bl && strncmp(args, "force ", 6) == 0) { _bl_force = true; args += 6; while (*args == ' ') args++; }
  char tok[12]; uint8_t tl = 0;
  while (*args && *args != ' ' && tl < sizeof(tok) - 1) tok[tl++] = *args++;
  tok[tl] = 0;
  while (*args == ' ') args++;
  int idx = -1;
  // (swarm v1) '#n' = image n of 'fw peers'; 'fw-295' / 'bl-rev-7' = that version: the BEST HOLDER is chosen (fair share, relays, SNR,
  // power) and the other holders are kept for failover
  int img = -1; ImgInfo im[MAX_PEERS]; const int ni = imageList(im, MAX_PEERS);
  if (tok[0] == '#' || (tl <= 2 && tok[0] >= '0' && tok[0] <= '9')) {
    const int want = atoi(tok[0] == '#' ? tok + 1 : tok);
    if (want < 1 || want > ni) { snprintf(reply, 160, "ERR - no image #%d ('fw peers', then 'fw list')", want); return true; }
    img = want - 1;
  } else if (strncasecmp(tok, "fw-", 3) == 0 || strncasecmp(tok, "bl-rev-", 7) == 0) {
    const bool want_bl = strncasecmp(tok, "bl-rev-", 7) == 0; const uint32_t v = (uint32_t)strtoul(tok + (want_bl ? 7 : 3), nullptr, 10);
    int hits = 0;
    for (int k = 0; k < ni; k++) if ((im[k].kind == KIND_BOOTLOADER) == want_bl && im[k].ver == v) { if (!hits) img = k; hits++; }
    if (!hits) { snprintf(reply, 160, "ERR - nobody in your circle offers %s ('fw peers', then 'fw list')", tok); return true; }
    if (hits > 1) { snprintf(reply, 160, "ERR - two different builds are called %s: pick one by #n ('fw list')", tok); return true; }
  }
  if (img >= 0) {
    if ((im[img].kind == KIND_BOOTLOADER) != bl) { strcpy(reply, bl ? "ERR - that is an app image: use 'fw pull'" : "ERR - that is a bootloader image: use 'fw blupdate'"); return true; }
    idx = bestHolder(im[img].kind, im[img].ver, im[img].swarm ? im[img].digest : nullptr, nullptr, 0);
    if (idx < 0) { strcpy(reply, "ERR - every holder of that image is on a CRITICAL battery - try later"); return true; }
    if (!in_pull) {                                       // a new pull by image: the image and up to 4 holders, best first
      _img_set = im[img].swarm; _img_kind = im[img].kind; _img_ver = im[img].ver; memcpy(_img_digest, im[img].digest, 8);
      _nholders = 0; _holders_tried = 0;
      uint8_t tried = 0;
      for (int h = idx; h >= 0 && _nholders < SWARM_HOLDERS; ) {
        memcpy(_holders[_nholders], _peers[h].id, 4); tried |= (uint8_t)(1u << _nholders); _nholders++;
        h = bestHolder(im[img].kind, im[img].ver, im[img].swarm ? im[img].digest : nullptr, nullptr, tried);
      }
    }
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
    if (bl && !_route_len) { strcpy(reply, "ERR - a bootloader update needs a compatible offer in reach: run 'fw peers' first (behind relays: 'fw route ..' then the full 8-hex id)"); return true; }
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
      startResolve(sid, tl / 2, bl, reply); return true;
    }
  }
  if (idx < 0) { strcpy(reply, "ERR - unknown peer: 'fw peers' first, or give 2/3 bytes of its id (AABB / AABBCC) or the full 8-hex id"); return true; }
  if (img < 0 && !in_pull) { _img_set = false; _nholders = 0; }   // (swarm v1) a pull from one named host: no holder failover
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
    if (cur_ver && strcmp(p.name, "(not listed)") == 0) { strcpy(reply, "ERR - to switch, pick the new server from 'fw peers' / 'fw list' (its version is checked first)"); return true; }
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
  // a word typed after the id (an old habit: the admin password of v1.17.27 and older) is ignored
  char target[16]; snprintf(target, sizeof(target), "@%02X%02X%02X%02X%s", p.id[0], p.id[1], p.id[2], p.id[3], bl ? "!" : "");
  if (in_pull) { switchSource(target, reply); return true; }
  startPullAt(target, reply, bl);
  return true;
}

// Re-target a running pull to another server. It is treated like a resume after an outage: the new server is asked for its offer, and if
// the image is identical (sameImage: sizes, CRCs, SHA-256, version, LZMA props) the download continues from the first missing chunk - the
// sparse chunk store is keyed by image, not by server. A different image restores the previous server and key; nothing is erased.
void FwOtaTransfer::revertSwitch(const char* why) {   // undo switchSource(): previous server, keep every chunk, keep waiting/resuming
  memset(_man.name, 0, sizeof(_man.name)); strncpy(_man.name, _switch_prev_name, sizeof(_man.name) - 1);
  memcpy(_route, _switch_prev_route, sizeof(_switch_prev_route)); _route_len = _switch_prev_route_len;
  setRet(_switch_prev_ret_set ? _switch_prev_ret : nullptr, _switch_prev_ret_len, false);
  routeReset();                                            // routes of the previous server again (from its route)
  _switching = false; _resuming = true; _phase = PH_OFFERING;
  _prev_session = _session; _session = newSess(0x3C3C ^ _cm.have); _sess_set = false; _attempts = 0; _backoff_ms = 0; _wait_started_ms = millis();
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
  saveState();
}

void FwOtaTransfer::switchSource(const char* target, char* reply) {
  if (!_saved_man.n_chunks && !_man.n_chunks) {           // still at first contact: nothing downloaded to keep -> simply a new pull
    _phase = PH_IDLE;
    startPullAt(target, reply, _man.kind == KIND_BOOTLOADER);
    return;
  }
  if (!_switching) {                                      // keep what to go back to (a second switch before the first answered keeps the original)
    strncpy(_switch_prev_name, _man.name, sizeof(_switch_prev_name) - 1); _switch_prev_name[sizeof(_switch_prev_name) - 1] = 0;
  }
  if (_phase == PH_DOWNLOADING) _saved_man = _man;       // identity of what is in the slot now
  if (_next_pull_duty) { _pull_duty_pct = _next_pull_duty; _next_pull_duty = 0; }   // "fw pull <other> duty x" re-targets at that duty
  memset(_man.name, 0, sizeof(_man.name)); strncpy(_man.name, target, sizeof(_man.name) - 1);
  routeReset();                                            // another server: its routes start from the one fw list found
  _switching = true; _resuming = true; _mac_fail = 0;
  _prev_session = _session; _session = newSess(0x5A5A ^ _cm.have); _sess_set = false;
  _win_count = 0; _retries = 0; _last_req_ms = 0; _attempts = 0; _backoff_ms = 0; _retry_timeout_ms = 0;
  _wait_started_ms = millis();
  _phase = PH_OFFERING;
  strncpy(_last_err, "switching source", sizeof(_last_err) - 1);
  saveState();
  snprintf(reply, 160, "OK - switching the pull to %s: %lu/%lu chunks are kept if it serves the same image ('fw pull-status')",
           target, (unsigned long)_cm.have, (unsigned long)_saved_man.n_chunks);
}

// (rc8, C23) "fw wdt [clear|hang]"
// ---- (rc21, C57) time from a swarm member with an RTC (own packets on the circle's discovery channel, zero-hop only) ----
void FwOtaTransfer::sendTime(uint8_t type, uint32_t nonce) {
  uint8_t b[14 + XFER_MAC_LEN]; b[0] = XFER_MAGIC; b[1] = type; memcpy(&b[2], &nonce, 4);
  uint32_t n = 6;
  const uint32_t t = _mesh->getRTCClock()->getCurrentTime();
  if (type == MSG_TIME_RSP) { memcpy(&b[6], &t, 4); memcpy(&b[10], _pub, 4); n = 14; }
  uint8_t full[32]; hmac(_list_key, 16, b, n, nullptr, 0, full); memcpy(&b[n], full, XFER_MAC_LEN); n += XFER_MAC_LEN;
  uint8_t d[11 + sizeof(b)]; memcpy(d, &t, 4); d[4] = 0; memcpy(&d[5], type == MSG_TIME_REQ ? FLOOD_TREQ : FLOOD_TANS, 6); memcpy(&d[11], b, n);
  mesh::Packet* p = _mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _disc_ch, d, 11 + n);
  if (!p) return;
  if (type == MSG_TIME_RSP) _mesh->sendZeroHop(p, (uint32_t)0);   // the time is read now: not behind the OTA queue
  else if (!_flood_host->sendOta(p, nullptr, 0, 0)) _mesh->sendZeroHop(p, (uint32_t)0);
}

void FwOtaTransfer::timeTick(uint32_t now) {
  if (!_circle_set || !_mesh || !_flood_host) return;
  if (_ta_at_ms && (int32_t)(now - _ta_at_ms) >= 0) {
    _ta_at_ms = 0;
    if (_flood_host->clockSource()) { sendTime(MSG_TIME_RSP, _ta_nonce); _ta_sent++; }
  }
  if (_flood_host->clockSource()) return;                 // a node with an RTC never asks (and draws no random number: rc20 timing)
  if (!_tq_next_ms) { _tq_next_ms = (now + TQ_FIRST_MS + (uint32_t)_mesh->getRNG()->nextInt(0, (int)TQ_FIRST_MS)) | 1; return; }   // 20-40 s
  if ((int32_t)(now - _tq_next_ms) < 0) return;
  static const uint32_t RETRY_MS[3] = { 120000UL, 600000UL, 1800000UL };
  _tq_nonce = (uint32_t)_mesh->getRNG()->nextInt(1, 0x7FFFFFFF); _tq_sent_ms = now;
  sendTime(MSG_TIME_REQ, _tq_nonce);
  _tq_next_ms = (now + (_tq_tries < 3 ? RETRY_MS[_tq_tries] : TQ_PERIOD_MS)) | 1;
  if (_tq_tries < 255) _tq_tries++;
}

void FwOtaTransfer::onTime(const uint8_t* b, uint32_t len, uint8_t hops) {
  if (hops || !_circle_set || !_flood_host || len < 2 || b[0] != XFER_MAGIC) return;   // a neighbour's time only
  const uint32_t n = b[1] == MSG_TIME_REQ ? 6 : b[1] == MSG_TIME_RSP ? 14 : 0;           // fixed sizes (the channel pads to 16 B)
  if (!n || len < n + XFER_MAC_LEN || !macOk(_list_key, b, n + XFER_MAC_LEN)) return;
  uint32_t nonce; memcpy(&nonce, &b[2], 4);
  if (b[1] == MSG_TIME_REQ) {
    if (!_flood_host->clockSource()) return;              // only a node with a valid RTC answers
    _ta_nonce = nonce; _ta_at_ms = (millis() + TA_MIN_MS + TA_SLOT_MS * (uint32_t)_mesh->getRNG()->nextInt(0, (int)TA_SLOTS)) | 1;
    return;
  }
  if (_ta_at_ms && nonce == _ta_nonce) { _ta_at_ms = 0; _ta_supp++; }   // another member answered first: ours is not needed
  if (!_tq_nonce || nonce != _tq_nonce || _flood_host->clockSource()) return;   // not our open request (or a replay)
  _tq_nonce = 0; _tq_tries = 3; _tq_next_ms = (millis() + TQ_PERIOD_MS) | 1;
  uint32_t ref; memcpy(&ref, &b[6], 4);
  const int32_t d = (int32_t)(ref - _mesh->getRTCClock()->getCurrentTime());
  memcpy(_clk_from, &b[10], 4); _clk_at_ms = millis() | 1; _clk_delta = d;
  _clk_how = 3;
  if (d < -CLK_BOUND_S || (_clk_synced && d > CLK_BOUND_S)) { _clk_refused++; return; }   // a peer far off: not taken
  _clk_synced = true; _clk_n++;
  _clk_how = d >= CLK_STEP_MIN_S ? 1 : d <= -CLK_HOLD_MIN_S ? 2 : 0;
  if (d >= CLK_STEP_MIN_S || d <= -CLK_HOLD_MIN_S) _flood_host->clockDiscipline(ref);
}

bool FwOtaTransfer::cmdTime(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  const bool rtc = _flood_host && _flood_host->clockSource();
  if (!strcmp(arg, "ask")) {
    if (!_circle_set) strcpy(reply, "ERR - set 'fw passphrase <phrase>' first: the time comes from your circle");
    else if (rtc) strcpy(reply, "OK - this node has a valid RTC: it answers time requests, it does not ask");
    else { _tq_next_ms = millis() | 1; _tq_tries = 3; strcpy(reply, "OK - asking the neighbours with an RTC for the time: 'fw time' in ~10 s"); }
    return true;
  }
  if (*arg) { strcpy(reply, "usage: fw time [ask]"); return true; }
  const uint32_t t = _mesh ? _mesh->getRTCClock()->getCurrentTime() : 0;
  uint32_t z = t / 86400 + 719468, era = z / 146097, doe = z - era * 146097;     // days -> civil date (H. Hinnant)
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  uint32_t dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9, yy = yoe + era * 400 + (mm <= 2);
  int l = snprintf(reply, 160, "%02lu:%02lu:%02lu %lu/%lu/%lu UTC | ", (unsigned long)(t % 86400 / 3600), (unsigned long)(t % 3600 / 60),
                   (unsigned long)(t % 60), (unsigned long)dd, (unsigned long)mm, (unsigned long)yy);
  if (rtc) snprintf(reply + l, 160 - l, "RTC chip: answers neighbours' requests (sent %u, dropped %u)", _ta_sent, _ta_supp);
  else if (!_circle_set) snprintf(reply + l, 160 - l, "no RTC; no circle: no swarm time");
  else if (!_clk_at_ms) snprintf(reply + l, 160 - l, "no RTC: no swarm time yet (asked %u times)", _tq_tries);
  else {
    const int32_t d = _clk_delta; const uint32_t ago = (millis() - _clk_at_ms) / 60000;
    snprintf(reply + l, 160 - l, "no RTC: from %02X%02X%02X%02X %lum ago, %+ld s (%s) | next ask in %lum", _clk_from[0], _clk_from[1], _clk_from[2],
             _clk_from[3], (unsigned long)ago, (long)d,
             _clk_how == 3 ? "refused: > 1 h" : _clk_how == 1 ? "stepped" : _clk_how == 2 ? "held" : "in step",
             (unsigned long)(_tq_next_ms && (int32_t)(_tq_next_ms - millis()) > 0 ? (_tq_next_ms - millis()) / 60000 : 0));
  }
  return true;
}

bool FwOtaTransfer::cmdWdt(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (strcmp(arg, "clear") == 0) { _wdt_resets = 0; if (_fs) _fs->remove(WDT_FILE); }
#ifdef FW_OTA_DEV
  else if (strcmp(arg, "hang") == 0) {
    _wdt_hang_at = millis() + 3000;
    snprintf(reply, 160, "OK - the loop freezes in 3 s; the watchdog must restart the node within %s ('fw wdt' afterwards: last reset watchdog)",
             g_wdt.runningText());
    return true;
  }
#endif
  else if (*arg) { strcpy(reply, "usage: fw wdt [clear]"); return true; }
  if (g_wdt.draining()) {                                 // (rc10, C27) another firmware's timeout is running out
    char ours[12]; Watchdog::secsText(FW_OTA_WDT_SECS, ours);
    snprintf(reply, 160, "watchdog switching %s -> %s: one planned restart within %s (old timeout running out) | last reset: %s | watchdog resets: %u",
             g_wdt.runningText(), ours, g_wdt.runningText(), g_wdt.lastResetText(), (unsigned)_wdt_resets);
    return true;
  }
  snprintf(reply, 160, "watchdog %s, %s, fed from the main loop | last reset: %s%s | watchdog resets: %u ('fw wdt clear')",
           g_wdt.armed() ? "armed" : "NOT armed", g_wdt.runningText(), g_wdt.lastResetText(), _wdt_planned ? " (planned: timeout change)" : "",
           (unsigned)_wdt_resets);
  return true;
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

// ======================= swarm v1 =======================================
uint8_t FwOtaTransfer::powerState() const {
  if (!_minbatt_mv || !_batt_mv) return 0;               // not configured / not measured: unknown (ranked like OK)
  if (_batt_mv + POWER_CRIT_MV < _minbatt_mv) return 3;
  if (_batt_mv < _minbatt_mv) return 2;
  return 1;
}

uint8_t FwOtaTransfer::activePulls() const {
  const uint32_t now = millis(); uint8_t n = 0;
  for (uint8_t i = 0; i < DUTY_SESS_N; i++) if (_dsess[i].keyed && sessActive(_dsess[i], now)) n++;
  return n;
}

uint32_t FwOtaTransfer::holderScore(const PeerInfo& p) const {
  const uint8_t back = p.has_ret ? p.ret_len : p.route_len;
  const uint8_t relays = rtCount(back) > rtCount(p.route_len) ? rtCount(back) : rtCount(p.route_len);
  return holderRate(askedPct(), p.swarm ? p.sduty : 0, p.swarm ? p.spulls : 0, chunkAirMs(back), relays, p.snr4, p.swarm ? p.spower : 0);
}

// The images of the circle from the peer table (one entry per kind + version + digest; holders of older firmware without a digest count
// by version), newest app first, then bootloaders - owner decision 8.2 (Z to A). best = the holder with the best expected rate.
int FwOtaTransfer::imageList(ImgInfo* out, int cap) const {
  int n = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    const PeerInfo& p = _peers[i];
    if (!p.used || !p.fw_version) continue;
    int k = -1;
    for (int j = 0; j < n && k < 0; j++)
      if (out[j].kind == p.kind && out[j].ver == p.fw_version && (!p.swarm || !out[j].swarm || memcmp(out[j].digest, p.digest, 8) == 0)) k = j;
    if (k < 0) {
      if (n >= cap) continue;
      k = n++; memset(&out[k], 0, sizeof(out[k])); out[k].kind = p.kind; out[k].ver = p.fw_version; out[k].best = -1; out[k].minhops = 0xFF;
    }
    if (p.swarm && !out[k].swarm) { memcpy(out[k].digest, p.digest, 8); out[k].swarm = true; }
    out[k].n++;
    if (rtCount(p.route_len) < out[k].minhops) out[k].minhops = rtCount(p.route_len);
    if (out[k].best < 0 || holderScore(p) > holderScore(_peers[out[k].best])) out[k].best = (int8_t)i;
  }
  for (int a = 0; a < n; a++) for (int b2 = a + 1; b2 < n; b2++) {   // apps before bootloaders, newest first
    const bool sw = out[b2].kind < out[a].kind || (out[b2].kind == out[a].kind && out[b2].ver > out[a].ver);
    if (sw) { ImgInfo t = out[a]; out[a] = out[b2]; out[b2] = t; }
  }
  return n;
}

// the best holder of an image (kind + version + digest) in the peer table, skipping one id and the holders already tried (bit = index in
// _holders); -1 = none usable (unknown, CRITICAL, or all tried)
int FwOtaTransfer::bestHolder(uint8_t kind, uint32_t ver, const uint8_t* digest, const uint8_t* skip_id, uint8_t tried) const {
  int best = -1; uint32_t bs = 0;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    const PeerInfo& p = _peers[i];
    if (!p.used || p.kind != kind || p.fw_version != ver || (p.swarm && digest && memcmp(p.digest, digest, 8) != 0)) continue;
    if (skip_id && memcmp(p.id, skip_id, 4) == 0) continue;
    bool skip = false;
    for (uint8_t h = 0; h < _nholders; h++) if ((tried & (1u << h)) && memcmp(_holders[h], p.id, 4) == 0) skip = true;
    if (skip) continue;
    const uint32_t s = holderScore(p);
    if (s && s > bs) { bs = s; best = i; }
  }
  return best;
}

uint32_t FwOtaTransfer::switchAfterMs() const { return switchAfter(_stall_cph); }   // the rate frozen when the pull stalled

// move a running pull by image to another holder (the existing source switch: the chunks are kept when it offers the same image)
bool FwOtaTransfer::switchToHolder(int idx, const char* why, bool back) {
  if (idx < 0 || idx >= MAX_PEERS || !_peers[idx].used) return false;
  const PeerInfo& p = _peers[idx];
  const bool bl = _man.kind == KIND_BOOTLOADER;
  uint8_t br[sizeof(_switch_prev_route)], bt[sizeof(_switch_prev_ret)];   // (rc20, C48) going back: the route that worked before the move
  const uint8_t brl = _switch_prev_route_len, btl = _switch_prev_ret_len; const bool bts = _switch_prev_ret_set;
  if (back) { memcpy(br, _switch_prev_route, sizeof(br)); memcpy(bt, _switch_prev_ret, sizeof(bt)); }
  memcpy(_switch_prev_route, _route, sizeof(_switch_prev_route)); _switch_prev_route_len = _route_len;
  memcpy(_switch_prev_ret, _ret, sizeof(_switch_prev_ret)); _switch_prev_ret_len = _ret_len; _switch_prev_ret_set = _ret_set;
  if (!_route_manual && back) { memcpy(_route, br, sizeof(br)); _route_len = brl; setRet(bts ? bt : nullptr, btl, false); }
  else if (!_route_manual) {
    if (rtBytes(p.route_len)) memcpy(_route, p.route, rtBytes(p.route_len));
    _route_len = p.route_len; setRet(p.has_ret ? p.ret : nullptr, p.ret_len, false);
  }
  char target[16]; snprintf(target, sizeof(target), "@%02X%02X%02X%02X%s", p.id[0], p.id[1], p.id[2], p.id[3], bl ? "!" : "");
  char tmp[160]; switchSource(target, tmp);
  _switches++; _last_switch_ms = millis() | 1;
  snprintf(_last_err, sizeof(_last_err), "%s: %02X%02X%02X%02X", why, p.id[0], p.id[1], p.id[2], p.id[3]);
  return true;
}

// (rc20, C48) the last holder change, for 'fw route': why (1 holder silent, 2 moved to a faster one, 3 moved back, 4 adopted a nearer holder)
void FwOtaTransfer::noteSwitch(uint8_t why, const uint8_t* id) {
  _sw_why = why; memcpy(_sw_id, id, 4); _sw_ms = millis() | 1;
  if (why != 2) _mv_eval_ms = 0;                         // another change since the move: its saved way back is gone, no judgement
}
uint32_t FwOtaTransfer::rateCph(uint32_t now) const {     // chunks per hour since the pull (re)started at this holder
  const uint32_t ms = since(now, _rate_t0_ms);
  return ms < 60000 ? 0 : (uint32_t)((uint64_t)(_cm.have - _rate_have0) * 3600000ULL / ms);
}

bool FwOtaTransfer::switchToNextHolder(uint32_t now) {
  uint8_t cur[4]; const bool hc = pullTargetId(cur);
  for (uint8_t h = 0; h < _nholders; h++) if (hc && memcmp(_holders[h], cur, 4) == 0) _holders_tried |= (uint8_t)(1u << h);
  int idx = bestHolder(_img_kind, _img_ver, _img_digest, hc ? cur : nullptr, _holders_tried);
  if (idx < 0) { _holders_tried = 0; _wait_started_ms = now; return false; }   // all tried: wait on, start over after the next silence
  noteSwitch(1, _peers[idx].id);
  return switchToHolder(idx, "holder silent - switched to");
}

// (rc15) a holder advert (the merged announce): a session-0 list answer on the circle channel; zero-hop (one transmission, the neighbours
// hear it) or flooded (counts against the flood budget). bl = advertise the bootloader offer instead of the app.
void FwOtaTransfer::sendAnnounce(bool zero_hop, bool bl) {
  if (!_circle_set || !_mesh || (bl ? !_bl_serving : !_serving)) return;
  if (!zero_hop && !floodBudget(millis())) return;
  uint8_t zs[6] = {0}; uint8_t r[LIST_RSP_MAX];
  const uint32_t n = buildListRsp(r, zs, bl ? _bl_man : _serve_man, nullptr, 0);
  uint8_t d[5 + 6 + LIST_RSP_MAX]; uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
  memcpy(d, &ts, 4); d[4] = 0; memcpy(&d[5], FLOOD_ANN, 6); memcpy(&d[11], r, n);
  mesh::Packet* p = _mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _disc_ch, d, 11 + n);
  if (!p) return;
  stats.adverts++;
  const uint32_t delay = 100 + _mesh->getRNG()->nextInt(0, 2000);
  if (zero_hop) { if (!_flood_host || !_flood_host->sendOta(p, nullptr, 0, delay)) _mesh->sendZeroHop(p, delay); }
  else { stats.advert_floods++; floodOut(p, delay); }
}

// (rc15) DEMAND: every circle node notes the pulls it overhears. An offer carries the image tag ("@ID8[!]=TAG8", MAC'd with the circle's
// offer key - only circle members can read it) and the session; requests, chunks and PACE carry the session in clear (liveness); an ABORT ends it.
// (rc15, C35) session ids were millis() << 8 ^ a constant: two nodes that start a pull in the same millisecond (equal uptime - found by the
// simulator; on hardware e.g. after a shared power cut) had the SAME session at the server. Now also our id and the RNG.
uint32_t FwOtaTransfer::newSess(uint32_t salt) const {
  uint32_t s = ((uint32_t)millis() << 8) ^ salt;
  if (_pub) s ^= (uint32_t)_pub[0] << 24 | (uint32_t)_pub[1] << 16 | (uint32_t)_pub[2] << 8 | _pub[3];
  if (_mesh) s ^= (uint32_t)_mesh->getRNG()->nextInt(0, 0x7FFFFFFF);
  return s ? s : 1;
}

void FwOtaTransfer::noteDemand(const uint8_t* b, uint16_t len) {
  if (len < 12 + XFER_MAC_LEN || b[1] != MSG_CHUNK_IMG) return;
  const uint32_t now = millis() | 1;
  uint16_t rs; memcpy(&rs, &b[10], 2);
  const bool mine = (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) && !_listening && (uint16_t)(_seq - rs) <= 2;   // our own window
  if (!mine) { memcpy(_heard_tag, &b[2], 4); _heard_tag_ms = now; }   // another's stream: a puller about to offer listens instead
  int slot = 0;
  for (uint8_t i = 0; i < DEMAND_N; i++) {
    if (memcmp(_dem[i].tag, &b[2], 4) == 0) { slot = i; break; }
    if (since(now, _dem[i].last_ms) > since(now, _dem[slot].last_ms)) slot = i;   // else the stalest
  }
  memcpy(_dem[slot].tag, &b[2], 4); _dem[slot].last_ms = now;
}

uint8_t FwOtaTransfer::demandFor(const uint8_t* digest, bool bl, uint32_t now) const {
  (void)bl; uint8_t n = 0;
  for (uint8_t i = 0; i < DEMAND_N; i++)
    if (_dem[i].last_ms && since(now, _dem[i].last_ms) <= DEMAND_LIVE_MS && memcmp(_dem[i].tag, digest, 4) == 0) n++;
  return n;
}

// (rc15, owner) an IDLE holder that hears a stream of what it holds - served by somebody else - offers itself: one ZERO-HOP advert (rev 3:
// never a flood - the stream passes here, so its pullers or their relays are in range); quiet 30 min, doubling to 2 h while nobody comes.
// Busy (serving a pull) = silent. No demand = silent.
void FwOtaTransfer::advertTick(uint32_t now) {
  if ((int32_t)(now - _adv_next_ms) < 0) return;
  const uint8_t da = _serving ? demandFor(_serve_man.sha256, false, now) : 0;
  const uint8_t db = _bl_serving ? demandFor(_bl_man.sha256, true, now) : 0;
  if ((!da && !db) || activePulls() || powerState() >= 2) {          // no demand, or busy, or a weak battery: stay quiet
    if (!da && !db) { _adv_count = 0; _adv_ivl_ms = 0; }
    _adv_next_ms = now + 10000; return;
  }
  const bool came = _adv_count && (_serve_offer_reqs + _bl_offer_reqs) != _adv_offers_mark;
  if (came) { _adv_count = 0; _adv_ivl_ms = 0; }                     // somebody came after the last advert: a new episode when demand returns
  sendAnnounce(true, !da);                                           // (rc15 rev 3) zero-hop only: the stream it heard passes here
  _adv_ivl_ms = _adv_ivl_ms ? umin(_adv_ivl_ms * 2, ADV_MAX_IVL_MS) : ADV_FIRST_IVL_MS; _adv_next_ms = now + _adv_ivl_ms;
  _adv_offers_mark = _serve_offer_reqs + _bl_offer_reqs; _adv_count++;
}

// (rc15) move a running pull by image to a clearly faster holder: its expected rate (fair share / relay airtime / link / power) >= 1.5x
// ours (our agreed share on our route). A first finding only arms a move after a random 0-3 min; the move happens if it still holds then.
void FwOtaTransfer::moveCheck(uint32_t now, bool arm) {
  if (_listening) { _move_to = -1; return; }             // (rc15) a listener follows the stream; only the requesting puller moves
  if (_phase != PH_DOWNLOADING || !_img_set || (_last_move_ms && since(now, _last_move_ms) < MOVE_HOLD_MS)) { _move_to = -1; return; }
  uint8_t cur[4]; if (!pullTargetId(cur)) { _move_to = -1; return; }
  const int idx = bestHolder(_img_kind, _img_ver, _img_digest, cur, 0);
  if (idx < 0 || (_mv_bar_until && (int32_t)(now - _mv_bar_until) < 0 && memcmp(_peers[idx].id, _mv_bar, 4) == 0)) { _move_to = -1; return; }
  const int ci = peerIdx(cur); const int8_t snr4 = ci >= 0 ? _peers[ci].snr4 : 40;
  const uint8_t back = _ret_set ? _ret_len : _route_len;
  const uint32_t mine = holderRate(askedPct(), _agreed_pct ? _agreed_pct : askedPct(), 0, chunkAirMs(back), rtCount(back), snr4, 1);
  const uint32_t cand = holderScore(_peers[idx]);
  if ((uint64_t)cand * 2 < (uint64_t)mine * 3) { _move_to = -1; return; }   // not 1.5x better
  if (arm) { _move_to = (int8_t)idx; _move_at_ms = now + _mesh->getRNG()->nextInt(0, MOVE_JITTER_MS); return; }
  _move_to = -1; _last_move_ms = now | 1; stats.moves++;
  if (g_tune.move_eval) { _mv_cph = rateCph(now); memcpy(_mv_from, cur, 4); _mv_eval_ms = now + MOVE_EVAL_MS; }
  noteSwitch(2, _peers[idx].id);
  switchToHolder(idx, "moved to a faster holder");
}

// (rc20, C48) 15 min after a move: the new holder measured slower than the one we left (rate below 80 %) -> back to it, and no move to
// the slow one for 2 h. The rate model ranks holders by duty share, relays and SNR; one link that is good one way only (a strong advert,
// a weak way back) made board 2 move to the twin and back within 20 min (rc19 rollout, 08:04-08:25)
void FwOtaTransfer::moveEval(uint32_t now) {
  if (!_mv_eval_ms || (int32_t)(now - _mv_eval_ms) < 0) return;
  _mv_eval_ms = 0;
  uint8_t cur[4];
  if (_phase != PH_DOWNLOADING || !_img_set || !pullTargetId(cur) || (uint64_t)rateCph(now) * 5 >= (uint64_t)_mv_cph * 4) return;
  memcpy(_mv_bar, cur, 4); _mv_bar_until = (now + MOVE_BAR_MS) | 1;
  const int i = peerIdx(_mv_from, _man.kind);
  if (i >= 0) { _last_move_ms = now | 1; noteSwitch(3, _mv_from); switchToHolder(i, "moved back (slower)", true); }
}
int FwOtaTransfer::peerIdx(const uint8_t* id, uint8_t kind) const {   // the (last) peer table entry of that node (and kind), -1 = none
  int k = -1;
  for (uint8_t i = 0; i < MAX_PEERS; i++) if (_peers[i].used && memcmp(_peers[i].id, id, 4) == 0 && (kind == 0xFF || _peers[i].kind == kind)) k = i;
  return k;
}

// (rc15) discovery answers of a holder wait 1.5 s per hop (+ spread); another holder's answer for the SAME session and image counts; at its
// time an answer is dropped when K = 2 were heard already - nearest first, the circle grows only where nobody nearer answered.
void FwOtaTransfer::answerLater(const uint8_t* r, uint32_t n, uint32_t delay_ms) {
  PendAns* p = nullptr;
  for (uint8_t i = 0; i < PANS_N && !p; i++) if (!_pans[i].used) p = &_pans[i];
  if (!p || n > sizeof(p->b) || !(r[36] & KIND_SWARM)) { stats.answers_sent++; floodListRsp(r, n, delay_ms, false); return; }
  memset(p, 0, sizeof(*p)); p->used = true; memcpy(&p->sess, &r[2], 4); memcpy(p->digest, &r[46 + rtBytes(r[45])], 8);
  p->due_ms = millis() + delay_ms; p->len = (uint8_t)n; memcpy(p->b, r, n);
}
void FwOtaTransfer::answerHeard(const uint8_t* b, uint32_t len) {
  if (len < 46 + XFER_MAC_LEN || !(b[36] & KIND_SWARM) || len < 46u + rtBytes(b[45]) + 8 + XFER_MAC_LEN) return;
  uint32_t sess; memcpy(&sess, &b[2], 4);
  for (uint8_t i = 0; i < PANS_N; i++)
    if (_pans[i].used && _pans[i].sess == sess && memcmp(_pans[i].digest, &b[46 + rtBytes(b[45])], 8) == 0 && _pans[i].heard < 255) _pans[i].heard++;
}
void FwOtaTransfer::pendAnsTick(uint32_t now) {
  for (uint8_t i = 0; i < PANS_N; i++) {
    PendAns& p = _pans[i];
    if (!p.used || (int32_t)(now - p.due_ms) < 0) continue;
    p.used = false;
    if (p.heard >= ANS_SUPPRESS_K) { stats.answers_suppressed++; continue; }
    stats.answers_sent++; floodListRsp(p.b, p.len, 0, false);
  }
}

// ======================= (rc17) fleet fill + gap repair (SWARM-DESIGN 9.2 / 9.3) =======================
// MSG_FLEET_ANN: [0]magic [1]type [2..5]tag [6]mode [7]kind [8..11]fw_version [12..15]payload_len [16..19]raw_len [20..23]payload_crc32
// [24..27]raw_crc32 [28..29]compression [30..31]n_chunks [32..63]sha256 [64..68]lzma props [69..76]board [77..80]server id [81..82]seq
// [83..84]the requester's chunks (= n_chunks: its stream is done) [85..88]the requester's id [mac]
static const uint32_t FANN_LEN = 89 + XFER_MAC_LEN;
static const uint32_t FANN_ALIVE_MS = 3 * FANN_IVL_MS + 60000;   // a requester not heard for 16 min (3 announces lost) has stopped or died ...
static const uint32_t STREAM_DEAD_MS = 180000;                   // ... and so has a stream with no chunk of it heard for 3 min
static const uint16_t GAP_BEHIND = 16;                           // (rc19, C44) gaps this far behind the stream's top are its own: asked for

// The requester tells the nodes along its path which image its stream carries (and how to place and check it): sent along its route like its
// requests, so every node near a hop hears it; the stream's server echoes it once zero-hop for the nodes around itself. The MAC is the image
// key's (circle + image digest): only circle members can make or use one.
// (rc19) one builder for every FLEET_ANN: the requester's announce, a holder's announce and the requester's cancel
__attribute__((noinline)) uint32_t FwOtaTransfer::buildFann(uint8_t* b, const XferManifest& m, uint8_t mode, const uint8_t* server, uint16_t have, const uint8_t* req) {
  uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_FLEET_ANN; memcpy(&b[n], m.sha256, 4); n += 4;
  b[n++] = mode; b[n++] = m.kind;
  memcpy(&b[n], &m.fw_version, 4); n += 4; memcpy(&b[n], &m.payload_len, 4); n += 4; memcpy(&b[n], &m.raw_len, 4); n += 4;
  memcpy(&b[n], &m.payload_crc32, 4); n += 4; memcpy(&b[n], &m.raw_crc32, 4); n += 4; memcpy(&b[n], &m.compression, 2); n += 2;
  const uint16_t nc = (uint16_t)m.n_chunks; memcpy(&b[n], &nc, 2); n += 2;
  memcpy(&b[n], m.sha256, 32); n += 32; memcpy(&b[n], m.lzma_props, 5); n += 5;
  memcpy(&b[n], g_fwpull_tag.board, 8); n += 8; memcpy(&b[n], server, 4); n += 4;
  _fann_seq++; memcpy(&b[n], &_fann_seq, 2); n += 2; memcpy(&b[n], &have, 2); n += 2; memcpy(&b[n], req, 4); n += 4;
  n += XFER_MAC_LEN;
  uint8_t k[32]; imgKey(m.sha256, k); macFillKey(b, n, k); memset(k, 0, sizeof(k));
  return n;
}

void FwOtaTransfer::sendFleetAnn() {
  uint8_t sid[4]; if (!pullTargetId(sid)) return;
  uint8_t b[FANN_LEN];
  const uint32_t n = buildFann(b, _man, _fill ? _fill_mode : _fleet_mode, sid, (uint16_t)_cm.have, _pub);
  stats.fleet_anns++;
  send(b, n, 60, _route, _route_len);
}

// May this node collect that image now? (owner 9.1/9.2) Never while it pulls or installs, never an older or equal image, never over a slot
// waiting for its apply; mode 1: an EMPTY slot only; mode 2 (fleet): an older slot image too, unless it is serving a pull right now. A
// collector stuck with part of an older fill takes a newer one. CRITICAL battery and 'fw serve off' (the opt-out) never collect.
bool FwOtaTransfer::fillAllowed(const XferManifest& m, uint8_t mode) {
  if (!_circle_set || _serve_disabled || powerState() == 3 || m.kind != KIND_APP || _staging || _resolving || _reset_at_ms || _apply_armed) return false;
  if (_phase == PH_OFFERING || _phase == PH_VERIFYING || _paused) return false;
  if (_phase == PH_DOWNLOADING) return _fill && !_sess_set && !_listening && !_gap_phase && m.fw_version > _man.fw_version;   // a stalled older fill
  CtrlBlock cb; if (readCtrl(cb) && cb.apply_state == APPLY_PENDING) return false;
  SlotHeader h;
  if (readSlotHeader(h) && slotValid()) {
    if (h.fw_version >= m.fw_version) return false;
    if (mode != FLEET_REPLACE || activePulls()) return false;
  }
  return true;
}

void FwOtaTransfer::onFleetAnn(const uint8_t* b, uint32_t len) {
  if (len != FANN_LEN || memcmp(&b[2], &b[32], 4) != 0 || memcmp(&b[69], g_fwpull_tag.board, 8) != 0) return;
  const bool hann = (b[6] & FLEET_HOLDER) != 0;           // (study src_spread) a holder's zero-hop announce (requester field = origin)
  const uint8_t mode = b[6] & (uint8_t)~(FLEET_HOLDER | FLEET_CANCEL);
  if (mode != FLEET_FILL_EMPTY && mode != FLEET_REPLACE) return;
  uint8_t k[32]; imgKey(&b[32], k);
  const bool ok = macOk(k, b, len); memset(k, 0, sizeof(k));
  if (!ok) return;
  if (b[6] & FLEET_CANCEL) { onFleetCancel(b, len); return; }   // (rc19, C42)
  uint16_t seq; memcpy(&seq, &b[81], 2);
  uint16_t rhave; memcpy(&rhave, &b[83], 2);
  if (_serving && memcmp(_serve_man.sha256, &b[32], 32) == 0) {   // the stream's server (or another holder): echo it once zero-hop
    if (g_tune.algo == ALGO_E && rhave >= _serve_man.n_chunks) _tr_c++;   // (study E) another holder's announce: consistent
    if (memcmp(_fecho_tag, &b[2], 4) != 0 || _fecho_seq != seq) {
      memcpy(_fecho_tag, &b[2], 4); _fecho_seq = seq;
      if (memcmp(&b[77], _pub, 4) == 0) send(b, len, 300 + _mesh->getRNG()->nextInt(0, 700), nullptr, 0);
    }
    if (mode == FLEET_REPLACE && memcmp(&b[77], _pub, 4) == 0) spreadStart(&b[32]);   // (rc18) the server of a fleet stream gossips the image
    return;
  }
  XferManifest m; memset(&m, 0, sizeof(m));
  m.kind = b[7]; memcpy(&m.fw_version, &b[8], 4); memcpy(&m.payload_len, &b[12], 4); memcpy(&m.raw_len, &b[16], 4);
  memcpy(&m.payload_crc32, &b[20], 4); memcpy(&m.raw_crc32, &b[24], 4); memcpy(&m.compression, &b[28], 2);
  uint16_t nc; memcpy(&nc, &b[30], 2); m.n_chunks = nc; m.chunk_size = XFER_CHUNK;
  memcpy(m.sha256, &b[32], 32); memcpy(m.lzma_props, &b[64], 5);
  if (!m.payload_len || m.payload_len > SLOT_PAYLOAD_MAX || m.n_chunks != (m.payload_len + XFER_CHUNK - 1) / XFER_CHUNK || m.n_chunks > ChunkMap::MAX_CHUNKS) return;
  if (_phase == PH_DOWNLOADING && memcmp(_man.sha256, m.sha256, 32) == 0) {   // we are on it already: its requester is alive (and how far)
    if (memcmp(&b[85], _pub, 4) == 0) return;            //   (our own announce, repeated by a relay: we are the requester)
    // (rc19, C44) only the REQUESTER's announce tells how far its stream is: a holder's announce (have = all chunks; the fleet's source sends
    // them from the first offer) made rc18's listeners think the stream was done - they left it and asked for the chunks ahead of it
    // (C42) whose stream we follow, its mode. Never a LOWER mode: a neighbour that went on to pull the image along its path announces mode 1,
    // and that made a fleet fill passive (it paused when that pull ended: T3/T4 sims). Only the fleet's cancel makes it passive
    if (!hann && (!_fill || mode >= _fill_mode)) { _fann_heard_ms = millis() | 1; _fann_have = rhave; memcpy(_fann_req, &b[85], 4); if (_fill) _fill_mode = mode; }
    if (g_tune.algo == ALGO_E && _gap_phase && rhave > _cm.have) {   // (study E) a holder near us: ask it now
      _gap_dry = 0; _e_wait = false; _e_prog_ms = millis() | 1; _gapr_at_ms = millis() + (uint32_t)_mesh->getRNG()->nextInt(0, GAP_SPREAD_MS);
    }
    return;
  }
  if (hann && holderSwitch(m, &b[77], &b[85])) return;
  if (!fillAllowed(m, mode)) return;
  snprintf(m.name, sizeof(m.name), "@%02X%02X%02X%02X", b[77], b[78], b[79], b[80]);   // the stream's server: the path for gaps nobody near holds
  const bool fresh = !_fc.set || memcmp(_fc.m.sha256, m.sha256, 32) != 0;
  _fc.set = true; _fc.m = m; memcpy(_fc.server, &b[77], 4); _fc.mode = mode; _fc.have = rhave; if (fresh) _fc.ms = millis() | 1;
  memcpy(_fc.origin, hann ? &b[85] : &b[77], 4); memcpy(_fc.req, &b[85], 4);
  const uint32_t now = millis();                          // its stream is on the air already: start now (else at its next chunk)
  if (_heard_tag_ms && since(now, _heard_tag_ms) < STREAM_T_MIN_MS && memcmp(_heard_tag, m.sha256, 4) == 0) startFill();
  else if (g_tune.algo == ALGO_E && g_tune.e_spread && rhave >= m.n_chunks && (hann || memcmp(&b[77], &b[85], 4) == 0)) startFill();   // (study E) a holder:
}                                                         //   no stream will come - fill from it by gap rounds

// Collect the announced stream: the slot is cleared (its image stops being served), the payload area erased, and every chunk of the image
// heard on any hop is stored - with no session: a session is made only if gaps have to come along the path.
void FwOtaTransfer::startFill() {
  if (!_fc.set) return;
  const uint32_t now = millis();
  stopServing(); _apply_armed = false;
  _man = _fc.m; _saved_man = _fc.m; _man.dst_off = 0;
  _img_set = true; _img_kind = KIND_APP; _img_ver = _man.fw_version; memcpy(_img_digest, _man.sha256, 8);
  memcpy(_holders[0], _fc.server, 4); _nholders = 1; _holders_tried = 0;
  memcpy(_origin, _fc.origin, 4); memcpy(_origin_tag, _man.sha256, 4);
  _fill = true; _fill_mode = _fc.mode; _fc.set = false; stats.fills++; memcpy(_fann_req, _fc.req, 4); _fill_paused = false; _strm_top = 0; _fann_heard_ms = now | 1; _fann_have = _fc.have; _img_heard_ms = now | 1;
  useImgKeys(_man.sha256);
  eraseSlotHeader();
  _cm.reset(_man.n_chunks); _erased = false; _erase_next = 0;
  _session = newSess(0xF11F); _sess_set = false; _prev_session = 0; _seq = 0; _agreed_pct = 0; _img_ok = true; _fann_next_ms = 0;
  _route_len = 0; _route_manual = false; setRet(nullptr, 0, false); routeReset();
  _resuming = false; _retries = 0; _win_count = 0; _crc_bad = 0; _crc_bad_idx = -1; _mac_fail = 0; _verify_retries = 0;
  _listening = true; _stream_ms = now | 1; _stream_gap_ms = 0; _gap_at_ms = 0; _gap_phase = false;
  _rate_t0_ms = now; _rate_have0 = 0; _last_chunk_ms = 0; _last_switch_ms = 0; _next_disc_ms = now + DISC_DOWNLOAD_MAX_MS;
  _phase = PH_DOWNLOADING;
  strcpy(_last_err, _fill_mode == FLEET_FILL_EMPTY ? "passive fill: saving a pull's stream" : "fleet fill: saving the stream");   // (rc20, C52)
  saveState();
}

// (rc19, C42, owner) a fleet pull's requester aborted it: every node of that fleet stops. The requester repeats the cancel (cxlTick), the
// stream's server echoes it zero-hop (onFleetAnn), and every node it reaches passes it on ONCE zero-hop - also one that missed every
// repeat hears it from a neighbour. A fill that follows THIS requester's stream pauses (chunks kept, it becomes a passive fill: a later
// stream of the same image continues it, a newer one replaces it, 'fw pull-resume' finishes it); a node that already holds the image keeps
// it and keeps serving it (owner) but stops spreading it. Another requester's fleet pull of the same image is not touched.
void FwOtaTransfer::onFleetCancel(const uint8_t* b, uint32_t len) {
  if (_serving && memcmp(_serve_man.sha256, &b[32], 32) == 0 && memcmp(&b[77], _pub, 4) == 0) {   // the stream's server: echo every repeat
    uint16_t seq; memcpy(&seq, &b[81], 2);
    if (memcmp(_fecho_tag, &b[2], 4) != 0 || _fecho_seq != seq) {
      memcpy(_fecho_tag, &b[2], 4); _fecho_seq = seq; send(b, len, 300 + _mesh->getRNG()->nextInt(0, 700), nullptr, 0);
    }
    if (memcmp(_spread_tag, &b[2], 4) == 0) { memset(_spread_tag, 0, 4); _tr_inc_ms = 0; }   // it stops spreading the image (keeps serving it)
    return;
  }
  if (memcmp(_cxl_seen, &b[2], 4) == 0 && memcmp(_cxl_seen + 4, &b[85], 4) == 0) return;   // handled already (a repeat)
  bool hit = false;
  if (_fc.set && memcmp(_fc.m.sha256, &b[32], 32) == 0) { _fc.set = false; hit = true; }
  if (memcmp(_spread_tag, &b[2], 4) == 0) { memset(_spread_tag, 0, 4); _tr_inc_ms = 0; hit = true; }
  if (_fill && (_phase == PH_OFFERING || _phase == PH_DOWNLOADING) && memcmp(_man.sha256, &b[32], 32) == 0 && memcmp(_fann_req, &b[85], 4) == 0) {
    if (_sess_set) sendAbort();                          //   a collector that already went along the path frees its server
    _fill_mode = FLEET_FILL_EMPTY;                        //   a cancelled fleet fill is a passive fill from here (also after a reboot)
    if (_erased) { _phase = PH_DOWNLOADING; _resuming = false; _sess_set = false; saveState(); pauseFill("fleet pull cancelled by its requester"); }
    else { _phase = PH_IDLE; _fill = false; clearState(); }   //   nothing collected yet
    hit = true;
  }
  if (!hit) return;
  memcpy(_cxl_seen, &b[2], 4); memcpy(_cxl_seen + 4, &b[85], 4);
  send(b, len, 500 + _mesh->getRNG()->nextInt(0, 2000), nullptr, 0);   // passed on once, zero-hop
}

// the requester of a fleet pull that 'fw pull-abort' stopped: the cancel goes out now and again after 2, 10 and 30 min (a requester that
// only went quiet looks the same to its listeners - owner: the cancel must not get lost), along its route like its announces
void FwOtaTransfer::cxlTick(uint32_t now) {
  if (!_cxl_n || (int32_t)(now - _cxl_at_ms) < 0) return;
  static const uint32_t AFTER_MS[] = { 0, 1200000, 480000, 120000 };   // the wait before the next one, by the repeats left (0 .. 3)
  _cxl_n--; _cxl_at_ms = now + AFTER_MS[_cxl_n];
  _fann_seq++; memcpy(&_cxl[81], &_fann_seq, 2);          // a new seq: relays and the server's echo do not drop it as a duplicate
  uint8_t k[32]; imgKey(&_cxl[32], k); macFillKey(_cxl, FANN_LEN, k); memset(k, 0, sizeof(k));
  send(_cxl, FANN_LEN, 100, _cxl_route, _cxl_rlen);
}

void FwOtaTransfer::pauseFill(const char* why) {
  _fill_paused = true; _listening = false; _gap_phase = false; _stream_ms = 0; _gap_at_ms = 0; _e_wait = false; _el_phase = 0;
  strncpy(_last_err, why, sizeof(_last_err) - 1); _last_err[sizeof(_last_err) - 1] = 0;
}

// (rc19, C42, owner) a plain pull's listeners help only while its requester still pulls: they fall quiet an hour after its last announce
static const uint32_t PASSIVE_QUIET_MS = 3600000;
bool FwOtaTransfer::reqHeard(uint32_t now) const { return _fann_heard_ms && since(now, _fann_heard_ms) < PASSIVE_QUIET_MS; }

// The stream we collect (or listen to) is still being pulled: its requester announced lately and was not done. Gaps wait until then - a
// stream that only pauses (windows through relays, a retry) must not turn every collector into a second requester at the server.
bool FwOtaTransfer::streamStillPulled(uint32_t now) const {
  return _fann_heard_ms && since(now, _fann_heard_ms) < FANN_ALIVE_MS && _fann_have < _man.n_chunks
      && _img_heard_ms && since(now, _img_heard_ms) < STREAM_DEAD_MS
      && (!g_tune.newq_ms || !_last_chunk_ms || since(now, _last_chunk_ms) < g_tune.newq_ms);
}

// One gap round: ask the neighbours for up to GAP_REP_MAX missing chunks (those another node asked for lately are left out - their repair
// reaches us too). Rounds that brought nothing count; after GAP_DRY_MAX of them the rest goes along the path.
void FwOtaTransfer::gapTick(uint32_t now) {
  if ((int32_t)(now - _gapr_at_ms) < 0) return;
  const bool m1 = _fill && _fill_mode == FLEET_FILL_EMPTY && !_sess_set;
  if (m1 && !reqHeard(now)) { pauseFill("passive fill paused"); return; }   // (rc19, C42) its puller went quiet an hour ago
  // (rc19, C44) while the requester still pulls, only chunks BEHIND its stream are asked for: what lies ahead comes with the stream. rc18 asked
  // for the first missing chunks - ahead of the stream for a listener that kept up, so the server repaired the stream in advance (test B:
  // 739 repairs for 1766 chunks). Nothing behind it = wait, not a dry round (no escalation while the requester pulls)
  // (rc20 study: C50 - gaps behind the stream for 30 min of requester silence - and C53 - its stream as a sign of life - cut the bench's
  // repairs but made the valley 67 % slower with 14 % more air: the listeners' early repairs are what spreads the image hop by hop. Not built)
  const bool live = _fann_heard_ms && since(now, _fann_heard_ms) < FANN_ALIVE_MS && _fann_have < _man.n_chunks;
  const uint16_t lim = !live ? (uint16_t)_cm.n : _strm_top > GAP_BEHIND ? (uint16_t)(_strm_top - GAP_BEHIND) : 0;
  if (_cm.firstMissing() >= lim) { _gap_round = 0; _gap_dry = 0; _gapr_at_ms = now + g_tune.gap_round_ms; return; }
  if (_gap_round) { if (_cm.have == _gap_have0) _gap_dry++; else { _gap_dry = 0; _e_prog_ms = now | 1; } }
  _gap_have0 = _cm.have;
  if (_gap_dry >= g_tune.gap_dry_max) {
    if (m1) { _gapr_at_ms = now + 60000; return; }        // (rc19, C42) a plain pull's listener never pulls along the path: it waits for repairs
    if (g_tune.algo == ALGO_D && _fill && !_sess_set) { _gap_phase = false; enterElect(now); return; }   // (study D) the inventory next
    if (g_tune.algo == ALGO_E && !_sess_set && since(now, _e_prog_ms) < g_tune.e_fallback_ms) {         // (study E) wait for a neighbour's
      if (!_e_wait) { _e_wait = true; strcpy(_last_err, "gaps: waiting for a neighbour that has more"); }   //   summary with more chunks
      _gapr_at_ms = now + 60000; return;
    }
    gapEscalate(now); return;
  }
  const bool cov = _gap_cov_ms && since(now, _gap_cov_ms) < GAP_COVER_MS;
  uint8_t blk[GAP_MAX_BLKS]; uint8_t map[GAP_MAX_BLKS][8]; uint8_t nb = 0, cnt = 0;
  memset(map, 0, sizeof(map));
  const uint8_t gmax = g_tune.gap_max > GAP_REP_CAP ? GAP_REP_CAP : g_tune.gap_max;
  for (uint16_t i = _cm.firstMissing(); i < lim && cnt < gmax; i = _cm.firstMissing(i + 1)) {
    if (cov && ((_gap_cov[i >> 3] >> (i & 7)) & 1)) continue;
    const uint8_t bi = (uint8_t)(i / GAP_BLK_CHUNKS); int k = -1;
    for (uint8_t j = 0; j < nb; j++) if (blk[j] == bi) k = j;
    if (k < 0) { if (nb >= GAP_MAX_BLKS) break; k = nb; blk[nb++] = bi; }
    map[k][(i % GAP_BLK_CHUNKS) >> 3] |= (uint8_t)(1u << (i & 7)); cnt++;
  }
  _gap_round++;
  const uint32_t jit = (uint32_t)_mesh->getRNG()->nextInt(0, GAP_SPREAD_MS);
  if (!cnt) { stats.gap_supp++; _gapr_at_ms = now + g_tune.gap_round_ms + jit; return; }   // all ours were asked by others: their repairs serve us
  uint8_t b[9 + GAP_MAX_BLKS * 9 + XFER_MAC_LEN]; uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_GAP_REQ; memcpy(&b[n], _man.sha256, 4); n += 4;
  _gap_seq++; memcpy(&b[n], &_gap_seq, 2); n += 2;
  b[n++] = nb;
  for (uint8_t j = 0; j < nb; j++) { b[n++] = blk[j]; memcpy(&b[n], map[j], 8); n += 8; }
  n += XFER_MAC_LEN; macFillKey(b, n, _ik_mac);
  send(b, n, 50, nullptr, 0); stats.gap_reqs++;
  _gapr_at_ms = now + g_tune.gap_round_ms + (uint32_t)cnt * 2000 + jit;   // the repairs come paced (~2 s each at duty 1.0)
  snprintf(_last_err, sizeof(_last_err), "gaps: asking the neighbours (round %u)", (unsigned)_gap_round);
}

// Nobody near holds what is still missing: it was lost upstream. A puller with a session asks its server along the path (as before rc17); a
// collector becomes a pull by image of the stream's server (neighbours first, then a flood) - its path stream feeds everyone along it too.
void FwOtaTransfer::gapEscalate(uint32_t now) {
  _gap_phase = false; stats.escalations++;
  if (_sess_set) { strcpy(_last_err, "gaps: from the server (path)"); _retries = 0; requestWindow(); return; }
  enterWait("gaps: asking the stream's server (path)");
  _next_disc_ms = now;                                   // the holder search right away (a collector knows no route to the server) ...
  _esc_pick_ms = now + 20000;                            // ... then the nearest holder that answered (a filled node 1 relay away beats the
}                                                        //     stream's server 3 relays away: fewer relays = less airtime and energy)

// Does this node hold chunk idx of the image with that tag? full = from a served (complete) slot image, else from a pull / fill in progress
bool FwOtaTransfer::holdsChunk(const uint8_t* tag, uint16_t idx, bool* full) const {
  if (_serving && memcmp(_serve_man.sha256, tag, 4) == 0) { *full = true; return idx < _serve_man.n_chunks; }
  if ((_phase == PH_DOWNLOADING || _phase == PH_VERIFYING || _phase == PH_COMPLETE) && _erased && _man.kind == KIND_APP
      && memcmp(_man.sha256, tag, 4) == 0 && idx < _cm.n && _cm.has(idx)) { *full = false; return true; }
  return false;
}

// A neighbour's GAP_REQ: (a) we miss the same chunks -> we do not ask for them ourselves for a while; (b) we hold some -> repair them after a
// delay by link quality (the best link first; LOW battery last, CRITICAL never), each cancelled when another node is heard sending it
void FwOtaTransfer::onGapReq(const uint8_t* b, uint32_t len, int8_t snr4) {
  if (len < 9 + XFER_MAC_LEN) return;
  const uint8_t nb = b[8];
  if (!nb || nb > GAP_MAX_BLKS || len != 9u + nb * 9u + XFER_MAC_LEN) return;
  const uint8_t* key = nullptr;
  if (_serving && memcmp(_serve_man.sha256, &b[2], 4) == 0) key = imageKeys(_serve_man, 0);
  else if ((_phase == PH_DOWNLOADING || _phase == PH_VERIFYING || _phase == PH_COMPLETE) && _man.kind == KIND_APP && memcmp(_man.sha256, &b[2], 4) == 0) key = _ik_mac;
  if (!key || !macOk(key, b, len)) return;
  const uint32_t now = millis();
  const bool mine = _phase == PH_DOWNLOADING && memcmp(_man.sha256, &b[2], 4) == 0;
  if (mine && (!_gap_cov_ms || since(now, _gap_cov_ms) >= GAP_COVER_MS)) { memset(_gap_cov, 0, sizeof(_gap_cov)); }
  uint16_t want[GAP_REP_CAP]; uint8_t nw = 0;
  for (uint8_t j = 0; j < nb; j++) {
    const uint8_t* e = &b[9 + j * 9];
    for (uint8_t c = 0; c < GAP_BLK_CHUNKS; c++) {
      if (!((e[1 + (c >> 3)] >> (c & 7)) & 1)) continue;
      const uint16_t idx = (uint16_t)(e[0] * GAP_BLK_CHUNKS + c);
      if (mine && idx < ChunkMap::MAX_CHUNKS) _gap_cov[idx >> 3] |= (uint8_t)(1u << (idx & 7));
      bool full; if (nw < repCap() && holdsChunk(&b[2], idx, &full)) want[nw++] = idx;
    }
  }
  if (mine) _gap_cov_ms = now | 1;
  if (!nw || powerState() == 3) return;
  if (g_tune.algo == ALGO_E && _serving) { _tr_inc_ms = now | 1; memcpy(_tr_tag, &b[2], 4); }   // (study E) a neighbour misses: gossip
  const bool same = _rep.i < _rep.n && memcmp(_rep.tag, &b[2], 4) == 0;
  if (_rep.i < _rep.n && !same) return;                  // busy repairing another image
  if (!same) {
    memset(&_rep, 0, sizeof(_rep)); memcpy(_rep.tag, &b[2], 4);
    const int32_t s = snr4 == 127 ? 0 : (snr4 < -40 ? -40 : (snr4 > 40 ? 40 : snr4));
    _rep.due = now + 300 + (uint32_t)(40 - s) * g_tune.rep_snr_ms + (uint32_t)_mesh->getRNG()->nextInt(0, 800) + (powerState() == 2 ? 5000 : 0);
    const uint32_t sp = dutySpacingMs(chunkAirMs(0), chanPct(_duty_pct), 0);
    _rep.sp = sp > chunkAirMs(0) ? sp : chunkAirMs(0) + CHUNK_GAP_MS;
  }
  for (uint8_t k = 0; k < nw && _rep.n < repCap(); k++) {
    bool dup = false; for (uint8_t q = _rep.i; q < _rep.n; q++) if (_rep.idx[q] == want[k]) dup = true;
    if (!dup) _rep.idx[_rep.n++] = want[k];
  }
}

void FwOtaTransfer::repHeard(const uint8_t* tag, uint16_t idx) {
  if (_rep.i >= _rep.n || memcmp(_rep.tag, tag, 4) != 0) return;
  if (!_rep.sent) { stats.repairs_supp += _rep.n - _rep.i; _rep.i = _rep.n; return; }   // a better link started first: leave it to that node
  for (uint8_t q = _rep.i; q < _rep.n; q++) if (_rep.idx[q] == idx) { _rep.idx[q] = 0xFFFF; stats.repairs_supp++; }
}

// one repair at a time, in a quiet moment, at our duty: a zero-hop image chunk (req seq 0xFxxx marks it as a repair)
void FwOtaTransfer::repTick(uint32_t now) {
  while (_rep.i < _rep.n && _rep.idx[_rep.i] == 0xFFFF) _rep.i++;
  if (_rep.i >= _rep.n || (int32_t)(now - _rep.due) < 0) return;
  if (!otaQuiet(now, _last_foreign_ms, _rep.wait0)) { if (!_rep.wait0) _rep.wait0 = now | 1; _rep.due = now + 200; return; }
  _rep.wait0 = 0;
  const uint16_t idx = _rep.idx[_rep.i++];
  bool full; if (!holdsChunk(_rep.tag, idx, &full)) return;
  const XferManifest& m = full ? _serve_man : _man;
  const uint8_t* ik = full ? imageKeys(_serve_man, 0) : nullptr;
  uint8_t kk[32]; if (!ik) { memcpy(kk, _ik_mac, 16); memcpy(kk + 16, _ik_enc, 16); ik = kk; }
  const uint32_t off = (uint32_t)idx * XFER_CHUNK, clen = umin(m.payload_len - off, XFER_CHUNK);
  uint8_t r[2 + 4 + 2 + 2 + 2 + XFER_CHUNK + 2 + XFER_MAC_LEN]; uint32_t n = 0;
  uint32_t tag; memcpy(&tag, m.sha256, 4);
  r[n++] = XFER_MAGIC; r[n++] = MSG_CHUNK_IMG; memcpy(&r[n], &tag, 4); n += 4; memcpy(&r[n], &idx, 2); n += 2;
  const uint16_t wl = (uint16_t)clen; memcpy(&r[n], &wl, 2); n += 2;
  const uint16_t rs = (uint16_t)(0xF000 | (++_rep.seq & 0x0FFF)); memcpy(&r[n], &rs, 2); n += 2;
  flashRead(&r[n], SLOT_ADDR + SLOT_PAYLOAD_OFFSET + (full ? 0 : _man.dst_off) + off, clen); n += clen;
  const uint16_t cc = crc16(&r[n - clen], clen); memcpy(&r[n], &cc, 2); n += 2;
  ctrXor(ik + 16, tag, idx, &r[n - 2 - clen], clen);
  n += XFER_MAC_LEN; macFillKey(r, n, ik);
  memset(kk, 0, sizeof(kk));
  send(r, n, 0, nullptr, 0); stats.repairs++; if (_rep.sent < 255) _rep.sent++;
  _rep.due = now + _rep.sp + _mesh->getRNG()->nextInt(0, 60);
}

// "fw minbatt" - show; "fw minbatt 3400" - set (mV); "fw minbatt off". Below it this node is a LOW holder (pullers take it last), 150 mV
// below CRITICAL (it stops serving). LiPo and LiFePO4 differ - set it per node (e.g. 3500 LiPo, 3150 LiFePO4).
bool FwOtaTransfer::cmdMinBatt(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (*arg) {
    long v = strcmp(arg, "off") == 0 ? 0 : strtol(arg, nullptr, 10);
    if (v && (v < 2500 || v > 4500)) { strcpy(reply, "usage: fw minbatt <2500-4500 mV>|off - below = LOW holder (taken last), 150 mV below = CRITICAL (no serving)"); return true; }
    _minbatt_mv = (uint16_t)v;
    if (_fs) { _fs->remove(MINBATT_FILE); if (v) { File f = _fs->open(MINBATT_FILE, FILE_O_WRITE); if (f) { f.write((const uint8_t*)&_minbatt_mv, 2); f.close(); } } }
  }
  static const char* const PS[] = { "unknown", "OK", "LOW", "CRITICAL" };
  char bv[24]; if (_batt_mv) snprintf(bv, sizeof(bv), "%u mV", (unsigned)_batt_mv); else strcpy(bv, "not measured yet");
  if (!_minbatt_mv) snprintf(reply, 160, "minbatt off (power state unknown = ranked like OK) | battery %s", bv);
  else snprintf(reply, 160, "minbatt %u mV | battery %s: %s (LOW = taken last as a holder, CRITICAL = no serving below %u mV)",
                (unsigned)_minbatt_mv, bv, PS[powerState()], (unsigned)(_minbatt_mv - POWER_CRIT_MV));
  return true;
}

// ======================= (algorithm study, owner 2026-10-08) C/D election, E trickle =======================
// C/D: when the stream ends (C) or the local repair ran dry (D), every collector still missing chunks waits have/n x elect_span (fewest
// chunks first; LOW battery after all OK nodes), then claims. A claim heard from a node missing at least as much defers us: we listen to
// its stream. No better claim within elect_settle after ours: we pull (by image, from the nearest holder) and announce like a requester.
#ifdef FW_OTA_STUDY   // (rc18) the election is simulator-only (the study's C/D); the firmware runs G and ignores claims
static const uint32_t CLAIM_LEN = 2 + 4 + 2 + 4 + 1 + XFER_MAC_LEN;
void FwOtaTransfer::enterElect(uint32_t now) {
  _gap_phase = false;
  if (_el_rounds >= g_tune.elect_rounds) { _el_phase = 0; strcpy(_last_err, "partial: waiting for a manual wave"); return; }
  _el_phase = 1;
  const uint32_t span = g_tune.elect_span_ms;
  _el_at_ms = now + 3000 + (uint32_t)((uint64_t)_cm.have * span / (_cm.n ? _cm.n : 1)) + (uint32_t)_mesh->getRNG()->nextInt(0, 4000)
            + (powerState() == 2 ? span : 0);
  strcpy(_last_err, "gaps: inventory (fewest chunks pulls next)");
}

void FwOtaTransfer::sendClaim() {
  uint8_t b[CLAIM_LEN]; uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_CLAIM; memcpy(&b[n], _man.sha256, 4); n += 4;
  const uint16_t hv = _cm.have; memcpy(&b[n], &hv, 2); n += 2; memcpy(&b[n], _pub, 4); n += 4; b[n++] = _el_rounds;
  n += XFER_MAC_LEN; macFillKey(b, n, _ik_mac);
  stats.claims++;
  if (g_tune.claim_flood && floodBudget(millis())) {
    uint8_t d[5 + 6 + CLAIM_LEN]; uint32_t ts = _mesh->getRTCClock()->getCurrentTime();
    memcpy(d, &ts, 4); d[4] = 0; memcpy(&d[5], FLOOD_CLAIM, 6); memcpy(&d[11], b, n);
    mesh::Packet* p = _mesh->createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, _disc_ch, d, 11 + n);
    if (p) { stats.claim_floods++; floodOut(p, 50); return; }
  }
  send(b, n, 50, nullptr, 0);
}

void FwOtaTransfer::onClaim(const uint8_t* b, uint32_t len) {
  if (len < CLAIM_LEN || b[0] != XFER_MAGIC || b[1] != MSG_CLAIM) return;
  if (_phase != PH_DOWNLOADING || !_erased || memcmp(&b[2], _man.sha256, 4) != 0 || !macOk(_ik_mac, b, CLAIM_LEN)) return;
  if (memcmp(&b[8], _pub, 4) == 0) return;               // our own, relayed back
  uint16_t hv; memcpy(&hv, &b[6], 2);
  const bool better = hv < _cm.have || (hv == _cm.have && memcmp(&b[8], _pub, 4) < 0);
  const uint32_t now = millis();
  if ((_el_phase == 1 || _el_phase == 2) && better) {   // it misses at least as much: it pulls, we listen
    if (_el_phase == 1) stats.claims_supp++;
    _el_phase = 3; _el_rounds++; _el_at_ms = now + g_tune.elect_defer_ms;
    strcpy(_last_err, "gaps: another node pulls next, listening");
  } else if (_el_phase == 3) _el_at_ms = now + g_tune.elect_defer_ms;   // the round goes on
}

void FwOtaTransfer::electTick(uint32_t now) {
  if ((int32_t)(now - _el_at_ms) < 0) return;
  if (_el_phase == 1) { sendClaim(); _el_phase = 2; _el_at_ms = now + g_tune.elect_settle_ms; return; }
  if (_el_phase == 2) {                                  // nobody missing more: we are the next puller
    _el_phase = 0; _el_rounds++; _el_puller = true; stats.elected++;
    gapEscalate(now); return;
  }
  _el_phase = 0; enterElect(now);                         // deferred, but its stream never came: a new inventory
}
#else
void FwOtaTransfer::enterElect(uint32_t) { _el_phase = 0; }
void FwOtaTransfer::sendClaim() {}
void FwOtaTransfer::onClaim(const uint8_t*, uint32_t) {}
void FwOtaTransfer::electTick(uint32_t) { _el_phase = 0; }
#endif

// E: Trickle (RFC 6206). A node missing chunks after the stream, and a holder that heard a neighbour miss chunks lately, send a short
// zero-hop summary once per interval unless k equal summaries were heard; a different summary resets the interval to tr_imin. A collector
// that hears a neighbour with more chunks asks it (a gap round) at once.
static const uint32_t SUMM_LEN = 2 + 4 + 2 + 2 + XFER_MAC_LEN;
void FwOtaTransfer::trickleStart(uint32_t now, uint32_t ivl) {
  _tr_i = ivl; _tr_end = now + ivl; _tr_t = now + ivl / 2 + (uint32_t)_mesh->getRNG()->nextInt(0, (int)(ivl / 2)); _tr_c = 0;
}
void FwOtaTransfer::trickleTick(uint32_t now) {
  if (g_tune.algo != ALGO_E) return;
  const bool missing = _phase == PH_DOWNLOADING && _erased && _man.kind == KIND_APP && _gap_phase;
  const bool spread = memcmp(_spread_tag, _serve_man.sha256, 4) == 0;   // (rc19, C42) a fleet image: the 48 h window; another one fades in an hour
  const bool holder = _serving && _tr_inc_ms && since(now, _tr_inc_ms) < (spread ? g_tune.gossip_life_ms : PASSIVE_QUIET_MS) && memcmp(_serve_man.sha256, _tr_tag, 4) == 0;
  if (!missing && !holder) { _tr_on = false; return; }
  if (!_tr_on) { _tr_on = true; trickleStart(now, g_tune.tr_imin_ms); }
  if (_tr_t && (int32_t)(now - _tr_t) >= 0) { _tr_t = 0;
    if (_tr_c < g_tune.tr_k && !(g_tune.batt_rest && powerState() == 3)) { if (holder && !missing && g_tune.e_spread && spread) sendHolderAnn(); else sendSumm(); } }
  if ((int32_t)(now - _tr_end) >= 0) trickleStart(now, umin(_tr_i * 2, g_tune.tr_imax_ms));
}
void FwOtaTransfer::sendSumm() {
  const bool hold = _serving && memcmp(_serve_man.sha256, _tr_tag, 4) == 0 && !(_phase == PH_DOWNLOADING && _gap_phase);
  const XferManifest& m = hold ? _serve_man : _man;
  const uint8_t* k = hold ? imageKeys(_serve_man, 0) : _ik_mac;
  if (!k) return;
  uint8_t b[SUMM_LEN]; uint32_t n = 0;
  b[n++] = XFER_MAGIC; b[n++] = MSG_SUMM; memcpy(&b[n], m.sha256, 4); n += 4;
  const uint16_t hv = hold ? (uint16_t)m.n_chunks : _cm.have, nc = (uint16_t)m.n_chunks;
  memcpy(&b[n], &hv, 2); n += 2; memcpy(&b[n], &nc, 2); n += 2;
  n += XFER_MAC_LEN; macFillKey(b, n, k);
  stats.summs++;
  send(b, n, 50 + _mesh->getRNG()->nextInt(0, 500), nullptr, 0);
}
void FwOtaTransfer::sendHolderAnn() {                    // the FLEET_ANN layout, sent zero-hop by a holder: server = requester = us, have = n
  const XferManifest& m = _serve_man;
  const bool og = g_tune.src_spread && memcmp(_origin_tag, m.sha256, 4) == 0;   // (src_spread) the requester field = the image's origin
  uint8_t b[FANN_LEN];
  const uint32_t n = buildFann(b, m, FLEET_REPLACE | (g_tune.src_spread ? FLEET_HOLDER : 0), _pub, (uint16_t)m.n_chunks, og ? _origin : _pub);
  stats.summs++;
  send(b, n, 50 + _mesh->getRNG()->nextInt(0, 500), nullptr, 0);
}
// (rc18) this node holds an image that came by a fleet pull (or is the source of one): gossip it as a holder, older neighbours fill from it
void FwOtaTransfer::spreadStart(const uint8_t* digest) {
  if (g_tune.algo != ALGO_E || !g_tune.src_spread) return;
  memcpy(_spread_tag, digest, 4); _tr_inc_ms = millis() | 1; memcpy(_tr_tag, digest, 4);
}

// (study src_spread) a pull by id that does not move (no answer yet, or no chunk for 30 min) hears a neighbour that holds the image of
// the node it targets: it pulls that image from the neighbour (the chunks it has are kept - same image)
bool FwOtaTransfer::holderSwitch(const XferManifest& m, const uint8_t* holder, const uint8_t* origin) {
  if (g_tune.algo != ALGO_E || !g_tune.src_spread || !g_tune.req_adopt || _fill || m.kind != KIND_APP) return false;
  uint8_t tid[4]; if (!pullTargetId(tid) || memcmp(tid, origin, 4) != 0 || memcmp(tid, holder, 4) == 0) return false;
  const uint32_t now = millis();
  const bool stuck = _phase == PH_OFFERING ? since(now, _wait_started_ms) > 600000u
                   : _phase == PH_DOWNLOADING && !_listening && _last_chunk_ms && since(now, _last_chunk_ms) > 1800000u;
  if (!stuck || (_last_switch_ms && since(now, _last_switch_ms) < 1800000u)) return false;
  if (_img_set && memcmp(_img_digest, m.sha256, 8) != 0) return false;
  _img_set = true; _img_kind = KIND_APP; _img_ver = m.fw_version; memcpy(_img_digest, m.sha256, 8);
  memcpy(_holders[0], holder, 4); _nholders = 1; _holders_tried = 0;
  memcpy(_origin, origin, 4); memcpy(_origin_tag, m.sha256, 4);
  char t[16]; snprintf(t, sizeof(t), "@%02X%02X%02X%02X", holder[0], holder[1], holder[2], holder[3]);
  char tmp[160]; switchSource(t, tmp); _last_switch_ms = now | 1; stats.elected++; noteSwitch(4, holder);
  return true;
}
void FwOtaTransfer::onSumm(const uint8_t* b, uint32_t len) {
  if (g_tune.algo != ALGO_E || len != SUMM_LEN) return;
  const bool miss = _phase == PH_DOWNLOADING && _erased && memcmp(&b[2], _man.sha256, 4) == 0;
  const bool hold = !miss && _serving && memcmp(&b[2], _serve_man.sha256, 4) == 0;
  if (!miss && !hold) return;
  const uint8_t* k = hold ? imageKeys(_serve_man, 0) : _ik_mac;
  if (!k || !macOk(k, b, len)) return;
  uint16_t hv; memcpy(&hv, &b[6], 2);
  const uint16_t mine = hold ? (uint16_t)_serve_man.n_chunks : _cm.have;
  const uint32_t now = millis();
  if (hv == mine) { _tr_c++; return; }                   // consistent
  if (hold) { _tr_inc_ms = now | 1; memcpy(_tr_tag, &b[2], 4); }
  if (miss && hv > mine && _gap_phase) {                 // a neighbour has more: ask now
    _gap_dry = 0; _e_wait = false; _e_prog_ms = now | 1; _gapr_at_ms = now + (uint32_t)_mesh->getRNG()->nextInt(0, GAP_SPREAD_MS);
  }
  if (_tr_on && _tr_i > g_tune.tr_imin_ms) trickleStart(now, g_tune.tr_imin_ms);
}

}  // namespace fw_ota
