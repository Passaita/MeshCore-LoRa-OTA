#!/bin/bash
# Host test of the LoRa-OTA duty (v1.17.25, A10): the pure functions are EXTRACTED from the real src/helpers/fw_ota/FwOtaTransfer.cpp on every
# run (dutyParse, dutyAgree, dutySpacingMs, dutyWinMax, dutyWinExtraMs), compiled with ASan/UBSan and checked:
#   - the owner's negotiation examples (the server's duty is a ceiling, a per-pull duty overrides the puller's own, never multiplies);
#   - two pullers share one server's duty;
#   - a simulated pull (server pacing + puller window timeout, as in onChunkReq / requestWindow) at duty 0.1 .. 1.0 and 0 - 3 relays:
#     no re-request and no duplicate chunk on a lossless link, and the airtime share never above the agreed duty (+2 %).
# usage: test/fw_ota_duty_test.sh
set -e
cd "$(dirname "$0")/.."
W=$(mktemp -d)
python3 - "$W/duty_test.cpp" <<'PY'
import sys
HEAD = r"""#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cctype>
#include <cstdlib>
static const uint8_t DUTY_MIN_PCT = 10, XFER_WINDOW = 8;
struct AnsRec { uint32_t sess; uint32_t ms; uint8_t direct; uint8_t flooded; };
static const uint32_t SCAN_MS = 25000, SCAN_REQ_MIN_MS = 6000, SCAN_REQ_SPREAD_MS = 4000, ANS_MEMORY_MS = 60000; static const uint8_t SCAN_REQ_RESENDS = 1;
static const uint32_t OFFER_RETRY_MS = 4000, WAIT_BACKOFF_MAX_MS = 60000, WAIT_BACKOFF_LONG_MS = 300000; static const uint16_t WAIT_LONG_AFTER = 10;
static const uint32_t CHUNK_GAP_MS = 90, DUTY_WIN_MS = 10000, WIN_RETRY_MS = 6000, BATCH_GAP_MS = 5000, BATCH_GAP_HOP_MS = 2500, YIELD_QUIET_MS = 2000, YIELD_MAX_MS = 20000; static const uint8_t DUTY_FULL_PCT = 30; static const uint8_t RELAY_BATCH = 5;
"""
TAIL = r"""
// ---- a pull, simulated (v1.17.27): one server, one puller, H relays; chunk airtime `air`; lossless ----
// channel share = airtime of every transmission of the chunks (server + each relay repeat) / elapsed time
struct Sim { uint32_t chunks_sent = 0, dups = 0, rereq = 0, windows = 0, overlap = 0; double share = 0; uint32_t total_ms = 0; };
static Sim simulate(uint32_t air, uint8_t pct, int hops, uint32_t n_chunks) {
  Sim s; const uint32_t hop_ms = air + 300, req_air = 150;           // a relay repeats after its own airtime + a small delay
  const uint32_t lat_chunk = hops * hop_ms, lat_req = req_air + hops * (req_air + 300);
  std::vector<bool> have(n_chunks, false); uint32_t first_missing = 0, t = 0, next_tx = 0, chan_air = 0, last_start = 0; bool any = false;
  const uint32_t sp = dutySpacingMs(air, pct, hops); const bool paced = dutyPaced(pct, hops);
  uint32_t rtt = lat_req + 150 + 400 + hops * (400 + 300);
  while (first_missing < n_chunks) {
    uint32_t cnt = dutyWinMax(air, pct, hops); if (!hops && cnt > XFER_WINDOW) cnt = XFER_WINDOW; if (first_missing + cnt > n_chunks) cnt = n_chunks - first_missing;
    uint32_t timeout = WIN_RETRY_MS / 2 + dutyWinExtraMs(air, pct, hops) + (hops ? (rtt < 20000 ? rtt : 20000) : 0);   // as requestWindow
    uint32_t t_req = t; s.windows++;
    uint32_t now0 = t_req + lat_req, d0 = 100;
    if (paced && next_tx && next_tx > now0 + d0) d0 = next_tx - now0;
    uint32_t last_arrival = t_req, radio_free = now0;
    for (uint32_t k = 0; k < cnt; k++) {
      uint32_t idx = first_missing + k;
      uint32_t start = now0 + (paced ? d0 + k * sp : 100 + k * CHUNK_GAP_MS); if (start < radio_free) start = radio_free;
      if (hops && any && start - last_start < air * (hops + 1)) s.overlap++;   // the next chunk while a relay still repeats the previous one
      last_start = start; any = true;
      radio_free = start + air; chan_air += air * (hops + 1); s.chunks_sent++;
      uint32_t arrive = start + air + lat_chunk;
      if (arrive - last_arrival > timeout) s.rereq++;
      if (k == 0) rtt = (rtt * 3 + (arrive - t_req)) / 4;
      last_arrival = arrive;
      if (have[idx]) s.dups++; have[idx] = true;
    }
    if (paced) next_tx = now0 + d0 + cnt * sp + batchGapMs(hops);
    t = last_arrival;
    while (first_missing < n_chunks && have[first_missing]) first_missing++;
  }
  s.total_ms = t; s.share = (double)chan_air / t; return s;
}
int main(){
  setvbuf(stdout, NULL, _IONBF, 0);
  int checks = 0;
  // 1) parsing "fw duty x" / "... duty x"
  struct { const char* in; int pct; } P[] = { {"0.5",50},{"0.1",10},{"1",100},{"1.0",100},{"1.00",100},{"0.35",35},{" 0.8",80},{"0.80",80},{"0.500",50},
    {"0.05",0},{"0",0},{"1.1",0},{"2",0},{"",0},{"abc",0},{"0.5x",0},{".5",0},{"0.555",0},{"-0.5",0},{"0.5 1",0},{"99999999999",0} };
  for (auto& p : P) { if (dutyParse(p.in) != p.pct) { printf("dutyParse(%s)=%d want %d\n", p.in, dutyParse(p.in), p.pct); assert(0); } checks++; }
  // 2) the owner's examples (2026-09-28): effective = min(pull duty, server duty); a per-pull duty replaces the puller's own
  auto pullDuty = [](uint8_t node, uint8_t per_pull) { return per_pull ? per_pull : node; };
  assert(dutyAgree(pullDuty(50, 0), 80, 0) == 50); checks++;          // server 0.8, puller global 0.5, 'fw pull #1 pw'          -> 0.5
  assert(dutyAgree(pullDuty(50, 80), 80, 0) == 80); checks++;         // server 0.8, 'fw pull #1 pw duty 0.8'                     -> 0.8
  assert(dutyAgree(pullDuty(50, 100), 50, 0) == 50); checks++;        // server 0.5, 'fw pull #x pw duty 1'                       -> 0.5
  assert(dutyAgree(pullDuty(30, 0), 50, 0) == 30); checks++;          // a slower puller is never sped up
  assert(dutyAgree(0, 50, 0) == 50); checks++;                        // an older puller (no wish): the server's own duty
  // 3) two pullers share the server: the second one gets what is left, never below 0.1
  assert(dutyAgree(50, 80, 50) == 30); checks++;
  assert(dutyAgree(50, 50, 50) == DUTY_MIN_PCT); checks++;
  assert(dutyAgree(50, 50, 200) == DUTY_MIN_PCT); checks++;
  assert(dutyAgree(100, 100, 0) == 100); checks++;
  // 4) pacing math
  assert(dutySpacingMs(611, 100, 0) == CHUNK_GAP_MS && dutySpacingMs(611, 50, 0) == 1222 && dutySpacingMs(611, 10, 0) == 6110); checks++;
  assert(dutySpacingMs(611, 50, 2) == 3666 && dutySpacingMs(611, 30, 2) == 6110 && dutySpacingMs(611, 100, 2) == 1833); checks++;   // relays count (v1.17.27)
  assert(dutyWinMax(611, 100, 0) == 8 && dutyWinMax(611, 50, 0) == 8 && dutyWinMax(611, 20, 0) == 3 && dutyWinMax(611, 10, 0) == 1); checks++;
  assert(dutyWinMax(611, 30, 1) == RELAY_BATCH && dutyWinMax(611, 10, 3) == RELAY_BATCH); checks++;                                // fixed batch through relays
  assert(dutyWinExtraMs(611, 100, 0) == 0 && dutyWinExtraMs(611, 50, 0) == 1222 - 611 + BATCH_GAP_MS + 1000); checks++;
  assert(batchGapMs(0) == 5000 && batchGapMs(1) == 5000 && batchGapMs(2) == 7500 && batchGapMs(3) == 10000); checks++;   // (rc5, C12)
  for (int pc = 10; pc <= 100; pc++) for (int h = 0; h <= 3; h++) { assert(dutyWinMax(611, pc, h) >= 1 && dutyWinMax(611, pc, h) <= XFER_WINDOW); checks++; }
  // 5) simulated pulls: no re-request, no duplicate, CHANNEL share (server + relay repeats) <= agreed + 2 %, no chunk on top of its own repeats
  const uint8_t pcts[] = {10, 20, 30, 50, 80, 100}; const uint32_t airs[] = {611, 450, 900};
  for (uint32_t air : airs) for (uint8_t pc : pcts) for (int h = 0; h <= 3; h++) {
    Sim s = simulate(air, pc, h, 300);
    printf("  air %4u ms duty %.2f %d relay(s): %3u windows, channel share %.3f, %2u re-requests, %u dups, %u overlaps, %6.1f min\n", air, pc / 100.0, h, s.windows, s.share, s.rereq, s.dups, s.overlap, s.total_ms / 60000.0);
    assert(s.rereq == 0); checks++;
    assert(s.overlap == 0); checks++;
    assert(s.dups == 0); checks++;
    if (pc < 100) { assert(s.share <= pc / 100.0 + 0.02); checks++; }                                                            // the whole chain
  }
  // (v1.17.28) waiting for a server that does not answer: 4 s doubling to 60 s as before for the first WAIT_LONG_AFTER offers, then on to
  // 5 min - a stall of hours (the way back broken) costs a fraction of the offers; the wait is jittered 0.5 .. 1.5 x (mean = the value)
  { uint32_t b = 0, t = 0; uint16_t a = 0; int in1h = 0, in2h = 0; uint32_t mx = 0; const uint32_t old[10] = {4000,8000,16000,32000,60000,60000,60000,60000,60000,60000};
    while (t < 7200000UL) { b = waitBackoffMs(b, a); if (a < 10) assert(b == old[a]); a++; t += b; if (b > mx) mx = b; if (t <= 3600000UL) in1h++; in2h++; }
    assert(mx == WAIT_BACKOFF_LONG_MS && in2h <= 36); checks++;
    uint32_t o = 0, to = 0; int old2h = 0; while (to < 7200000UL) { o = o ? (o * 2 > 60000 ? 60000 : o * 2) : 4000; to += o; old2h++; }
    printf("  waiting offers: %d in the 1st hour, %d in 2 h (v1.17.27: %d), longest wait %u s\n", in1h, in2h, old2h, (unsigned)(mx / 1000)); }
  // (v1.17.28) fw list on air: 2 direct requests per scan (was 8), each server answers a session once (twice directly), never flooded
  // on top of a direct answer - the burst that drowned the weak relay path to board 2 (2026-09-29)
  { AnsRec r[6] = {}; uint8_t wr = 0; uint32_t t = 1000;
    assert(answerAllowed(r,6,wr,0xA1,false,t) && answerAllowed(r,6,wr,0xA1,false,t+7000) && !answerAllowed(r,6,wr,0xA1,false,t+8000)); checks++;   // 2 direct, not 3
    assert(!answerAllowed(r,6,wr,0xA1,true,t+8100)); checks++;                                       // answered directly: no flood on top
    assert(answerAllowed(r,6,wr,0xB2,true,t) && !answerAllowed(r,6,wr,0xB2,true,t+50)); checks++;   // a flood once (the same discovery twice)
    assert(answerAllowed(r,6,wr,0xB2,false,t+60)); checks++;                                          // a direct request after the flood: answered
    assert(answerAllowed(r,6,wr,0xA1,false,t+8100+ANS_MEMORY_MS)); checks++;                          // forgotten after a minute
    for (uint32_t k = 0; k < 6; k++) assert(answerAllowed(r,6,wr,0xC0+k,true,t+70000)); assert(answerAllowed(r,6,wr,0xB2,true,t+70001)); checks++;   // ring: oldest evicted
    int old_pk = 1 + 8 + 8 * 2 + 2, new_pk = 0;                                                      // one server in direct range: DISC + 8 req + 16 answers + 2 floods
    for (int rnd = 0; rnd <= 4000; rnd += 500) {
      uint32_t start = 0, end = SCAN_MS, n_req = 1; uint8_t done = 0; uint32_t at = scanResendAt(0, start, rnd, end);
      while (at) { n_req++; done++; assert(at >= start + SCAN_REQ_MIN_MS && at <= start + SCAN_REQ_MIN_MS + SCAN_REQ_SPREAD_MS && end - at >= 3000); at = scanResendAt(done, at, rnd, end); }
      assert(n_req == 2); AnsRec q[6] = {}; uint8_t w = 0; int ans = 0;
      ans += 2 * answerAllowed(q,6,w,0x77,false,1000);                                               // app + bootloader offer, in time order:
      ans += 2 * answerAllowed(q,6,w,0x77,true,1100);                                                //   the request, the flood (same session),
      for (uint32_t i = 1; i < n_req; i++) ans += 2 * answerAllowed(q,6,w,0x77,false,1000 + i * 7000); //   the repeated request
      assert(ans == 4); new_pk = 1 + (int)n_req + ans; }
    checks++; printf("  fw list with one server in direct range: %d packets (v1.17.28-rc1: %d)\n", new_pk, old_pk); }
  // (v1.17.28, C2/C6/C7) how a server answers a discovery: bits 1 app, 2 bootloader, 4 flooded, 8 direct
  { assert(discAnswer("*", 0, true, true, true) == 0); checks++;                        // C2: a scan from a direct neighbour - its LIST_REQ gets it
    assert(discAnswer("*", 2, true, true, true) == (1|2|4)); checks++;                  // a scan through relays: both offers, flooded
    assert(discAnswer("@1A2B3C4D", 2, true, true, true) == (1|4|8)); checks++;          // C6 + C7: a pull for the app - app only, flood AND direct
    assert(discAnswer("@1A2B3C4D!", 1, true, true, true) == (2|4|8)); checks++;         // C7: a bootloader update - bootloader only
    assert(discAnswer("@1A2B3C4D", 0, true, true, false) == (1|4|8)); checks++;         // a pull from a neighbour: still answered
    assert(discAnswer("@1A2B3C4D!", 2, true, true, false) == 0 && discAnswer("*", 3, true, false, false) == 0); checks++;   // nothing to offer
    assert(discAnswer("*", 0, false, true, true) == (1|2|8)); checks++; }                // an old discovery without " F": direct, as before
  // (v1.17.28, C8) fw pull-pause [Nh]
  { assert(pauseHours("") == 0 && pauseHours("  ") == 0 && pauseHours(" 3h") == 3 && pauseHours("168H") == 168); checks++;
    assert(pauseHours("0h") == -1 && pauseHours("169h") == -1 && pauseHours("3") == -1 && pauseHours("3hx") == -1 && pauseHours("h") == -1 && pauseHours("30m") == -1); checks++; }
  // (rc6, owner) duty rescale: the user's 1.0 = the old 0.3 (30 % of the channel model), 0.3 = 9 %; both ends pace by chanPct()
  { assert(chanPct(100) == 30 && chanPct(30) == 9 && chanPct(10) == 3 && chanPct(1) == 1 && chanPct(0) == 0); checks++;
    const uint32_t air = 611;                                          // a 160 B chunk at SF7/62.5 kHz
    const double r1 = 60000.0 / (dutySpacingMs(air, chanPct(100), 2) * 5 + batchGapMs(2)) * 5, r3 = 60000.0 / (dutySpacingMs(air, chanPct(30), 2) * 5 + batchGapMs(2)) * 5;
    assert(dutySpacingMs(air, chanPct(100), 2) == dutySpacingMs(air, 30, 2)); checks++;     // new 1.0 = old 0.3
    printf("  rc6 rescale, 2 relays, ideal: duty 1.0 %.1f chunks/min (1620 chunks %.1f h), duty 0.3 %.1f chunks/min (%.1f h)\n", r1, 1620 / r1 / 60, r3, 1620 / r3 / 60); }
  // (rc6, C14) send only after YIELD_QUIET_MS without other traffic, or after YIELD_MAX_MS of waiting
  { assert(otaQuiet(1000, 0, 0)); assert(!otaQuiet(5000, 4000, 0)); assert(otaQuiet(6000, 4000, 0)); checks++;
    assert(!otaQuiet(20000, 19500, 5000) && otaQuiet(25001, 25000, 5000)); checks++;    // waiting 20 s: send anyway
    assert(!otaQuiet(3, 0xFFFFFFF0u, 0) && otaQuiet(0x7D1, 0xFFFFFFFFu, 0)); checks++; }   // millis() wrap
  printf("duty parse + negotiation + pacing + simulated pulls + wait back-off + scan/answer budget + discovery answers + pause + rc6: %d passed\n", checks); return 0;
}
"""
src = open("src/helpers/fw_ota/FwOtaTransfer.cpp").read()
def fn(sig):
    i = src.index(sig); j = src.index("{", i); d = 0
    for k in range(j, len(src)):
        if src[k] == "{": d += 1
        elif src[k] == "}":
            d -= 1
            if d == 0: return src[i:k + 1]
funcs = [fn(s) for s in ("static uint8_t dutyParse(", "static uint8_t dutyAgree(", "static bool dutyPaced(", "static uint32_t dutySpacingMs(", "static uint8_t dutyWinMax(", "static uint32_t batchGapMs(", "static uint8_t chanPct(", "static bool otaQuiet(", "static uint32_t dutyWinExtraMs(", "static uint32_t waitBackoffMs(", "static bool answerAllowed(", "static uint32_t scanResendAt(", "static uint8_t discAnswer(", "static int pauseHours(")]
open(sys.argv[1], "w").write(HEAD + "#include <vector>\n" + "\n".join(funcs) + "\n" + TAIL)
PY
g++ -std=c++11 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o "$W/duty_test" "$W/duty_test.cpp"
"$W/duty_test"
rm -rf "$W"
