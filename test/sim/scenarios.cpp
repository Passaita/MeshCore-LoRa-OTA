// LoRa-OTA network simulator - scenarios on the REAL code. Each prints PASS/FAIL; the run fails if any scenario fails.
#define private public           // F2 reads a node's progress (as audit.cpp does; the layout is unchanged)
#define protected public
#include "sim.h"
#undef private
#undef protected
using namespace sim;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
static uint32_t floodsAll(World& w) { uint32_t n = 0; for (auto& x : w.nodes) n += x->st.floods; return n; }
static const fw_ota::FwOtaTransfer::SwarmStats& ss(World& w, int i) { return w[i].xfer->stats; }
static bool serving1(World& w, int i) { return has(w.cli(i, "fw duty"), "serving 1 pull"); }
static const char* PHRASE = "fw passphrase sim circle delta tango";

static void circle(World& w, std::vector<int> ids) {
  for (int i : ids) w.cli(i, PHRASE);
  w.run(120000, [&]() { for (int i : ids) if (!has(w.cli(i, "fw passphrase"), "fingerprint")) return false; return true; });
}

// S0: two neighbours - a holder serves, the other scans, pulls by version and installs it (sanity of the simulator itself)
static void s0_basic() {
  printf("S0 basic pull between two neighbours\n");
  World w; W = &w; g_sim_now = 0;
  int a = w.add("A").idx, b = w.add("B").idx; w.link(a, b, 10);
  w.seedImage(a, 300, 32000, 7, true); w.seedImage(b, 299, 31000, 8, true);
  circle(w, {a, b});
  w.run(20000);
  CHECK(has(w.cli(a, "fw serve"), "fw-300"), "A serves fw-300");
  w.cli(b, "fw duty 1"); w.cli(a, "fw duty 1");
  w.cli(b, "fw peers"); w.run(35000);
  std::string p = w.cli(b, "fw list");
  CHECK(has(p, "#1 fw-300 x1 (0hop)"), "B lists fw-300: %s", p.c_str());
  w.cli(b, "fw pull #1");
  w.run(3 * 3600000, [&]() { return w[b].app_fw == 300; });
  CHECK(w[b].app_fw == 300, "B installed fw-300 (app_fw=%u) - %s", w[b].app_fw, w.cli(b, "fw pull-status").c_str());
  printf("  B pulled + installed at %.1f min; A sent %u packets (%u ms air), B %u\n", g_sim_now / 60000.0, w[a].st.tx, w[a].st.tx_air, w[b].st.tx);
}

static std::string after(const std::string& s, const char* key) { size_t i = s.find(key); return i == std::string::npos ? "" : s.substr(i + strlen(key)); }

// K1-K4: today's hardware duty tests (2026-10-06, rc13/rc14 on boards 1 / twin / board 3) on the simulated network
static void k_duty() {
  printf("K1-K4 fair share between pullers that cannot hear each other (two relay branches), duty changes, a leaving pull\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p1 = w.add("P1").idx, p2 = w.add("P2").idx, r1 = w.add("R1", false).idx, r2 = w.add("R2", false).idx;
  w.link(s, r1, 11); w.link(r1, p1, 11); w.link(s, r2, 11); w.link(r2, p2, 11);   // (rc15) same-area pullers share ONE stream (K7)
  w.seedImage(s, 301, 160000, 9, true); w.seedImage(p1, 300, 150000, 1, true); w.seedImage(p2, 300, 150000, 1, true);
  circle(w, {s, p1, p2});
  for (int i : {s, p1, p2}) w.cli(i, "fw duty 1");
  w.run(20000);
  w.run(130000); w.cli(p1, "fw peers"); w.run(5000); w.cli(p2, "fw peers"); w.run(60000);
  if (getenv("SIM_TRACE")) printf("    P1 peers: %s\n    P2 peers: %s\n", w.cli(p1, "fw list").c_str(), w.cli(p2, "fw list").c_str());
  w.cli(p1, "fw pull #1"); w.run(180000);
  std::string d = w.cli(s, "fw duty");
  CHECK(has(d, "serving 1 pull(s) at 1.00"), "K1 one pull at 1.00: %s", d.c_str());
  w.cli(p2, "fw pull #1"); w.run(180000);
  d = w.cli(s, "fw duty"); std::string st = w.cli(s, "fw pull-status"), q1 = w.cli(p1, "fw pull-status");
  CHECK(has(d, "serving 2 pull(s) at 1.00"), "K1 two pulls, total 1.00: %s", d.c_str());
  CHECK(has(st, "at 0.50") && has(after(st, "at 0.50"), "at 0.50"), "K1 0.50 + 0.50: %s", st.c_str());
  CHECK(has(q1, "duty 0.50") && has(q1, "shared by the server"), "K1 the running pull was told: %s", q1.c_str());
  w.cli(s, "fw duty 0.6"); w.run(120000);
  st = w.cli(s, "fw pull-status");
  CHECK(has(st, "at 0.30") && has(after(st, "at 0.30"), "at 0.30"), "K2 server 0.6 -> 0.30 + 0.30: %s", st.c_str());
  w.cli(s, "fw duty 1"); w.run(120000);
  st = w.cli(s, "fw pull-status");
  CHECK(has(st, "at 0.50") && has(after(st, "at 0.50"), "at 0.50"), "K2 back to 0.50 + 0.50: %s", st.c_str());
  w.cli(p1, "fw duty 0.3"); w.run(120000);
  st = w.cli(s, "fw pull-status");
  CHECK(has(st, "at 0.30") && has(st, "at 0.70"), "K4 puller 0.3 -> 0.30 + 0.70: %s", st.c_str());
  w.cli(p1, "fw duty 1"); w.run(120000);
  w.cli(p1, "fw pull-pause"); w.run(90000);
  d = w.cli(s, "fw duty"); std::string q2 = w.cli(p2, "fw pull-status");
  CHECK(has(d, "serving 1 pull(s) at 1.00"), "K3 a pause frees the share: %s", d.c_str());
  CHECK(has(q2, "duty 1.00"), "K3 the other pull gets 1.00: %s", q2.c_str());
  q1 = w.cli(p1, "fw pull-status"); w.cli(p1, "fw pull-resume"); w.run(60000);
  q2 = w.cli(p2, "fw pull-status");
  CHECK(!has(q2, "mac_fail"), "C33 no false mac_fail from a neighbour's PACE: %s", q2.c_str());
  CHECK(w[s].st.tx > 0 && !w[s].st.wdt_resets, "no watchdog resets");
  printf("  server sent %u packets, %u ms air in %.0f min; collisions seen at P1 %u, P2 %u\n", w[s].st.tx, w[s].st.tx_air, g_sim_now / 60000.0, w[p1].st.collisions, w[p2].st.collisions);
}

// K5: failover between two holders of the same image (rc13 fix C30): the chosen holder stops serving -> the pull moves on, chunks kept
static void k_failover() {
  printf("K5 failover to the second holder (rc13, C30)\n");
  World w; W = &w; g_sim_now = 0;
  int h1 = w.add("H1").idx, h2 = w.add("H2").idx, p = w.add("P").idx;
  w.link(h1, p, 11); w.link(h2, p, 9); w.link(h1, h2, 10);
  w.seedImage(h1, 301, 160000, 9, true); w.seedImage(h2, 301, 160000, 9, true); w.seedImage(p, 300, 150000, 1, true);
  circle(w, {h1, h2, p});
  for (int i : {h1, h2, p}) w.cli(i, "fw duty 1");
  w.run(20000); w.cli(p, "fw peers"); w.run(35000);
  std::string pe = w.cli(p, "fw list");
  CHECK(has(pe, "#1 fw-301 x2"), "two holders of one image: %s", pe.c_str());
  w.cli(p, "fw pull #1"); w.run(240000);
  std::string ps = w.cli(p, "fw pull-status");
  int first = has(ps, "@") ? w.find(after(ps, "'@").substr(0, 0)) : -1; (void)first;
  bool from_h1 = has(ps, "downloading"); (void)from_h1;
  // stop whichever holder serves it: ask both servers who has an active pull
  int busy = has(w.cli(h1, "fw duty"), "serving 1 pull") ? h1 : h2, other = busy == h1 ? h2 : h1;
  w.cli(busy, "fw serve off"); uint32_t t_off = g_sim_now; const uint32_t sf0 = w[p].xfer->stats.searches_flood;
  if (getenv("SIM_TRACE")) for (int k = 0; k < 24; k++) { w.run(30000); printf("    +%3.1f min %s\n", (g_sim_now - t_off) / 60000.0, w.cli(p, "fw pull-status").c_str()); if (has(w.cli(other, "fw duty"), "serving 1 pull")) break; }
  w.run(15 * 60000, [&]() { return has(w.cli(other, "fw duty"), "serving 1 pull"); });
  uint32_t took = g_sim_now - t_off;
  CHECK(has(w.cli(other, "fw duty"), "serving 1 pull"), "the pull moved to %s after %.1f min: %s", w[other].name.c_str(), took / 60000.0, w.cli(p, "fw pull-status").c_str());
  CHECK(took < 5 * 60000, "R1 (C34) the switch came within 5 min (took %.1f min)", took / 60000.0);
  CHECK(w[p].xfer->stats.searches_flood == sf0, "R6 the image search found the neighbour holder without a flood (%u floods, %u zero-hop)",
        w[p].xfer->stats.searches_flood - sf0, w[p].xfer->stats.searches_zero);
  w.run(3 * 3600000, [&]() { return w[p].app_fw == 301; });
  CHECK(w[p].app_fw == 301, "the pull finished from the second holder: %s", w.cli(p, "fw pull-status").c_str());
  printf("  switch after %.1f min; installed at %.0f min\n", took / 60000.0, g_sim_now / 60000.0);
}

// K6: a pull through one stock MeshCore relay (board 2's position): discovery through the relay, route learnt, transfer finishes
static void k_relay() {
  printf("K6 pull through a stock relay\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r = w.add("Relay", false, true).idx, p = w.add("P").idx;
  w.link(s, r, 8); w.link(r, p, 6);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true);
  circle(w, {s, p});
  w.cli(s, "fw duty 1"); w.cli(p, "fw duty 1");
  w.run(20000); w.cli(p, "fw peers"); w.run(60000);
  std::string pe = w.cli(p, "fw list");
  CHECK(has(pe, "fw-301 x1 (1hop)"), "listed behind the relay: %s", pe.c_str());
  w.cli(p, "fw pull #1");
  w.run(4 * 3600000, [&]() { return w[p].app_fw == 301; });
  CHECK(w[p].app_fw == 301, "finished through the relay: %s", w.cli(p, "fw pull-status").c_str());
  printf("  400 chunks through 1 relay: %.0f min; relay forwarded %u packets\n", g_sim_now / 60000.0, w[r].st.tx);
}


// K7 (rc15 shared image): a second puller of the same image in the same area LISTENS - the server serves one stream; a server duty
// change PACEs the requester only, the listener overhears that PACE without a MAC fail (C33)
static uint32_t nChunks(uint32_t len) { return (len + 159) / 160; }
static void k_share() {
  printf("K7 shared stream: the second puller listens\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p1 = w.add("P1").idx, p2 = w.add("P2").idx;
  w.link(s, p1, 11); w.link(s, p2, 11); w.link(p1, p2, 11);
  w.seedImage(s, 301, 160000, 9, true); w.seedImage(p1, 300, 150000, 1, true); w.seedImage(p2, 300, 150000, 1, true);
  circle(w, {s, p1, p2});
  for (int i : {s, p1, p2}) w.cli(i, "fw duty 1");
  w.run(150000); w.cli(p1, "fw peers"); w.cli(p2, "fw peers"); w.run(40000);
  const uint32_t tx0 = w[s].st.tx;
  w.cli(p1, "fw pull #1"); w.run(180000);
  w.cli(p2, "fw pull #1"); w.run(120000);
  std::string d = w.cli(s, "fw duty"), q2 = w.cli(p2, "fw pull-status");
  CHECK(has(d, "serving 1 pull(s) at 1.00"), "one stream at 1.00: %s", d.c_str());
  CHECK(has(q2, "saving shared stream"), "P2 listens: %s", q2.c_str());
  w.cli(s, "fw duty 0.6"); w.run(120000);
  q2 = w.cli(p2, "fw pull-status");
  CHECK(!has(q2, "mac_fail"), "C33 no MAC fail from the requester's PACE: %s", q2.c_str());
  w.cli(s, "fw duty 1");
  uint32_t t1 = 0, t2 = 0;
  w.run(4 * 3600000, [&]() { if (!t1 && w[p1].app_fw == 301) t1 = g_sim_now; if (!t2 && w[p2].app_fw == 301) t2 = g_sim_now; return t1 && t2; });
  CHECK(t1 && t2, "both finished: %s / %s", w.cli(p1, "fw pull-status").c_str(), w.cli(p2, "fw pull-status").c_str());
  const double ratio = (double)(w[s].st.tx - tx0) / nChunks(160000);
  CHECK(ratio < 1.35, "server transmissions per chunk %.2f (< 1.35; two separate streams = ~2.1)", ratio);
  printf("  P1 done %.0f min, P2 done %.0f min (joined 3 min later); server tx/chunk %.2f\n", t1 / 60000.0, t2 / 60000.0, ratio);
}

// R2 (owner's bottleneck, rev 3): H1 serves P1, P2 listens; H2 gets the same image later and is idle - it hears the stream and advertises
// ZERO-HOP only; nobody moves for no gain (same links), no herd, no flood; both finish
static void r_bottleneck() {
  printf("R2 idle holder next to a shared stream\n");
  World w; W = &w; g_sim_now = 0;
  int h1 = w.add("H1").idx, h2 = w.add("H2").idx, p1 = w.add("P1").idx, p2 = w.add("P2").idx;
  for (int a : {h1, h2, p1, p2}) for (int b : {h1, h2, p1, p2}) if (a < b) w.link(a, b, 10);
  w.seedImage(h1, 301, 160000, 9, true); w.seedImage(h2, 300, 150000, 1, true);
  w.seedImage(p1, 300, 150000, 1, true); w.seedImage(p2, 300, 150000, 1, true);
  circle(w, {h1, h2, p1, p2});
  for (int i : {h1, h2, p1, p2}) w.cli(i, "fw duty 1");
  w.run(150000);
  w.cli(p1, "fw peers"); w.cli(p2, "fw peers"); w.run(40000);
  w.cli(p1, "fw pull #1"); w.run(60000); w.cli(p2, "fw pull #1"); w.run(240000);
  const uint32_t f0 = floodsAll(w), a0 = ss(w, h2).adverts, h1a = ss(w, h1).adverts;
  w.seedImage(h2, 301, 160000, 9, true); w.cli(h2, "fw serve on");   // H2 now holds the same image (as after a Bluetooth fill)
  w.run(30 * 60000);
  const uint32_t moves = ss(w, p1).moves + ss(w, p2).moves, adv = ss(w, h2).adverts - a0;
  CHECK(adv >= 1 && adv <= 2 && ss(w, h2).advert_floods == 0, "H2 advertised zero-hop 1-2 times (%u, floods %u)", adv, ss(w, h2).advert_floods);
  CHECK(ss(w, h1).adverts == h1a, "the busy server H1 stayed silent (%u)", ss(w, h1).adverts - h1a);
  CHECK(moves == 0, "no move without a gain (%u)", moves);
  CHECK(floodsAll(w) == f0, "no flood (%u)", floodsAll(w) - f0);
  w.run(4 * 3600000, [&]() { return w[p1].app_fw == 301 && w[p2].app_fw == 301; });
  CHECK(w[p1].app_fw == 301 && w[p2].app_fw == 301, "both finished: %s / %s", w.cli(p1, "fw pull-status").c_str(), w.cli(p2, "fw pull-status").c_str());
  printf("  H2 adverts %u (zero-hop), moves %u, floods %u, done at %.0f min\n", adv, moves, floodsAll(w) - f0, g_sim_now / 60000.0);
}

// R3: a quiet network costs nothing: holders + pullers + a stock relay, nobody pulls, 6 h; one holder reboots at 3 h (zero-hop advert only)
static void r_quiet() {
  printf("R3 quiet network: no floods for hours\n");
  World w; W = &w; g_sim_now = 0;
  int a = w.add("A").idx, b = w.add("B").idx, r = w.add("Relay", false, true).idx, c = w.add("C").idx, d = w.add("D").idx;
  w.link(a, b, 10); w.link(b, r, 8); w.link(r, c, 8); w.link(c, d, 9);
  w.seedImage(a, 301, 64000, 9, true); w.seedImage(b, 300, 60000, 1, true); w.seedImage(c, 301, 64000, 9, true); w.seedImage(d, 299, 60000, 2, true);
  circle(w, {a, b, c, d});
  const uint32_t f0 = floodsAll(w);
  w.run(3 * 3600000);
  w.boot(w[c], POWER_RESETREAS_SREQ_Msk);
  w.run(3 * 3600000);
  uint32_t adv = 0, txs = 0; for (int i : {a, b, c, d}) adv += ss(w, i).adverts;
  for (auto& n : w.nodes) txs += n->st.disc;
  CHECK(floodsAll(w) == f0, "no flood in 6 h (%u)", floodsAll(w) - f0);
  CHECK(adv <= 4, "only boot adverts, zero-hop (%u)", adv);
  printf("  6 h: %u floods, %u adverts (all zero-hop), %u discovery-channel transmissions in all (incl. the circle setup)\n", floodsAll(w) - f0, adv, txs);
}

// R4: 'fw list' behind a relay with 4 holders of one image: the flood step gets <= ~2 answers (K = 2), the image is still listed
static void r_suppress() {
  printf("R4 answer suppression: 4 holders, ~2 answers\n");
  World w; W = &w; g_sim_now = 0;
  int p = w.add("P").idx, r = w.add("Relay", false, true).idx;
  std::vector<int> hs; for (int k = 0; k < 4; k++) hs.push_back(w.add("H" + std::to_string(k)).idx);
  w.link(p, r, 8);
  for (int h : hs) { w.link(h, r, 9); for (int g : hs) if (g < h) w.link(g, h, 10); w.seedImage(h, 301, 64000, 9, true); }
  w.seedImage(p, 300, 60000, 1, true);
  std::vector<int> all = hs; all.push_back(p); circle(w, all);
  w.run(150000);
  const uint32_t f0 = floodsAll(w);
  w.cli(p, "fw peers"); w.run(60000);
  uint32_t sent = 0, supp = 0; for (int h : hs) { sent += ss(w, h).answers_sent; supp += ss(w, h).answers_suppressed; }
  std::string pe = w.cli(p, "fw list");
  CHECK(has(pe, "fw-301 x"), "the image is listed: %s", pe.c_str());
  CHECK(sent <= 2, "at most 2 flooded answers (%u sent, %u suppressed)", sent, supp);
  printf("  answers sent %u, suppressed %u, flood transmissions for the list %u; %s\n", sent, supp, floodsAll(w) - f0, pe.c_str());
}

// R5: a long weak chain: 3 stock relays, 10 % loss per link, low SNR - discovery, pull and install by themselves
static void r_chain() {
  printf("R5 long weak chain: 3 relays, 10 %% loss\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r1 = w.add("R1", false).idx, r2 = w.add("R2", false).idx, r3 = w.add("R3", false).idx, p = w.add("P").idx;
  w.link(s, r1, 3, 0.1); w.link(r1, r2, 2, 0.1); w.link(r2, r3, 2, 0.1); w.link(r3, p, 3, 0.1);
  w.seedImage(s, 301, 32000, 9, true); w.seedImage(p, 300, 30000, 1, true);
  circle(w, {s, p});
  w.cli(s, "fw duty 1"); w.cli(p, "fw duty 1");
  w.run(150000);
  uint32_t t_list = g_sim_now; std::string pe;
  for (int k = 0; k < 4 && !has(pe, "fw-301"); k++) { w.cli(p, "fw peers"); w.run(90000); pe = w.cli(p, "fw list"); }
  CHECK(has(pe, "fw-301 x1 (3hop)"), "listed 3 hops away: %s", pe.c_str());
  w.cli(p, "fw pull #1"); uint32_t t0 = g_sim_now;
  w.run(10 * 3600000, [&]() { return w[p].app_fw == 301; });
  CHECK(w[p].app_fw == 301, "finished over the weak chain: %s", w.cli(p, "fw pull-status").c_str());
  printf("  listed after %.1f min, 200 chunks in %.0f min; floods %u, lost packets %u\n", (t0 - t_list) / 60000.0, (g_sim_now - t0) / 60000.0, floodsAll(w),
         w[p].st.lost + w[s].st.lost + w[r1].st.lost + w[r2].st.lost + w[r3].st.lost);
}

// R7: restarts mid-pull - the puller (resumes from its saved state, chunks kept) and the server (the puller waits and continues)
static void r_reboot() {
  printf("R7 restarts mid-pull (puller, then server)\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p = w.add("P").idx; w.link(s, p, 10);
  w.seedImage(s, 301, 120000, 9, true); w.seedImage(p, 300, 110000, 1, true);
  circle(w, {s, p}); w.cli(s, "fw duty 1"); w.cli(p, "fw duty 1");
  w.run(20000); w.cli(p, "fw peers"); w.run(35000); w.cli(p, "fw pull #1"); w.run(8 * 60000);
  std::string before = w.cli(p, "fw pull-status");
  w.boot(w[p], POWER_RESETREAS_SREQ_Msk); w.run(5 * 60000);
  std::string after_ = w.cli(p, "fw pull-status");
  CHECK(has(after_, "downloading") || has(after_, "offering") || has(after_, "WAITING") || has(after_, "saving shared stream"), "the puller resumed after its restart: %s | before: %s", after_.c_str(), before.c_str());
  w.boot(w[s], POWER_RESETREAS_SREQ_Msk); w.run(60000);
  w.run(4 * 3600000, [&]() { return w[p].app_fw == 301; });
  CHECK(w[p].app_fw == 301, "finished after both restarts: %s", w.cli(p, "fw pull-status").c_str());
  CHECK(!w[p].st.wdt_resets && !w[s].st.wdt_resets, "no watchdog resets (%u / %u)", w[p].st.wdt_resets, w[s].st.wdt_resets);
  printf("  done at %.0f min\n", g_sim_now / 60000.0);
}

// R8 (C27): a node updated from rc8 keeps the old 20 h watchdog running across the soft reset: it lets it run out ONCE (planned restart),
// then runs its own 10 min one - no site visit
static void r_wdt_drain() {
  printf("R8 C27 watchdog drain (20 h -> 10 min)\n");
  World w; W = &w; g_sim_now = 0; w.step_ms = 20;
  int a = w.add("A").idx;
  w[a].wdt.RUNSTATUS = 1; w[a].wdt.CRV = 72000u * 32768u - 1; w[a].wdt_fed = g_sim_now;   // rc8's watchdog, running
  w.boot(w[a], POWER_RESETREAS_SREQ_Msk);                                                 // the update's restart: it survives
  CHECK(w[a].wdt.CRV == 72000u * 32768u - 1, "the old timeout still runs after the soft reset");
  w.run(21 * 3600000);
  CHECK(w[a].st.wdt_resets == 1, "exactly one planned watchdog restart (%u)", w[a].st.wdt_resets);
  CHECK(w[a].wdt.CRV == fw_ota::Watchdog::ourCrv(), "our 10 min timeout runs now (CRV %u)", w[a].wdt.CRV);
  w.run(3 * 3600000);
  CHECK(w[a].st.wdt_resets == 1, "no further watchdog resets (%u)", w[a].st.wdt_resets);
}

// R9: 1 / 2 / 5 pullers of one image start TOGETHER next to one server: ~one stream, all finish about together
static void r_shared_n(int npull) {
  printf("R9 shared stream, %d puller(s) start together\n", npull);
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx; std::vector<int> ps;
  for (int k = 0; k < npull; k++) ps.push_back(w.add("P" + std::to_string(k + 1)).idx);
  std::vector<int> all = ps; all.push_back(s);
  for (int a : all) for (int b : all) if (a < b) w.link(a, b, 10);
  w.seedImage(s, 301, 64000, 9, true); for (int p : ps) w.seedImage(p, 300, 60000, 1, true);
  circle(w, all); for (int i : all) w.cli(i, "fw duty 1");
  w.run(150000); for (int p : ps) w.cli(p, "fw peers"); w.run(40000);
  const uint32_t tx0 = w[s].st.tx, t0 = g_sim_now;
  for (int p : ps) { if (getenv("SIM_DBG")) printf("  %s peers: %s\n", w[p].name.c_str(), w.cli(p, "fw list").c_str()); w.cli(p, "fw pull #1"); }
  w.run(4 * 3600000, [&]() { for (int p : ps) if (w[p].app_fw != 301) return false; return true; });
  int done = 0; for (int p : ps) done += w[p].app_fw == 301;
  if (getenv("SIM_DBG") && done < npull) { int k = 0; for (auto& e : w.events) if (e.find("> fw duty") == std::string::npos && e.find("> fw serve") == std::string::npos && k++ < 60) printf("    %s\n", e.substr(0, 230).c_str()); }
  const double ratio = (double)(w[s].st.tx - tx0) / nChunks(64000);
  CHECK(done == npull, "all %d finished (%d)", npull, done);
  CHECK(ratio < (npull == 1 ? 1.25 : 1.6), "server transmissions per chunk %.2f with %d pullers", ratio, npull);
  printf("  %d pullers: all done after %.1f min, server tx/chunk %.2f\n", npull, (g_sim_now - t0) / 60000.0, ratio);
}

// R10: a late joiner at ~50 %: stores the second half from the running stream, asks only for the first half after it ended
static void r_late() {
  printf("R10 late joiner at 50 %%\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p1 = w.add("P1").idx, p2 = w.add("P2").idx;
  w.link(s, p1, 11); w.link(s, p2, 11); w.link(p1, p2, 11);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(p1, 300, 60000, 1, true); w.seedImage(p2, 300, 60000, 1, true);
  circle(w, {s, p1, p2}); for (int i : {s, p1, p2}) w.cli(i, "fw duty 1");
  w.run(150000); w.cli(p1, "fw peers"); w.cli(p2, "fw peers"); w.run(40000);
  const uint32_t tx0 = w[s].st.tx;
  w.cli(p1, "fw pull #1");
  w.run(3600000, [&]() { return has(w.cli(p1, "fw pull-status"), "(50%)") || has(w.cli(p1, "fw pull-status"), "(51%)"); });   // ~50 %
  w.cli(p2, "fw pull #1"); w.run(90000);
  std::string q2 = w.cli(p2, "fw pull-status"), d = w.cli(s, "fw duty");
  CHECK(has(q2, "saving shared stream"), "the joiner listens: %s", q2.c_str());
  CHECK(has(d, "serving 1 pull"), "still one stream: %s", d.c_str());
  w.run(4 * 3600000, [&]() { return w[p1].app_fw == 301 && w[p2].app_fw == 301; });
  CHECK(w[p1].app_fw == 301 && w[p2].app_fw == 301, "both finished: %s", w.cli(p2, "fw pull-status").c_str());
  const double ratio = (double)(w[s].st.tx - tx0) / nChunks(64000);
  CHECK(ratio < 1.75, "server transmissions per chunk %.2f (one image + the first half again ~1.5; separate ~2.1)", ratio);
  printf("  server tx/chunk %.2f, done at %.0f min\n", ratio, g_sim_now / 60000.0);
}

// R11: two joiners on lossy links: the gap stream after the end is shared too; every gap filled
static void r_lossy_joiners() {
  printf("R11 two lossy joiners share the gap stream\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p1 = w.add("P1").idx, p2 = w.add("P2").idx, p3 = w.add("P3").idx;
  w.link(s, p1, 11); w.link(s, p2, 6, 0.2); w.link(s, p3, 6, 0.2); w.link(p1, p2, 9); w.link(p1, p3, 9); w.link(p2, p3, 9, 0.1);
  w.seedImage(s, 301, 64000, 9, true); for (int p : {p1, p2, p3}) w.seedImage(p, 300, 60000, 1, true);
  circle(w, {s, p1, p2, p3}); for (int i : {s, p1, p2, p3}) w.cli(i, "fw duty 1");
  w.run(150000); for (int p : {p1, p2, p3}) { w.cli(p, "fw peers"); } w.run(40000);
  const uint32_t tx0 = w[s].st.tx;
  w.cli(p1, "fw pull #1"); w.run(240000); w.cli(p2, "fw pull #1"); w.cli(p3, "fw pull #1");
  w.run(5 * 3600000, [&]() { return w[p1].app_fw == 301 && w[p2].app_fw == 301 && w[p3].app_fw == 301; });
  CHECK(w[p2].app_fw == 301 && w[p3].app_fw == 301, "both joiners finished: %s / %s", w.cli(p2, "fw pull-status").c_str(), w.cli(p3, "fw pull-status").c_str());
  const double ratio = (double)(w[s].st.tx - tx0) / nChunks(64000);
  CHECK(ratio < 2.2, "server transmissions per chunk %.2f (3 separate streams ~3.3)", ratio);
  printf("  server tx/chunk %.2f, done at %.0f min\n", ratio, g_sim_now / 60000.0);
}

// R12: the requesting puller dies mid-stream: a listener takes over by itself (never stranded)
static void r_leader_dies() {
  printf("R12 the requesting puller dies mid-stream\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p1 = w.add("P1").idx, p2 = w.add("P2").idx;
  w.link(s, p1, 11); w.link(s, p2, 11); w.link(p1, p2, 11);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(p1, 300, 60000, 1, true); w.seedImage(p2, 300, 60000, 1, true);
  circle(w, {s, p1, p2}); for (int i : {s, p1, p2}) w.cli(i, "fw duty 1");
  w.run(150000); w.cli(p1, "fw peers"); w.cli(p2, "fw peers"); w.run(40000);
  w.cli(p1, "fw pull #1"); w.run(120000); w.cli(p2, "fw pull #1"); w.run(240000);
  CHECK(has(w.cli(p2, "fw pull-status"), "saving shared stream"), "P2 listens: %s", w.cli(p2, "fw pull-status").c_str());
  w[p1].down_until = UINT32_MAX; const uint32_t t0 = g_sim_now;   // P1 is gone (power, antenna)
  if (getenv("SIM_DBG")) for (int k = 0; k < 12; k++) { w.run(60000); auto* x = w[p2].xfer.get();
    printf("   +%dm lis %u gap %u fann_age %d img_age %d stream_ms %u gap_at %u T %u S tx %u | %s\n", k + 1, x->_listening, x->_gap_phase,
      x->_fann_heard_ms ? (int)((g_sim_now - w[p2].boot_ms) - x->_fann_heard_ms) / 1000 : -1, x->_img_heard_ms ? (int)((g_sim_now - w[p2].boot_ms) - x->_img_heard_ms) / 1000 : -1,
      x->_stream_ms, x->_gap_at_ms, x->streamT(), w[s].st.tx, w.cli(p2, "fw pull-status").substr(0, 80).c_str()); }
  // (rc17) P2 carries on by itself: its gaps come as zero-hop repairs from S (its neighbour) or along the path - never stranded
  const uint32_t have0 = w[p2].xfer->_cm.have;
  w.run(5 * 60000);
  CHECK(w[p2].xfer->_cm.have > have0 + 20, "P2 goes on without P1 within 5 min (%u -> %u chunks): %s", have0, w[p2].xfer->_cm.have, w.cli(p2, "fw pull-status").c_str());
  w.run(3 * 3600000, [&]() { return w[p2].app_fw == 301; });
  CHECK(w[p2].app_fw == 301, "P2 finished: %s", w.cli(p2, "fw pull-status").c_str());
  printf("  P2 finished %.0f min after P1 died (repairs by S %u, path escalations %u)\n", (g_sim_now - t0) / 60000.0, ss(w, s).repairs, ss(w, p2).escalations);
}

// R14 (owner 2026-10-07): S - relay - P2 - P3; P3 hears the relay badly. P2 finishes, installs (60 s), restarts, serves; P3's missing chunks
// come from P2 (no relay) rather than from S
static void r_finished_serves() {
  printf("R14 a finished puller serves the far puller's gaps\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r = w.add("R", false).idx, p2 = w.add("P2").idx, p3 = w.add("P3").idx;
  w.link(s, r, 8); w.link(r, p2, 8); w.link(p2, p3, 9); w.link(r, p3, 2, 0.35);
  w.seedImage(s, 301, 48000, 9, true); w.seedImage(p2, 300, 45000, 1, true); w.seedImage(p3, 300, 45000, 1, true);
  circle(w, {s, p2, p3}); for (int i : {s, p2, p3}) w.cli(i, "fw duty 1");
  w.run(150000); w.cli(p2, "fw peers"); w.run(60000); w.cli(p3, "fw peers"); w.run(60000);
  w.cli(p2, "fw pull #1"); w.run(180000); w.cli(p3, "fw pull #1");
  uint32_t t2 = 0;
  w.run(6 * 3600000, [&]() { if (!t2 && w[p2].app_fw == 301) t2 = g_sim_now; return w[p3].app_fw == 301; });
  std::string ps2 = w.cli(p2, "fw pull-status");
  CHECK(w[p3].app_fw == 301, "P3 finished: %s", w.cli(p3, "fw pull-status").c_str());
  CHECK((has(ps2, "reqs=") && !has(ps2, "reqs=0 ")) || ss(w, p2).repairs > 0, "P2 served P3's gaps (windows or rc17 zero-hop repairs: %u): %s", ss(w, p2).repairs, ps2.c_str());
  printf("  P2 installed at %.0f min, P3 done at %.0f min; P2: %s\n", t2 / 60000.0, g_sim_now / 60000.0, ps2.c_str());
}

// ======================= (rc17) fleet fill + gap repair =======================
static uint32_t slotFw(World& w, int i) { enter(w[i]); fw_ota::SlotHeader h; return fw_ota::readSlotHeader(h) && fw_ota::slotValid() ? h.fw_version : 0; }
static uint64_t airAll(World& w) { uint64_t a = 0; for (auto& n : w.nodes) a += n->st.tx_air; return a; }
static uint32_t pctOf(World& w, int i) { auto* x = w[i].xfer.get(); return x && x->_man.n_chunks ? 100u * x->_cm.have / x->_man.n_chunks : 0u; }
static void powerCut(World& w, int i, uint32_t off_ms) { w[i].down_until = g_sim_now + off_ms; w[i].radio.rxq.clear(); w.boot(w[i], 0); }   // flash + files kept
struct FleetNet { int s, r1, r2, r3, p, c1, c2, c3, b1; };
// A field chain: S (server, fw-301) - R1 (LoRa-OTA, relays) - R2 (LoRa-OTA, relays) - R3 (stock relay) -
// P (a remote node, pulls). Hearers off the path: C1 near R1 (empty slot), C2 near R2 (fw-300), C3 near R3 (empty). B1 hears only C2 (a branch).
static FleetNet fleetNet(World& w, double loss) {
  FleetNet f;
  f.s = w.add("S").idx; f.r1 = w.add("R1").idx; f.r2 = w.add("R2").idx; f.r3 = w.add("R3", false).idx; f.p = w.add("P").idx;
  f.c1 = w.add("C1").idx; f.c2 = w.add("C2").idx; f.c3 = w.add("C3").idx; f.b1 = w.add("B1").idx;
  w.link(f.s, f.r1, 8, loss); w.link(f.r1, f.r2, 6, loss); w.link(f.r2, f.r3, 6, loss); w.link(f.r3, f.p, 7, loss);
  w.link(f.c1, f.r1, 8, loss); w.link(f.c2, f.r2, 8, loss); w.link(f.c3, f.r3, 7, loss); w.link(f.b1, f.c2, 9, loss);
  w.seedImage(f.s, 301, 64000, 9, true);
  for (int i : {f.r1, f.r2, f.p, f.c2, f.b1}) w.seedImage(i, 300, 60000, 1, true);
  circle(w, {f.s, f.r1, f.r2, f.p, f.c1, f.c2, f.c3, f.b1});
  for (int i : {f.s, f.r1, f.r2, f.p, f.c1, f.c2, f.c3, f.b1}) w.cli(i, "fw duty 1");
  w.run(150000);
  return f;
}
static std::string idHex(World& w, int i) { char t[12]; const uint8_t* k = w[i].id.pub_key; snprintf(t, sizeof t, "%02X%02X%02X%02X", k[0], k[1], k[2], k[3]); return t; }
static bool pFind(World& w, int p) {   // P scans until it lists fw-301 (3 relays away)
  for (int k = 0; k < 5; k++) {
    w.cli(p, "fw peers"); w.run(90000);
    if (getenv("SIM_DBG")) { for (auto& n : w.nodes) if (n->xfer) printf("    %s: ans sent %u supp %u, srch flood %u zero %u, rx %u lost %u coll %u\n", n->name.c_str(),
      n->xfer->stats.answers_sent, n->xfer->stats.answers_suppressed, n->xfer->stats.searches_flood, n->xfer->stats.searches_zero, n->st.rx, n->st.lost, n->st.collisions);
      printf("    P list: %s | hosts: %s\n", w.cli(p, "fw list").c_str(), w.cli(p, "fw list hosts").c_str()); }
    if (has(w.cli(p, "fw list"), "#1 fw-301")) return true;
  }
  return false;
}
struct FleetRes { double min_all; uint64_t air; uint32_t s_tx, anns, fills, gaps, reps, esc; };
static std::map<int, uint32_t> g_done;                 // node -> minute its slot held fw-301 (P: installed)
static bool trackDone(World& w, std::vector<int> ids, int p, uint32_t t0) {
  bool all = true;
  for (int i : ids) { const bool d = i == p ? w[i].app_fw == 301 : slotFw(w, i) == 301; if (d && !g_done.count(i)) g_done[i] = (g_sim_now - t0) / 60000; all = all && d; }
  return all;
}
static void printDone(World& w) { printf("  done at (min):"); for (auto& e : g_done) printf(" %s %u", w[e.first].name.c_str(), e.second); printf("\n"); }
static FleetRes fleetTotals(World& w, const FleetNet& f, uint32_t t0, uint64_t a0, uint32_t s0) {
  FleetRes r{}; r.min_all = (g_sim_now - t0) / 60000.0; r.air = airAll(w) - a0; r.s_tx = w[f.s].st.tx - s0;
  for (auto& n : w.nodes) if (n->xfer) { auto& x = n->xfer->stats; r.anns += x.fleet_anns; r.fills += x.fills; r.gaps += x.gap_reqs; r.reps += x.repairs; r.esc += x.escalations; }
  return r;
}

// F1 mode 1 (every pull): P pulls fw-301 normally; only the EMPTY slots along the path fill; older images stay; nobody but P installs
static void f_mode1(double loss) {
  printf("F1 fleet fill mode 1 (plain pull) on the owner's chain, %.0f %% loss per link\n", loss * 100);
  World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, loss);
  { const uint64_t sa = airAll(w); w.cli(f.p, "fw peers"); w.run(90000); std::string l = w.cli(f.p, "fw list");   // a scan here: measured, see F5
    printf("  one scan from P: %s (%.1f s of channel air)\n", l.c_str(), (airAll(w) - sa) / 1000.0); }
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w); const uint32_t s0 = w[f.s].st.tx;
  std::string r = w.cli(f.p, "fw pull " + idHex(w, f.s) + " duty 1");   // the owner's command: S by id (rc8 pulled a field relay this way)
  g_done.clear(); w.run(10 * 3600000, [&]() { return trackDone(w, {f.p, f.c1, f.c3}, f.p, t0); }); printDone(w);
  CHECK(w[f.p].app_fw == 301, "P installed fw-301: %s", w.cli(f.p, "fw pull-status").c_str());
  // (rc19, C42 owner) a plain pull's listeners only help while it pulls: C1 / C3 (no neighbour holds fw-301) keep >= 80 % paused - no path
  // pull of their own (rc18 escalated) - and 'fw pull-resume' finishes them
  for (int i : {f.c1, f.c3}) CHECK(slotFw(w, i) == 301 || (pctOf(w, i) >= 80 && has(w.cli(i, "fw pull-status"), "PAUSED")),
                                   "%s filled or paused at >= 80 %%: %s", w[i].name.c_str(), w.cli(i, "fw pull-status").c_str());
  CHECK(ss(w, f.c1).escalations + ss(w, f.c3).escalations == 0, "no path pulls by the listeners (%u)", ss(w, f.c1).escalations + ss(w, f.c3).escalations);
  for (int i : {f.c1, f.c3}) if (slotFw(w, i) != 301) w.cli(i, "fw pull-resume");
  w.run(4 * 3600000, [&]() { return slotFw(w, f.c1) == 301 && slotFw(w, f.c3) == 301; });
  CHECK(slotFw(w, f.c1) == 301 && slotFw(w, f.c3) == 301, "the empty slots near the path filled (after 'fw pull-resume'): C1 %u (%s) C3 %u (%s)", slotFw(w, f.c1),
        w.cli(f.c1, "fw pull-status").c_str(), slotFw(w, f.c3), w.cli(f.c3, "fw pull-status").c_str());
  CHECK(slotFw(w, f.r1) == 300 && slotFw(w, f.r2) == 300 && slotFw(w, f.c2) == 300, "mode 1 leaves older images alone (R1 %u R2 %u C2 %u)",
        slotFw(w, f.r1), slotFw(w, f.r2), slotFw(w, f.c2));
  CHECK(w[f.c1].app_fw == 0 && w[f.c3].app_fw == 0, "a fill never installs (C1 app %u, C3 app %u)", w[f.c1].app_fw, w[f.c3].app_fw);
  CHECK(has(w.cli(f.c1, "fw slot"), "fw-301") && has(w.cli(f.c1, "fw slot"), "not installed here"), "C39 'fw slot' on C1: %s", w.cli(f.c1, "fw slot").c_str());
  FleetRes x = fleetTotals(w, f, t0, a0, s0);
  printf("  all done %.0f min; channel air %.0f s; S tx %u (%.2f/chunk); announces %u, fills %u, gap requests %u, repairs %u, path escalations %u\n",
         x.min_all, x.air / 1000.0, x.s_tx, x.s_tx / 400.0, x.anns, x.fills, x.gaps, x.reps, x.esc);
  printf("  C1: %s\n  C3: %s\n", w.cli(f.c1, "fw slot").c_str(), w.cli(f.c3, "fw slot").c_str());
  w.run(3 * 3600000);                                   // (rc18) a plain pull is never spread hop by hop: the branch keeps its image
  CHECK(slotFw(w, f.b1) == 300, "mode 1 does not spread: B1 (a branch away from every hop) keeps fw-%u", slotFw(w, f.b1));
}

static const double SRC_TX_MAX = 2.6;   // (rc18) S tx per chunk on the chain: 1.7-2.5 over 16 seeds (rc17 and rc18 alike)

// F2 mode 2 ('fleet'): every hearer replaces its older image; restarts and power cuts during the main pull (owner); (rc18) the branch B1
// that hears no hop of the stream gets the image hop by hop from a filled neighbour (holder announce) - slot only; its operator installs
static void f_mode2(double loss, bool restarts) {
  printf("F2 fleet fill mode 2 ('fleet')%s, %.0f %% loss per link\n", restarts ? " + restarts/power cuts during the main pull" : "", loss * 100);
  World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, loss);
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w); const uint32_t s0 = w[f.s].st.tx;
  std::string r = w.cli(f.p, "fw pull " + idHex(w, f.s) + " duty 1 fleet");   // owner: "fw pull 1F2E3D4C duty 1 fleet"
  CHECK(has(r, "fleet"), "the pull says fleet: %s", r.c_str());
  const std::vector<int> hear = {f.r1, f.r2, f.c1, f.c2, f.c3}; g_done.clear();
  auto pct = [&](int i) { auto* x = w[i].xfer.get(); return x && x->_man.n_chunks ? 100u * x->_cm.have / x->_man.n_chunks : 0u; };
  if (restarts) {
    w.run(4 * 3600000, [&]() { return pct(f.c2) >= 30; });   w.boot(w[f.c2], POWER_RESETREAS_SREQ_Msk); w.log("TEST: C2 restarted");
    w.run(4 * 3600000, [&]() { return pct(f.p) >= 45; });    powerCut(w, f.r1, 40000); w.log("TEST: R1 (relay + collector) power cut 40 s");
    w.run(4 * 3600000, [&]() { return pct(f.p) >= 60; });    powerCut(w, f.p, 20000); w.log("TEST: P (requester) power cut 20 s");
    w.run(4 * 3600000, [&]() { return pct(f.c3) >= 75; });   powerCut(w, f.c3, 60000); w.log("TEST: C3 power cut 60 s");
  }
  if (getenv("SIM_DBG")) for (int k = 0; k < 80; k++) {
    w.run(10 * 60000); printf("  +%3u min S tx %u\n", (g_sim_now - t0) / 60000, w[f.s].st.tx - s0);
    for (int i : {f.p, f.r1, f.r2, f.c1, f.c2, f.c3}) { auto* x = w[i].xfer.get(); if (!x) continue;
      printf("    %-2s ph%u fill%u sess%u lis%u gap%u esc%u have %u | %s\n", w[i].name.c_str(), x->_phase, x->_fill, x->_sess_set, x->_listening, x->_gap_phase,
             x->stats.escalations, x->_cm.have, w.cli(i, "fw pull-status").substr(0, 120).c_str()); }
    std::vector<int> v = hear; v.push_back(f.p); if (trackDone(w, v, f.p, t0)) break; }
  if (!restarts) {                                       // (rc17 liveness) collectors wait while the requester still streams: no gap rounds
    auto pdone = [&]() { auto* x = w[f.p].xfer.get(); return w[f.p].app_fw == 301 || (x && x->_man.n_chunks && x->_cm.have >= x->_man.n_chunks); };
    w.run(12 * 3600000, [&]() { std::vector<int> v = hear; v.push_back(f.p); trackDone(w, v, f.p, t0); return pdone(); });
    uint32_t gq = 0; for (int i : hear) gq += w[i].xfer->stats.gap_reqs;
    CHECK(gq <= 100, "collectors sent %u gap requests while P's stream still ran (<= 100; rc18: 61 - a collector the stream brings nothing new for "
          "15 min starts early (newq); without the liveness rule 160)", gq);
  }
  w.run(12 * 3600000, [&]() { std::vector<int> v = hear; v.push_back(f.p); return trackDone(w, v, f.p, t0); }); printDone(w);
  CHECK(w[f.p].app_fw == 301, "P installed fw-301: %s", w.cli(f.p, "fw pull-status").c_str());
  for (int i : hear) CHECK(slotFw(w, i) == 301, "%s filled fw-301 (slot %u): %s", w[i].name.c_str(), slotFw(w, i), w.cli(i, "fw pull-status").c_str());
  for (int i : hear) CHECK(w[i].app_fw != 301, "%s did not install (app %u)", w[i].name.c_str(), w[i].app_fw);
  w.run(6 * 3600000, [&]() { return slotFw(w, f.b1) == 301; });
  CHECK(slotFw(w, f.b1) == 301 && w[f.b1].app_fw != 301, "(rc18) B1 (a branch away from every hop) got it hop by hop: slot %u app %u: %s",
        slotFw(w, f.b1), w[f.b1].app_fw, w.cli(f.b1, "fw pull-status").c_str());
  CHECK(!w[f.p].st.wdt_resets && !w[f.r1].st.wdt_resets && !w[f.c2].st.wdt_resets, "no watchdog resets");
  FleetRes x = fleetTotals(w, f, t0, a0, s0);
  if (!restarts && loss <= 0.11) {                       // (rc17) the cost of the fleet: one stream + small top-ups (a mutant that lets collectors
    CHECK(x.s_tx < SRC_TX_MAX * 400, "S sent %.2f per chunk (< %.1f: one stream + its re-asks)", x.s_tx / 400.0, SRC_TX_MAX);   //   compete for the server fails)
    CHECK(x.esc <= 6, "%u path escalations (<= 6)", x.esc);
  }
  printf("  all 6 done %.0f min; channel air %.0f s; S tx %u (%.2f/chunk); announces %u, fills %u, gap requests %u, repairs %u, path escalations %u\n",
         x.min_all, x.air / 1000.0, x.s_tx, x.s_tx / 400.0, x.anns, x.fills, x.gaps, x.reps, x.esc);
  for (int i : {f.s, f.r1, f.r2, f.r3, f.p, f.c1, f.c2, f.c3}) printf("    %-3s tx %4u (%6.1f s air)  %s\n", w[i].name.c_str(), w[i].st.tx, w[i].st.tx_air / 1000.0,
                                                          w[i].xfer ? w.cli(i, "fw slot").c_str() : "(stock relay)");
  if (restarts) return;
  // the operator installs the spread image on the branch (one 'fw apply', no transfer); C2 still offers fw-301 before its own install
  std::string pl = w.cli(f.b1, "fw peers"); w.run(60000); pl = w.cli(f.b1, "fw list");
  CHECK(has(pl, "fw-301"), "B1 sees fw-301 at its neighbours (served before their install): %s", pl.c_str());
  w.cli(f.b1, "fw apply"); w.run(10 * 60000, [&]() { return w[f.b1].app_fw == 301; });
  CHECK(w[f.b1].app_fw == 301, "B1 installed the spread fw-301 with 'fw apply': %s", w.cli(f.b1, "fw pull-status").c_str());
}

// F5 the main pull alone (every other node opted out): what fleet fill adds on top of it
static void f_alone(double loss) {
  printf("F5 P's pull alone (no collectors), %.0f %% loss\n", loss * 100);
  World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, loss);
  for (int i : {f.r1, f.r2, f.c1, f.c2, f.c3}) w.cli(i, "fw serve off");
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w); const uint32_t s0 = w[f.s].st.tx;
  w.cli(f.p, "fw pull " + idHex(w, f.s) + " duty 1");
  w.run(8 * 3600000, [&]() { return w[f.p].app_fw == 301; });
  CHECK(w[f.p].app_fw == 301, "P alone finished");
  printf("  P alone: %.0f min; channel air %.0f s; S tx %u (%.2f/chunk)\n", (g_sim_now - t0) / 60000.0, (airAll(w) - a0) / 1000.0, w[f.s].st.tx - s0, (w[f.s].st.tx - s0) / 400.0);
  for (int i : {f.s, f.r1, f.r2, f.r3, f.p}) printf("    %-3s tx %4u (%6.1f s air)\n", w[i].name.c_str(), w[i].st.tx, w[i].st.tx_air / 1000.0);
}

// F0 baseline: the same chain without fleet fill, the strongest manual way - each node pulls BY ID from its nearest holder, one after another
// (R1 <- S, R2 <- R1, P <- R2 through R3, C1 <- R1, C2 <- R2, C3 <- P through R3)
static void f_baseline(double loss) {
  printf("F0 baseline: 6 pulls one after another, each by id from its nearest holder (no fleet fill), %.0f %% loss\n", loss * 100);
  World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, loss);
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w); const uint32_t s0 = w[f.s].st.tx;
  int done = 0; g_done.clear();
  for (int i : {f.r1, f.r2, f.p, f.c1, f.c2, f.c3}) w.cli(i, "fw serve off");   // today's way: no fleet fill (the opt-out) until its own pull
  const int order[6][2] = {{f.r1, f.s}, {f.r2, f.r1}, {f.p, f.r2}, {f.c1, f.r1}, {f.c2, f.r2}, {f.c3, f.p}};
  for (auto& o : order) {
    const int i = o[0];
    std::string r0 = w.cli(i, "fw pull " + idHex(w, o[1]) + " duty 1");
    if (getenv("SIM_DBG")) { printf("  %s pull from %s: %s\n", w[i].name.c_str(), w[o[1]].name.c_str(), r0.c_str());
      for (int k = 0; k < 12 && w[i].app_fw != 301; k++) { w.run(5 * 60000); printf("    +%2d: %s | src: %s\n", 5 * (k + 1), w.cli(i, "fw pull-status").substr(0, 110).c_str(), w.cli(o[1], "fw pull-status").substr(0, 90).c_str()); } }
    w.run(6 * 3600000, [&]() { return w[i].app_fw == 301; });
    if (getenv("SIM_DBG") && w[i].app_fw != 301) { printf("  %s app %u slot: %s | ctl %s\n", w[i].name.c_str(), w[i].app_fw, w.cli(i, "fw slot").c_str(), w.cli(i, "fw ctl").c_str());
      for (auto& e : w.events) if (e.find(w[i].name + ":") != std::string::npos || e.find(w[i].name + " > fw pull") != std::string::npos) printf("    %s\n", e.substr(0, 160).c_str()); }
    done += w[i].app_fw == 301; g_done[i] = (g_sim_now - t0) / 60000;
    w.run(600000, [&]() { return !w[i].app_pending; }); w.run(5000);
    w.cli(i, "fw serve on"); w.run(60000);                                 // it serves the next one (once its install is done)
  }
  printDone(w);
  CHECK(done == 6, "baseline: all 6 pulled one by one (%d)", done);
  printf("  6 sequential pulls: %.0f min; channel air %.0f s; S tx %u\n", (g_sim_now - t0) / 60000.0, (airAll(w) - a0) / 1000.0, w[f.s].st.tx - s0);
}

// F3 safety (owner 9.2): mode 2 never touches a slot waiting for its apply, one serving a pull right now, an equal or a newer image, nor a node
// with 'fw serve off' (the opt-out); a CRITICAL battery does not collect
static void f_safety() {
  printf("F3 fleet fill safety rules\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p = w.add("P").idx, a = w.add("Apending").idx, b = w.add("Bserving", true, false).idx, q = w.add("Q").idx,
      c = w.add("Cequal").idx, d = w.add("Dnewer").idx, e = w.add("Eoptout").idx, g = w.add("Gcrit").idx, h = w.add("Hok").idx;
  std::vector<int> all = {s, p, a, b, q, c, d, e, g, h};
  for (int i : all) for (int j : all) if (i < j && !(i == q || j == q)) w.link(i, j, 10);
  w.link(b, q, 10);                                      // Q pulls fw-300 from B (B's only neighbour) during the fleet stream
  w.seedImage(s, 301, 48000, 9, true); w.seedImage(c, 301, 48000, 9, true); w.seedImage(d, 302, 49000, 3, true);
  for (int i : {p, a, b, e, g, h}) w.seedImage(i, 300, 45000, 1, true);
  w.seedImage(q, 299, 44000, 4, true);
  circle(w, all); for (int i : all) w.cli(i, "fw duty 1");
  w.cli(e, "fw serve off"); w.cli(g, "fw minbatt 3400"); w[g].batt_mv = 3100; w[g].xfer->noteBattery(3100);
  enter(w[a]); fw_ota::ctrlMarkPending();               // A: its slot waits for the apply (not done yet)
  w.run(150000);
  w.cli(q, "fw pull " + idHex(w, b));                     // B (no relay) serves Q fw-300 - busy during the fleet stream
  w.run(60000);
  w.cli(p, "fw peers"); w.run(40000);
  CHECK(has(w.cli(p, "fw list"), "#1 fw-302"), "P lists: %s", w.cli(p, "fw list").c_str());
  std::string l = w.cli(p, "fw list"); int n = has(l, "#2 fw-301") ? 2 : 1;
  w.cli(p, ("fw pull #" + std::to_string(n) + " fleet").c_str());
  bool b_overwritten = false;                             // B must not start a fill while Q's pull from it runs
  w.run(4 * 3600000, [&]() { if (w[b].xfer->stats.fills && w[q].app_fw != 300) b_overwritten = true; return w[p].app_fw == 301 && slotFw(w, h) == 301 && w[q].app_fw == 300; });
  CHECK(slotFw(w, h) == 301, "the control node H filled: %s", w.cli(h, "fw pull-status").c_str());
  CHECK(slotFw(w, a) == 300, "A (waiting for its apply) untouched: slot %u", slotFw(w, a));
  CHECK(slotFw(w, c) == 301 && w[c].xfer->stats.fills == 0, "C (equal) untouched");
  CHECK(slotFw(w, d) == 302, "D (newer) untouched: slot %u", slotFw(w, d));
  CHECK(slotFw(w, e) == 300, "E ('fw serve off' = opt-out) untouched: slot %u", slotFw(w, e));
  CHECK(slotFw(w, g) == 300, "G (CRITICAL battery) untouched: slot %u", slotFw(w, g));
  CHECK(!b_overwritten, "B (serving Q) was not overwritten before Q finished");
  CHECK(w[q].app_fw == 300, "Q finished its own pull from B: %s", w.cli(q, "fw pull-status").c_str());
}

// F4 where gaps come from (owner 9.3): (a) a LOCAL fade - L hears the server badly but P well: its gaps come from P (zero-hop), not along the
// path; (b) an UPSTREAM loss - S->R drops 20 %: P re-asks along the path and every collector downstream fills from those re-sends
static void f_gaps() {
  printf("F4 gap repair: local fade vs upstream loss\n");
  { World w; W = &w; g_sim_now = 0;
    int s = w.add("S").idx, p = w.add("P").idx, l = w.add("L").idx;
    w.link(s, p, 10); w.link(p, l, 10); w.link(s, l, 2, 0.35);
    w.seedImage(s, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true);
    circle(w, {s, p, l}); for (int i : {s, p, l}) w.cli(i, "fw duty 1");
    w.run(150000); w.cli(p, "fw peers"); w.run(40000);
    const uint32_t s0 = w[s].st.tx;
    w.cli(p, "fw pull #1 fleet");
    w.run(4 * 3600000, [&]() { return w[p].app_fw == 301 && slotFw(w, l) == 301; });
    CHECK(slotFw(w, l) == 301, "(a) L filled: %s", w.cli(l, "fw pull-status").c_str());
    CHECK(ss(w, l).escalations == 0, "(a) L's gaps came from its neighbour, not along the path (%u escalations)", ss(w, l).escalations);
    CHECK(ss(w, p).repairs > 2 * ss(w, s).repairs, "(a) the good link repaired (P %u, the lossy server %u)", ss(w, p).repairs, ss(w, s).repairs);
    printf("  (a) local fade: S tx/chunk %.2f; repairs by P %u, by S %u; L asked %u times\n", (w[s].st.tx - s0) / 400.0, ss(w, p).repairs, ss(w, s).repairs, ss(w, l).gap_reqs);
  }
  { World w; W = &w; g_sim_now = 0;
    int s = w.add("S").idx, r = w.add("R", false).idx, p = w.add("P").idx, q1 = w.add("Q1").idx, q2 = w.add("Q2").idx;
    w.link(s, r, 8, 0.2); w.link(r, p, 8); w.link(r, q1, 8); w.link(r, q2, 8); w.link(p, q1, 9); w.link(q1, q2, 9);
    w.seedImage(s, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true);
    circle(w, {s, p, q1, q2}); for (int i : {s, p, q1, q2}) w.cli(i, "fw duty 1");
    w.run(150000);
    for (int k = 0; k < 4 && !has(w.cli(p, "fw list"), "fw-301"); k++) { w.cli(p, "fw peers"); w.run(90000); }
    const uint32_t s0 = w[s].st.tx;
    w.cli(p, "fw pull #1");
    w.run(6 * 3600000, [&]() { return w[p].app_fw == 301 && slotFw(w, q1) == 301 && slotFw(w, q2) == 301; });
    CHECK(slotFw(w, q1) == 301 && slotFw(w, q2) == 301, "(b) Q1/Q2 filled: %s / %s", w.cli(q1, "fw pull-status").c_str(), w.cli(q2, "fw pull-status").c_str());
    printf("  (b) upstream loss 20 %%: S tx/chunk %.2f (P's re-asks along the path fill everyone); escalations Q1 %u Q2 %u; repairs P %u Q1 %u Q2 %u\n",
           (w[s].st.tx - s0) / 400.0, ss(w, q1).escalations, ss(w, q2).escalations, ss(w, p).repairs, ss(w, q1).repairs, ss(w, q2).repairs);
  }
}

// C37 / C38 / C39 texts on a node
static void f_cli() {
  printf("C37-C39 commands\n");
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p = w.add("P").idx; w.link(s, p, 10);
  w.seedImage(s, 301, 32000, 9, true); w.seedImage(p, 300, 30000, 1, true);
  circle(w, {s, p}); w.run(20000);
  std::string l0 = w.cli(p, "fw list");
  CHECK(has(l0, "no scan yet") && w[p].st.tx == 0, "C38 'fw list' before a scan sends nothing: %s", l0.c_str());
  w.cli(p, "fw peers"); w.run(35000);
  CHECK(has(w.cli(p, "fw list"), "#1 fw-301 x1 (0hop)"), "C38 'fw peers' scanned, 'fw list' shows: %s", w.cli(p, "fw list").c_str());
  CHECK(has(w.cli(p, "fw list hosts"), "#1 fw-301"), "C38 'fw list hosts': %s", w.cli(p, "fw list hosts").c_str());
  std::string sl = w.cli(s, "fw slot");
  CHECK(has(sl, "slot: app fw-301, 32000 B, crc ") && has(sl, "installed here, offered to neighbours"), "C39 fw slot: %s", sl.c_str());
  const uint32_t tx0 = w[p].st.tx;
  std::string r = w.cli(p, "fw pull #1 duty 1 pause 2h");
  CHECK(has(r, "paused") && has(r, "2 h"), "C37 one command: %s", r.c_str());
  w.run(30 * 60000);
  CHECK(w[p].st.tx == tx0 && has(w.cli(p, "fw pull-status"), "PAUSED"), "C37 nothing sent before the first offer (%u): %s", w[p].st.tx - tx0, w.cli(p, "fw pull-status").c_str());
  CHECK(has(w.cli(p, "fw slot"), "fw-300"), "the slot untouched while paused: %s", w.cli(p, "fw slot").c_str());
  w.run(4 * 3600000, [&]() { return w[p].app_fw == 301; });
  CHECK(w[p].app_fw == 301, "C37 the pull started by itself after 2 h: %s", w.cli(p, "fw pull-status").c_str());
  // (F1) the longest status line: every counter at its maximum + the longest reason - the reason is shown whole or not at all, never cut
  { enter(w[p]); auto& x = *w[p].xfer; x._phase = fw_ota::PH_DOWNLOADING; x._man.n_chunks = 2176; x._cm.reset(2176); for (int i = 0; i < 2175; i++) x._cm.set(i);
    strcpy(x._man.name, "@ABCDEF12"); x._retries = 255; x._crc_bad = 4294967295u; x._crc_bad_idx = 2175; x._mac_fail = 4294967295u;
    x._route_len = 6; x._listening = false; x._gap_phase = false; x._sess_set = true; x._rate_t0_ms = 0; x._rate_have0 = 0;
    strcpy(x._last_err, "gaps: asking the stream's server (path) 123456"); char r[200] = {0}; x.cmdStatus(r);
    CHECK(strlen(r) < 159 && (!has(r, "gaps") || has(r, "123456")), "F1 the longest status line is not cut (%zu B): %s", strlen(r), r);
    x._phase = fw_ota::PH_IDLE; x._man.n_chunks = 0; x._cm.reset(0); }
}

// ======================= (algorithm study, owner 2026-10-08) gap-fill algorithms A, C, D, E on four networks =======================
// usage: sim STUDY net=T1|T2|T3|T4 algo=A|C|D|E [seed=N] [chunks=N] [hours=N] [tune=value ...]
//   T1 today's chain (S - R1 - R2 - stock R3 - P, hearers C1-C3), 10 % loss per link
//   T2 the owner's valley B1 - R1 - R2 - R3s - R4 - R5 - PB - R6s - R7 - MDP (s = stock relay) with weak skip links (R2 hears B1, PB hears
//      R2 better than R4, ...), side nodes N1-N4; MDP pulls fw-301 from B1 with 'fleet'
//   T3 T2 + fading links (minutes-long fades on every link) + solar batteries (start 16:00, a cloudy day 2, R4/R6s/N2 nearly empty)
//   T4 T2 with a weak mountain end: R6s - R7 20 %, R7 - MDP 30 %, both fading
struct StudyNet { std::vector<int> all, lota, targets; int src = -1, req = -1; };
static StudyNet studyBuild(World& w, const std::string& net, uint32_t chunks) {
  StudyNet n; auto add = [&](const char* nm, bool lota) { int i = w.add(nm, lota).idx; n.all.push_back(i); if (lota) n.lota.push_back(i); return i; };
  const uint32_t len = chunks * fw_ota::XFER_CHUNK;
  if (net == "T1") {
    int s = add("S", true), r1 = add("R1", true), r2 = add("R2", true), r3 = add("R3s", false), p = add("P", true);
    int c1 = add("C1", true), c2 = add("C2", true), c3 = add("C3", true);
    w.link(s, r1, 8, 0.1); w.link(r1, r2, 6, 0.1); w.link(r2, r3, 6, 0.1); w.link(r3, p, 7, 0.1);
    w.link(c1, r1, 8, 0.1); w.link(c2, r2, 8, 0.1); w.link(c3, r3, 7, 0.1);
    n.src = s; n.req = p;
  } else {
    int b1 = add("B1", true), r1 = add("R1", true), r2 = add("R2", true), r3 = add("R3s", false), r4 = add("R4", true), r5 = add("R5", true),
        pb = add("PB", true), r6 = add("R6s", false), r7 = add("R7", true), md = add("MDP", true);
    int n1 = add("N1", true), n2 = add("N2", true), n3 = add("N3", true), n4 = add("N4", true);
    const double L = 0.1;
    w.link(b1, r1, 8, L); w.link(r1, r2, 6, L); w.link(r2, r3, 6, L); w.link(r3, r4, 6, L); w.link(r4, r5, 7, L); w.link(r5, pb, 6, L);
    w.link(pb, r6, 6, L); w.link(r6, r7, 6, net == "T4" ? 0.2 : L); w.link(r7, md, 5, net == "T4" ? 0.3 : L);
    w.link(b1, r2, 0, 0.45); w.link(r2, pb, -2, 0.55); w.link(r4, pb, -6, 0.8); w.link(r3, r5, -3, 0.6); w.link(pb, r7, -5, 0.7); w.link(r6, md, -7, 0.85);
    w.link(n1, r1, 8, L); w.link(n1, r2, 4, 0.25); w.link(n2, r4, 6, L); w.link(n2, r5, 2, 0.35); w.link(n3, pb, 7, L); w.link(n3, r6, 3, 0.3);
    w.link(n4, r7, 2, 0.35);
    if (net == "T3") for (auto& l : w.links) if (l.first.first < l.first.second) w.fade(l.first.first, l.first.second, 2400000, 240000, 0.8);
    if (net == "T4") { w.fade(r6, r7, 1800000, 300000, 0.85); w.fade(r7, md, 1800000, 300000, 0.85); }
    if (net == "T3") {
      w.day_off_ms = 16 * 3600000u - 300000u;          // the pull starts ~16:00
      w.weather = {0.6, 0.15, 0.8, 1.0, 1.0, 0.5};
      for (int i : n.all) { Battery& b = w[i].bat; b.on = true; b.mah = 0.6 * b.cap_mah; }
      w[md].bat.cap_mah = 25000; w[md].bat.mah = 15000; w[md].bat.peak_chg_ma = 1000;
      w[r4].bat.mah = 0.04 * w[r4].bat.cap_mah; w[r6].bat.mah = 0.03 * w[r6].bat.cap_mah; w[n2].bat.mah = 0.09 * w[n2].bat.cap_mah;
    }
    n.src = b1; n.req = md;
  }
  w.seedImage(n.src, 301, len, 9, true);
  for (int i : n.lota) if (i != n.src) { w.seedImage(i, 300, len - 4000, 1, true); n.targets.push_back(i); }
  circle(w, n.lota);
  for (int i : n.lota) { w.cli(i, "fw duty 1"); if (w[i].bat.on) w.cli(i, "fw minbatt 3200"); }
  return n;
}

static int study(int argc, char** argv) {
  std::map<std::string, std::string> a;
  for (int i = 2; i < argc; i++) { std::string t = argv[i]; size_t e = t.find('='); if (e != std::string::npos) a[t.substr(0, e)] = t.substr(e + 1); }
  auto num = [&](const char* k, double d) { return a.count(k) ? atof(a[k].c_str()) : d; };
  const std::string net = a.count("net") ? a["net"] : "T2", alg = a.count("algo") ? a["algo"] : "E";   // (rc18) default = G (algo E + the defaults)
  fw_ota::SwarmTune& t = fw_ota::g_tune; t = fw_ota::SwarmTune();
  t.algo = alg == "C" ? fw_ota::ALGO_C : alg == "D" ? fw_ota::ALGO_D : alg == "E" ? fw_ota::ALGO_E : fw_ota::ALGO_A;
  t.gap_max = (uint8_t)num("gap_max", t.gap_max); t.gap_round_ms = (uint32_t)(num("gap_round", t.gap_round_ms / 1000.0) * 1000);
  t.gap_dry_max = (uint8_t)num("dry", t.gap_dry_max); t.rep_snr_ms = (uint32_t)num("snr_ms", t.rep_snr_ms);
  t.elect_span_ms = (uint32_t)(num("span", t.elect_span_ms / 1000.0) * 1000); t.elect_settle_ms = (uint32_t)(num("settle", t.elect_settle_ms / 1000.0) * 1000);
  t.elect_defer_ms = (uint32_t)(num("defer", t.elect_defer_ms / 60000.0) * 60000); t.elect_rounds = (uint8_t)num("rounds", t.elect_rounds);
  t.claim_flood = num("claim_flood", t.claim_flood) != 0;
  t.tr_imin_ms = (uint32_t)(num("imin", t.tr_imin_ms / 1000.0) * 1000); t.tr_imax_ms = (uint32_t)(num("imax", t.tr_imax_ms / 60000.0) * 60000);
  t.tr_k = (uint8_t)num("k", t.tr_k); t.e_fallback_ms = (uint32_t)(num("fallback", t.e_fallback_ms / 60000.0) * 60000);
  t.gossip_life_ms = (uint32_t)(num("life", t.gossip_life_ms / 3600000.0) * 3600000); t.batt_rest = num("rest", t.batt_rest) != 0;
  t.req_adopt = num("adopt", t.req_adopt) != 0; t.newq_ms = (uint32_t)(num("newq", t.newq_ms / 60000.0) * 60000);   // (rc18) unset keys = the defaults
  t.e_spread = num("spread", t.e_spread) != 0; t.src_spread = num("srcspread", t.src_spread) != 0;
  t.move_eval = num("meval", t.move_eval) != 0;   // (rc20, C48) 0 = rc19
  g_seed = (uint32_t)num("seed", 1);
  const uint32_t chunks = std::max<uint32_t>(40, (uint32_t)num("chunks", 400)), hours = (uint32_t)num("hours", 72);
  World w; W = &w; g_sim_now = 0; w.chance = Rng(12345 + 104729ULL * g_seed);
  StudyNet n = studyBuild(w, net, chunks);
  w.run(150000);
  const uint32_t t0 = g_sim_now; const uint32_t s0 = w[n.src].st.tx;
  std::vector<uint64_t> air0, col0; for (auto& x : w.nodes) { air0.push_back(x->st.tx_air); col0.push_back(x->st.collisions); }
  std::string r = w.cli(n.req, "fw pull " + idHex(w, n.src) + " duty 1 fleet");
  std::map<int, double> done;
  auto check = [&]() { bool all = true;
    for (int i : n.targets) { if (done.count(i)) continue; const bool d = i == n.req ? w[i].app_fw == 301 : slotFw(w, i) == 301;
      if (d) done[i] = (g_sim_now - t0) / 60000.0; else all = false; }
    return all; };
  uint32_t last = 0;
  w.run(hours * 3600000u, [&]() { if (g_sim_now - last < 60000) return false; last = g_sim_now; return check(); });
  check();
  const uint32_t t_end = g_sim_now;
  uint64_t air = 0, col = 0, mx = 0; int mxi = -1;
  for (auto& x : w.nodes) { const uint64_t d = x->st.tx_air - air0[x->idx]; air += d; col += x->st.collisions - col0[x->idx]; if (d > mx) { mx = d; mxi = x->idx; } }
  fw_ota::FwOtaTransfer::SwarmStats S{};
  for (auto& x : w.nodes) if (x->xfer) { auto& q = x->xfer->stats; S.gap_reqs += q.gap_reqs; S.repairs += q.repairs; S.escalations += q.escalations;
    S.claims += q.claims; S.claim_floods += q.claim_floods; S.claims_supp += q.claims_supp; S.elected += q.elected; S.summs += q.summs; S.rests += q.rests; S.fills += q.fills; }
  const uint32_t s_tx = w[n.src].st.tx - s0;
  // after the last node: what the swarm still sends by itself (2 h)
  const uint64_t a1 = airAll(w); w.run(2 * 3600000u); const uint64_t after = airAll(w) - a1;
  uint32_t deaths = 0, er = 0, ermax = 0; double dead_h = 0, minpct = 100; std::string minn = "-";
  for (auto& x : w.nodes) { er += x->st.erases; ermax = std::max(ermax, x->st.erases);
    if (x->bat.on) { deaths += x->bat.deaths; dead_h += (x->bat.dead_ms + (x->bat.dead ? g_sim_now - x->bat.dead_since : 0)) / 3600000.0;
      const double pc = 100.0 * x->bat.min_mah / x->bat.cap_mah; if (pc < minpct) { minpct = pc; minn = x->name; } } }
  double tmax = 0; for (auto& d : done) tmax = std::max(tmax, d.second);
  printf("RESULT net=%s algo=%s seed=%u chunks=%u done=%zu/%zu time_min=%.0f air_s=%.0f maxtx_s=%.0f maxtx_node=%s coll=%llu src_tx_chunk=%.2f "
         "gap_reqs=%u repairs=%u esc=%u claims=%u claim_floods=%u claims_supp=%u elected=%u summs=%u rests=%u fills=%u after2h_s=%.1f "
         "deaths=%u dead_h=%.1f min_batt_pct=%.1f min_batt_node=%s erases=%u erases_max=%u",
         net.c_str(), alg.c_str(), g_seed, chunks, done.size(), n.targets.size(), done.size() == n.targets.size() ? tmax : -1.0, air / 1000.0, mx / 1000.0,
         mxi >= 0 ? w[mxi].name.c_str() : "-", (unsigned long long)col, s_tx / (double)chunks, S.gap_reqs, S.repairs, S.escalations, S.claims, S.claim_floods,
         S.claims_supp, S.elected, S.summs, S.rests, S.fills, after / 1000.0, deaths, dead_h, minpct, minn.c_str(), er, ermax);
  for (auto& kv : a) if (kv.first != "net" && kv.first != "algo" && kv.first != "seed" && kv.first != "chunks") printf(" %s=%s", kv.first.c_str(), kv.second.c_str());
  printf("\n  pull: %s\n  done (min):", r.c_str()); for (int i : n.targets) { if (done.count(i)) printf(" %s %.0f", w[i].name.c_str(), done[i]); else printf(" %s -", w[i].name.c_str()); }
  printf("\n  tx air (s):"); for (auto& x : w.nodes) printf(" %s %.0f", x->name.c_str(), (x->st.tx_air - air0[x->idx]) / 1000.0);
  if (net == "T3") { printf("\n  battery min %% / deaths:"); for (auto& x : w.nodes) printf(" %s %.0f/%u", x->name.c_str(), 100.0 * x->bat.min_mah / x->bat.cap_mah, x->bat.deaths); }
  printf("\n");
  if (getenv("SIM_DBG")) for (int i : n.targets) if (!done.count(i)) printf("  %s: %s | %s\n", w[i].name.c_str(), w.cli(i, "fw pull-status").c_str(), w.cli(i, "fw slot").c_str());
  (void)t_end;
  return 0;
}

// G1-G3 (rc18, docs/GAPFILL-ALGO-STUDY-20261008.md): the owner's valley B1 .. PB .. MDP with skip links, side nodes and stock relays, the
// rc18 defaults. G1 T2: MDP's pull by id finishes from a nearer holder of the same image (adopt). G2 T3: solar batteries from 16:00, R4
// CRITICAL at the start - it rests, and gets the image when the sun is back (48 h gossip window). G3 T4: a fading tail where the source's
// answers get lost on the first-found route - the source gossips from the fleet offer, the image moves hop by hop.
static void g_valley(const char* tag, const char* net, uint32_t seed) {
  printf("%s valley %s (seed %u): fleet pull B1 -> MDP, rc18 gap fill\n", tag, net, seed);
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = seed;
  World w; W = &w; g_sim_now = 0; w.chance = Rng(12345 + 104729ULL * g_seed);
  StudyNet n = studyBuild(w, net, 400);
  w.run(150000);
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w);
  w.cli(n.req, "fw pull " + idHex(w, n.src) + " duty 1 fleet");
  auto ok = [&](int i) { return i == n.req ? w[i].app_fw == 301 : slotFw(w, i) == 301; };
  auto all = [&]() { for (int i : n.targets) if (!ok(i)) return false; return true; };
  uint32_t last = 0;
  w.run(72 * 3600000u, [&]() { if (g_sim_now - last < 60000) return false; last = g_sim_now; return all(); });
  std::string miss; for (int i : n.targets) if (!ok(i)) miss += " " + w[i].name + " (" + w.cli(i, "fw pull-status").substr(0, 100) + ")";
  CHECK(all(), "all %zu LoRa-OTA nodes have fw-301 within 72 h (%.1f h); missing:%s", n.targets.size(), (g_sim_now - t0) / 3600000.0, miss.c_str());
  uint32_t deaths = 0, rests = 0; for (int i : n.lota) { deaths += w[i].bat.deaths; if (w[i].xfer) rests += w[i].xfer->stats.rests; }
  CHECK(deaths == 0, "none of our nodes ran flat (%u battery deaths)", deaths);
  if (!strcmp(net, "T3")) {
    int r4 = -1; for (int i : n.lota) if (w[i].name == "R4") r4 = i;
    CHECK(slotFw(w, r4) == 301, "R4 (4 %% at the start) got fw-301 after the sun brought it back: slot %u", slotFw(w, r4));
  }
  printf("  all done %.1f h; channel air %.0f s; source tx/chunk %.2f; deaths %u, rests %u\n", (g_sim_now - t0) / 3600000.0,
         (airAll(w) - a0) / 1000.0, w[n.src].xfer ? (w[n.src].st.tx) / 400.0 : 0.0, deaths, rests);
}

// G4 (rc18): a fleet pull by id survives a reboot before its first answer - the next offers still say "fleet" (the source may spread)
static void g_fleet_reboot() {
  printf("G4 fleet pull: the fleet flag survives a reboot before the first answer\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  w.cli(f.s, "fw serve off");                            // nobody answers yet
  w.cli(f.p, "fw pull " + idHex(w, f.s) + " duty 1 fleet"); w.run(60000);
  w.boot(w[f.p], POWER_RESETREAS_SREQ_Msk); w.run(60000);
  CHECK(w[f.p].xfer->_fleet_mode == fw_ota::FLEET_REPLACE, "P's pull is still a fleet pull after the reboot: %s", w.cli(f.p, "fw pull-status").c_str());
  w.cli(f.s, "fw serve on"); w.run(12 * 3600000, [&]() { return w[f.p].app_fw == 301 && slotFw(w, f.b1) == 301; });
  CHECK(w[f.p].app_fw == 301 && slotFw(w, f.b1) == 301, "P installed and the branch B1 got the image by spread: P app %u, B1 slot %u",
        w[f.p].app_fw, slotFw(w, f.b1));
}

// G5 (rc18, owner: battery protection = stability): a puller whose battery turns CRITICAL mid-pull sends nothing until it recovers,
// then finishes (chunks kept)
static void g_rest() {
  printf("G5 battery rest: CRITICAL mid-pull -> silent, recovers -> finishes\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  w.cli(f.p, "fw minbatt 3200"); w[f.p].batt_mv = 3300; w[f.p].xfer->noteBattery(3300);
  w.cli(f.p, "fw pull " + idHex(w, f.s) + " duty 1");
  w.run(4 * 3600000, [&]() { auto* x = w[f.p].xfer.get(); return x->_man.n_chunks && x->_cm.have * 10 >= x->_man.n_chunks * 3; });
  w[f.p].batt_mv = 2950; w[f.p].xfer->noteBattery(2950);   // below minbatt - 150 mV: CRITICAL
  w.run(5 * 60000);                                      // (what was queued goes out)
  const uint32_t tx0 = w[f.p].st.tx; const uint16_t h0 = w[f.p].xfer->_cm.have;
  w.run(3600000);
  CHECK(w[f.p].st.tx - tx0 <= 2, "P sent %u packets in the hour it was CRITICAL (<= 2: an advert at most)", w[f.p].st.tx - tx0);
  CHECK(w[f.p].xfer->stats.rests > 0, "P rested (%u): %s", w[f.p].xfer->stats.rests, w.cli(f.p, "fw pull-status").c_str());
  w[f.p].batt_mv = 3300; w[f.p].xfer->noteBattery(3300);
  w.run(8 * 3600000, [&]() { return w[f.p].app_fw == 301; });
  CHECK(w[f.p].app_fw == 301, "P finished after its battery recovered (it had %u chunks): %s", h0, w.cli(f.p, "fw pull-status").c_str());
}

// ======================= (rc19) C42 fleet cancel + passive fills, C44 gaps behind the stream, C45-C47 texts =======================
static uint32_t txOf(World& w, int i) { return w[i].st.tx; }

// H1 (C42, owner): P's fleet pull is aborted with 'fw pull-abort' - the listeners pause (chunks kept, nothing sent, no repairs at the
// server); L2 hears the server badly (repeats, the server's echo and L1's pass-on reach it). Then Q starts a fleet pull of the same image:
// the paused fills continue from its stream and finish (slot only).
static void h_cancel() {
  printf("H1 (C42) fleet cancel: the listeners pause; another requester's stream continues them\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r = w.add("R", false).idx, p = w.add("P").idx, l1 = w.add("L1").idx, l2 = w.add("L2").idx, q = w.add("Q").idx;
  w.link(s, r, 8); w.link(r, p, 7); w.link(s, l1, 9); w.link(s, l2, 5, 0.3); w.link(l1, l2, 9); w.link(s, q, 8);
  w.seedImage(s, 301, 64000, 9, true); for (int i : {p, l1, l2, q}) w.seedImage(i, 300, 60000, 1, true);
  circle(w, {s, p, l1, l2, q}); for (int i : {s, p, l1, l2, q}) w.cli(i, "fw duty 1");
  w.run(150000);
  w.cli(p, "fw pull " + idHex(w, s) + " duty 1 fleet");
  w.run(4 * 3600000, [&]() { return pctOf(w, l1) >= 30 && pctOf(w, l2) >= 20; });
  std::string ab = w.cli(p, "fw pull-abort");
  CHECK(has(ab, "the fleet stops too"), "the abort says the fleet stops: %s", ab.c_str());
  w.run(10 * 60000);
  for (int i : {l1, l2}) CHECK(has(w.cli(i, "fw pull-status"), "PAUSED") && has(w.cli(i, "fw slot"), "partial fw-301"),
                               "%s paused: %s | %s", w[i].name.c_str(), w.cli(i, "fw pull-status").c_str(), w.cli(i, "fw slot").c_str());
  uint32_t rep0 = 0; for (auto& n : w.nodes) if (n->xfer) rep0 += n->xfer->stats.repairs;
  const uint32_t t1 = txOf(w, l1), t2 = txOf(w, l2), g1 = ss(w, l1).gap_reqs + ss(w, l2).gap_reqs, e1 = ss(w, l1).escalations + ss(w, l2).escalations;
  w.run(2 * 3600000);
  uint32_t rep1 = 0; for (auto& n : w.nodes) if (n->xfer) rep1 += n->xfer->stats.repairs;
  CHECK(txOf(w, l1) - t1 <= 2 && txOf(w, l2) - t2 <= 2, "paused fills sent nothing in 2 h (L1 %u, L2 %u packets)", txOf(w, l1) - t1, txOf(w, l2) - t2);
  CHECK(rep1 == rep0 && ss(w, l1).gap_reqs + ss(w, l2).gap_reqs == g1 && ss(w, l1).escalations + ss(w, l2).escalations == e1,
        "no gap requests, repairs or path pulls after the cancel (repairs +%u)", rep1 - rep0);
  CHECK(w[l1].app_fw == 300 && w[l2].app_fw == 300, "nothing installed");
  // another requester, the same image: the paused fills continue from its stream (owner)
  w.cli(q, "fw pull " + idHex(w, s) + " duty 1 fleet");
  w.run(6 * 3600000, [&]() { return slotFw(w, l1) == 301 && slotFw(w, l2) == 301 && w[q].app_fw == 301; });
  CHECK(slotFw(w, l1) == 301 && slotFw(w, l2) == 301, "the paused fills finished from Q's stream: L1 %s | L2 %s", w.cli(l1, "fw pull-status").c_str(), w.cli(l2, "fw pull-status").c_str());
  CHECK(w[q].app_fw == 301 && w[l1].app_fw == 300 && w[l2].app_fw == 300, "Q installed, the fills did not (Q %u L1 %u L2 %u)", w[q].app_fw, w[l1].app_fw, w[l2].app_fw);
}

// H2 (C42, owner): a PLAIN pull's listeners help only while its puller pulls. L hears S one way (its gap requests reach nobody): no path
// pull, it pauses an hour after P's last announce and stays silent; 'fw pull-resume' then finishes it from the nearest holder. M misses
// > 20 %: paused at once; a NEWER image's stream later replaces its partial.
static void h_passive() {
  printf("H2 (C42) plain pull: listeners fade with their puller, pause, 'fw pull-resume' finishes; a newer stream replaces a partial\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p = w.add("P").idx, l = w.add("L").idx, m = w.add("M").idx, z = w.add("Z").idx, p2 = w.add("P2").idx;
  w.link(s, p, 9); w.link(s, l, 7, 0.12, false); w.link(s, m, 5, 0.45, false); w.link(z, m, 9); w.link(z, p2, 9);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true); w.seedImage(z, 302, 66000, 5, true);
  circle(w, {s, p, l, m, z, p2}); for (int i : {s, p, l, m, z, p2}) w.cli(i, "fw duty 1");
  w.run(150000);
  w.cli(p, "fw pull " + idHex(w, s) + " duty 1");
  w.run(4 * 3600000, [&]() { return w[p].app_fw == 301; });
  CHECK(w[p].app_fw == 301, "P installed: %s", w.cli(p, "fw pull-status").c_str());
  const uint32_t td = g_sim_now;
  w.run(75 * 60000);
  CHECK(has(w.cli(l, "fw pull-status"), "PAUSED") && ss(w, l).escalations == 0, "L paused an hour after P, no path pull (%u): %s",
        ss(w, l).escalations, w.cli(l, "fw pull-status").c_str());
  CHECK(has(w.cli(m, "fw pull-status"), "PAUSED") && ss(w, m).gap_reqs == 0, "M (> 20 %% missing) paused without a gap request (%u): %s",
        ss(w, m).gap_reqs, w.cli(m, "fw pull-status").c_str());
  CHECK(has(w.cli(l, "fw slot"), "partial fw-301") && has(w.cli(l, "fw slot"), "% of its chunks"), "L 'fw slot': %s", w.cli(l, "fw slot").c_str());
  const uint32_t lt = txOf(w, l); w.run(2 * 3600000);
  CHECK(txOf(w, l) - lt == 0, "L stayed silent for 2 h (%u packets)", txOf(w, l) - lt);
  (void)td;
  // the operator finishes L (its link is two-way now)
  w.link(s, l, 7, 0.12);
  std::string rs = w.cli(l, "fw pull-resume");
  CHECK(has(rs, "finishing the fill"), "pull-resume on a paused fill: %s", rs.c_str());
  w.run(3 * 3600000, [&]() { return slotFw(w, l) == 301; });
  CHECK(slotFw(w, l) == 301 && w[l].app_fw == 0, "L finished by 'fw pull-resume', not installed: %s", w.cli(l, "fw pull-status").c_str());
  // a newer image's stream replaces M's paused partial (owner)
  w.cli(p2, "fw pull " + idHex(w, z) + " duty 1");
  w.run(4 * 3600000, [&]() { return w[p2].app_fw == 302; });
  w.run(30 * 60000);
  auto* xm = w[m].xfer.get();
  CHECK(xm->_man.fw_version == 302 || slotFw(w, m) == 302, "M's partial fw-301 was replaced by the newer fw-302 stream: %s", w.cli(m, "fw pull-status").c_str());
}

// H6-H8 (rc19, owner 2026-10-10): the bench tests D-F as the hardware runs them - the H3 layout: S = board 1 (server, slot filled by
// Bluetooth), R = stock relay, P = board 2 (puller, off site), L1 = board 3, L2 = twin (next to S, they collect the stream). The timeline
// (every 10 min) is what the hardware log is compared with.
// SIM_BENCH_SHORT=1 (owner 2026-10-10 evening): the SHORT form of D-F - board 3 (direct to S) pulls, the twin listens, board 2 sits one
// relay away with its older image installed (it may hear part of a stream: shown, not checked).
struct Bench { World w; int s, r, p, l1, l2; bool shrt; std::vector<int> lis; };
static void benchBuild(Bench& b, bool emptyL) {
  World& w = b.w; W = &w; g_sim_now = 0; fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  b.shrt = getenv("SIM_BENCH_SHORT") != nullptr;
  b.s = w.add("S").idx; b.r = w.add("R", false).idx;
  const int far = w.add(b.shrt ? "B2" : "P").idx, near = w.add(b.shrt ? "P" : "L1").idx; b.l2 = w.add("L2").idx;
  b.p = b.shrt ? near : far; b.l1 = b.shrt ? far : near;
  w.link(b.s, b.r, 8, 0.1); w.link(b.r, far, 6, 0.25); w.link(b.s, near, 9, 0.1); w.link(b.s, b.l2, 9, 0.1); w.link(near, b.l2, 9, 0.1);
  w.seedImage(b.s, 301, 64000, 9, true); w.seedImage(b.p, 300, 60000, 1, true);
  if (b.shrt) w.seedImage(b.l1, 300, 60000, 1, true);
  for (int i : {b.l1, b.l2}) if (!emptyL && !(b.shrt && i == b.l1)) w.seedImage(i, 300, 60000, 1, true);
  b.lis = b.shrt ? std::vector<int>{b.l2} : std::vector<int>{b.l1, b.l2};
  circle(w, {b.s, b.p, b.l1, b.l2}); for (int i : {b.s, b.p, b.l1, b.l2}) w.cli(i, "fw duty 1");
  w.run(150000);
}
static void benchLine(Bench& b, uint32_t t0) {
  World& w = b.w;
  printf("    +%3u min P %s\n", (g_sim_now - t0) / 60000, w.cli(b.p, "fw pull-status").substr(0, 110).c_str());
  for (int i : {b.l1, b.l2}) printf("             %s %s\n", w[i].name.c_str(), w.cli(i, "fw pull-status").substr(0, 120).c_str());
  printf("             S %s\n", w.cli(b.s, "fw pull-status").substr(0, 120).c_str());
}
static void benchRun(Bench& b, uint32_t t0, uint32_t ms, std::function<bool()> stop = nullptr) {
  const bool tr = getenv("SIM_TRACE") != nullptr;
  for (uint32_t k = 0; k < ms; k += 600000) { b.w.run(600000); if (tr) benchLine(b, t0); if (stop && stop()) break; }
}
static bool lisDone(Bench& b, uint32_t fw) { for (int i : b.lis) if (slotFw(b.w, i) != fw) return false; return true; }
template <class T> static uint32_t lisSum(Bench& b, T fw_ota::FwOtaTransfer::SwarmStats::*f) { uint32_t t = 0; for (int i : b.lis) t += ss(b.w, i).*f; return t; }
static uint32_t lisTx(Bench& b) { uint32_t t = 0; for (int i : b.lis) t += txOf(b.w, i); return t; }

// H6 = bench test D (C42 fleet cancel): P 'fw pull <S> duty 1 fleet', the listeners fill (mode 2: they replace their older slot image);
// at ~25 % P 'fw pull-abort'. Expected: the abort reply names the fleet; within 10 min both listeners PAUSED ("cancelled by its
// requester"), 'fw slot' partial; nothing sent by them for an hour; S stays a server; 'fw pull-resume' on L2 finishes it from S (no install);
// L1 stays paused.
static void h_benchD() {
  printf("H6 bench test D: fleet pull aborted - the listeners pause; 'fw pull-resume' finishes one\n");
  Bench b; benchBuild(b, false); World& w = b.w; const uint32_t t0 = g_sim_now;
  w.cli(b.p, "fw pull " + idHex(w, b.s) + " duty 1 fleet");
  benchRun(b, t0, 4 * 3600000, [&]() { return pctOf(w, b.p) >= 25; });
  std::string ab = w.cli(b.p, "fw pull-abort");
  printf("  abort at P %u %%, L1 %u %%, L2 %u %%: %s\n", pctOf(w, b.p), pctOf(w, b.l1), pctOf(w, b.l2), ab.c_str());
  CHECK(has(ab, "the fleet stops too (the cancel is repeated for 30 min)"), "P's abort names the fleet: %s", ab.c_str());
  benchRun(b, t0, 10 * 60000);
  for (int i : b.lis) CHECK(has(w.cli(i, "fw pull-status"), "PAUSED") && has(w.cli(i, "fw pull-status"), "cancelled by its requester")
                                   && has(w.cli(i, "fw slot"), "partial fw-301"), "%s paused within 10 min: %s | %s", w[i].name.c_str(),
                                   w.cli(i, "fw pull-status").c_str(), w.cli(i, "fw slot").c_str());
  const uint32_t lt = lisTx(b), sr = ss(w, b.s).repairs;
  benchRun(b, t0, 60 * 60000);
  CHECK(lisTx(b) - lt <= 2 && ss(w, b.s).repairs == sr, "the paused listeners sent nothing for an hour (%u packets, S repairs +%u)", lisTx(b) - lt, ss(w, b.s).repairs - sr);
  CHECK(has(w.cli(b.s, "fw serve"), "fw-301"), "S still serves: %s", w.cli(b.s, "fw serve").c_str());
  std::string rs = w.cli(b.l2, "fw pull-resume");
  CHECK(has(rs, "OK - finishing the fill at"), "L2 'fw pull-resume': %s", rs.c_str());
  benchRun(b, t0, 4 * 3600000, [&]() { return slotFw(w, b.l2) == 301; });
  CHECK(slotFw(w, b.l2) == 301 && w[b.l2].app_fw == 300, "L2 finished from S, not installed: %s | %s", w.cli(b.l2, "fw pull-status").c_str(), w.cli(b.l2, "fw slot").c_str());
  if (!b.shrt) CHECK(has(w.cli(b.l1, "fw pull-status"), "PAUSED"), "L1 still paused: %s", w.cli(b.l1, "fw pull-status").c_str());
}

// H7 = bench test E (C42 passive fill, the puller "disappears"): the listeners' slots are EMPTY ('fw erase'; a plain pull fills only empty
// slots), P 'fw pull <S> duty 1' (plain), at ~25 % P 'fw pull-pause' - it frees its session at S and sends nothing, so for the listeners
// P is gone. Expected: more than 20 % missing -> both PAUSED ("passive fill paused") when the stream stops, no gap request, no path pull;
// 'fw pull-resume' on P an hour later: its stream continues the paused fills, P installs, the listeners finish (slot only).
static void h_benchE() {
  printf("H7 bench test E: plain pull, the puller pauses at 25 %% - the listeners pause; its resumed stream finishes them\n");
  Bench b; benchBuild(b, true); World& w = b.w; const uint32_t t0 = g_sim_now;
  w.cli(b.p, "fw pull " + idHex(w, b.s) + " duty 1");
  benchRun(b, t0, 4 * 3600000, [&]() { return pctOf(w, b.p) >= 25; });
  std::string pz = w.cli(b.p, "fw pull-pause");
  printf("  pause at P %u %%, L1 %u %%, L2 %u %%: %s\n", pctOf(w, b.p), pctOf(w, b.l1), pctOf(w, b.l2), pz.c_str());
  CHECK(has(pz, "OK - paused at"), "P paused: %s", pz.c_str());
  const uint32_t g0 = lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::gap_reqs);
  benchRun(b, t0, 20 * 60000);
  for (int i : b.lis) CHECK(has(w.cli(i, "fw pull-status"), "PAUSED") && has(w.cli(i, "fw pull-status"), "passive fill paused"),
                                   "%s paused within 20 min: %s", w[i].name.c_str(), w.cli(i, "fw pull-status").c_str());
  const uint32_t lt = lisTx(b);
  benchRun(b, t0, 60 * 60000);
  CHECK(lisTx(b) - lt == 0 && lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::gap_reqs) == g0 && lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::escalations) == 0,
        "silent for an hour: %u packets, gap requests +%u, path pulls %u", lisTx(b) - lt, lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::gap_reqs) - g0,
        lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::escalations));
  std::string rs = w.cli(b.p, "fw pull-resume");
  CHECK(has(rs, "OK - resumed at"), "P 'fw pull-resume': %s", rs.c_str());
  benchRun(b, t0, 6 * 3600000, [&]() { return w[b.p].app_fw == 301 && lisDone(b, 301); });
  CHECK(w[b.p].app_fw == 301, "P finished and installed: %s", w.cli(b.p, "fw pull-status").c_str());
  for (int i : b.lis) CHECK(slotFw(w, i) == 301 && w[i].app_fw == 0, "%s finished from P's resumed stream, not installed: %s | %s",
                                   w[i].name.c_str(), w.cli(i, "fw pull-status").c_str(), w.cli(i, "fw slot").c_str());
}

// H8 = bench test F (C42 passive fill near the end): as E, but P pauses at ~90 %: less than 20 % missing, so the listeners ask their
// neighbours for the rest (zero-hop: S holds the image) - no path pull - and finish while P stays paused. Not installed.
static void h_benchF() {
  printf("H8 bench test F: plain pull, the puller pauses at 90 %% - the listeners finish from S by zero-hop repairs\n");
  Bench b; benchBuild(b, true); World& w = b.w; const uint32_t t0 = g_sim_now;
  w.cli(b.p, "fw pull " + idHex(w, b.s) + " duty 1");
  w.run(6 * 3600000, [&]() { return pctOf(w, b.p) >= 90; });       // (stopped to the minute: the 10-min timeline overshoots 90 %)
  if (getenv("SIM_TRACE")) benchLine(b, t0);
  std::string pz = w.cli(b.p, "fw pull-pause");
  printf("  pause at P %u %%, L1 %u %%, L2 %u %%: %s\n", pctOf(w, b.p), pctOf(w, b.l1), pctOf(w, b.l2), pz.c_str());
  CHECK(has(pz, "OK - paused at"), "P paused: %s", pz.c_str());
  benchRun(b, t0, 90 * 60000, [&]() { return lisDone(b, 301); });
  for (int i : b.lis) CHECK(slotFw(w, i) == 301 && w[i].app_fw == 0, "%s finished from S while P is paused, not installed: %s | %s",
                                   w[i].name.c_str(), w.cli(i, "fw pull-status").c_str(), w.cli(i, "fw slot").c_str());
  CHECK(lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::escalations) == 0, "no path pulls (%u)", lisSum(b, &fw_ota::FwOtaTransfer::SwarmStats::escalations));
  CHECK(has(w.cli(b.p, "fw pull-status"), "PAUSED"), "P still paused: %s", w.cli(b.p, "fw pull-status").c_str());
}

// H9 (rc20, C41): the scan answer storm (RC17-FLEET-SIM-REPORT): a hub H with five holders of an OLDER image around it and LoRa-OTA relays,
// the newest image (S) three hops away. Every holder floods an answer to 'fw peers' and every node relays every flood: rc17 saw 4-18
// collisions per scan at the hub and never listed S. Prints how often S is listed (SIM_SEED=n, SIM_TRACE=1: the lists).
static void h_storm() {
  printf("H9 (C41) scan answer storm: 5 holders of an older image, the newest 3 hops away\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  if (getenv("SIM_SEED")) { g_seed = (uint32_t)atoi(getenv("SIM_SEED")); w.chance = Rng(12345 + 104729ULL * g_seed); }
  int h = w.add("H").idx, r1 = w.add("R1").idx, r2 = w.add("R2").idx, s = w.add("S").idx;
  int o1 = w.add("O1").idx, o2 = w.add("O2").idx, o3 = w.add("O3").idx, o4 = w.add("O4").idx, o5 = w.add("O5").idx;
  w.link(h, r1, 7, 0.1); w.link(r1, r2, 6, 0.1); w.link(r2, s, 7, 0.1);
  w.link(o1, h, 8, 0.1); w.link(o2, h, 7, 0.1); w.link(o1, o2, 6, 0.1); w.link(o3, r1, 7, 0.1); w.link(o3, h, 3, 0.2);
  w.link(o4, r2, 7, 0.1); w.link(o5, r1, 5, 0.1); w.link(o5, r2, 5, 0.1);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(h, 299, 60000, 1, true); w.seedImage(r1, 299, 60000, 1, true); w.seedImage(r2, 299, 60000, 1, true);
  if (!getenv("SIM_NO_OLD")) for (int o : {o1, o2, o3, o4, o5}) w.seedImage(o, 300, 60000, 1, true);
  circle(w, {h, r1, r2, s, o1, o2, o3, o4, o5}); for (auto& x : w.nodes) w.cli(x->idx, "fw duty 1");
  w.run(150000);
  uint32_t listed = 0, c0 = w[h].st.collisions, f0 = floodsAll(w); const int SCANS = 5;
  for (int k = 0; k < SCANS; k++) {
    w.cli(h, "fw peers"); w.run(120000);
    std::string pe = w.cli(h, "fw list");
    if (has(pe, "fw-301")) listed++;
    if (getenv("SIM_TRACE")) printf("    scan %d: %s\n", k + 1, pe.c_str());
    w.run(20 * 60000);
  }
  const uint32_t col = w[h].st.collisions - c0, fl = floodsAll(w) - f0;
  printf("  S listed in %u of %d scans; hub collisions %u (%.1f per scan); floods %u; answers sent S %u, older holders %u (suppressed %u); "
         "collisions R1 %u R2 %u\n", listed, SCANS, col, col / (double)SCANS, fl, ss(w, s).answers_sent,
         ss(w, o1).answers_sent + ss(w, o2).answers_sent + ss(w, o3).answers_sent + ss(w, o4).answers_sent + ss(w, o5).answers_sent + ss(w, r1).answers_sent + ss(w, r2).answers_sent,
         ss(w, o1).answers_suppressed + ss(w, o2).answers_suppressed + ss(w, o3).answers_suppressed + ss(w, o4).answers_suppressed + ss(w, o5).answers_suppressed,
         w[r1].st.collisions, w[r2].st.collisions);
  // (rc20) a MEASUREMENT, no check: S is missed in ~half of the scans with or without the older holders (SIM_NO_OLD=1) - the far answer is
  // lost in the relays' own collisions of the same floods (R2 hears four relays repeat each one), not in a storm of answers; spreading the
  // answers (4-12 s) or answering direct did not help (docs/POSTPONED-IMPROVEMENTS C41)
  (void)col;
}

// H3 (C44): test B on the bench - S (board 1) serves P (board 2) one stock relay away; L1, L2 (board 3, twin) sit next to S and collect the
// fleet stream. rc18: the listeners asked S for chunks AHEAD of the stream (739 repairs for 1766 chunks); rc19: only behind it.
static void h_behind() {
  printf("H3 (C44) test B layout: gap requests only behind the stream\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r = w.add("R", false).idx, p = w.add("P").idx, l1 = w.add("L1").idx, l2 = w.add("L2").idx;
  w.link(s, r, 8, 0.1); w.link(r, p, 6, 0.25); w.link(s, l1, 9, 0.1); w.link(s, l2, 9, 0.1); w.link(l1, l2, 9, 0.1);
  w.seedImage(s, 301, 64000, 9, true); for (int i : {p, l1, l2}) w.seedImage(i, 300, 60000, 1, true);
  circle(w, {s, p, l1, l2}); for (int i : {s, p, l1, l2}) w.cli(i, "fw duty 1");
  w.run(150000);
  const uint32_t t0 = g_sim_now;
  w.cli(p, "fw pull " + idHex(w, s) + " duty 1 fleet");
  uint32_t ahead = 0;
  if (getenv("SIM_DBG")) for (int k = 0; k < 40; k++) { w.run(3 * 60000);
    auto* xp = w[p].xfer.get(); printf("  +%3u min P have %u | S reps %u\n", (g_sim_now - t0) / 60000, xp->_cm.have, ss(w, s).repairs);
    for (int i : {l1, l2}) { auto* x = w[i].xfer.get(); printf("    %s have %u top %u lis %d gap %d fann %.1f min ago fhave %u gaps %u reps %u | %s\n", w[i].name.c_str(), x->_cm.have, x->_strm_top, x->_listening,
      x->_gap_phase, x->_fann_heard_ms ? (millis() - x->_fann_heard_ms) / 60000.0 : -1.0, x->_fann_have, x->stats.gap_reqs, x->stats.repairs, w.cli(i, "fw pull-status").substr(0, 90).c_str()); } }
  // the puller stalls for 8 min mid-pull (test B: board 2's re-offers) - the requester is alive, its stream pauses: the listeners must not ask
  // the server for what lies ahead of it
  w.run(4 * 3600000, [&]() { return pctOf(w, p) >= 50; });
  uint32_t ro = 0; for (auto& n : w.nodes) if (n->xfer) ro += n->xfer->stats.repairs;
  w.unlink(r, p); w.run(8 * 60000); w.link(r, p, 6, 0.25);
  uint32_t ro1 = 0; for (auto& n : w.nodes) if (n->xfer) ro1 += n->xfer->stats.repairs;
  printf("  repairs while the puller stalled 8 min: %u (gaps behind the stream)\n", ro1 - ro);
  w.run(12 * 3600000, [&]() { auto* xp = w[p].xfer.get();
    for (int i : {l1, l2}) { auto* x = w[i].xfer.get(); if (x->_man.n_chunks && xp->_man.n_chunks && x->_cm.have > xp->_cm.have + 40) ahead++; }
    return w[p].app_fw == 301 && slotFw(w, l1) == 301 && slotFw(w, l2) == 301; });
  uint32_t reps = 0, gaps = 0; for (auto& n : w.nodes) if (n->xfer) { reps += n->xfer->stats.repairs; gaps += n->xfer->stats.gap_reqs; }
  CHECK(w[p].app_fw == 301 && slotFw(w, l1) == 301 && slotFw(w, l2) == 301, "all three have fw-301 (P app %u, L1 %u, L2 %u)", w[p].app_fw, slotFw(w, l1), slotFw(w, l2));
  CHECK(reps <= 160, "repairs %u for 400 chunks (<= 160; rc18 465: a holder's announce ended the listeners' stream)", reps);
  CHECK(ahead == 0, "no listener got ahead of the puller by > 40 chunks (%u samples)", ahead);
  printf("  done %.0f min; repairs %u, gap requests %u, S tx %u (%.2f/chunk)\n", (g_sim_now - t0) / 60000.0, reps, gaps, w[s].st.tx, w[s].st.tx / 400.0);
}

// H11 (rc20, C48): a second holder T looks faster than S (a direct neighbour of P with a strong signal) but P's requests reach it badly (a
// link good one way only: the rc19 rollout's twin). rc19 moved to T by the rate model and stayed on the slow holder (or moved back and forth).
// rc20: 15 min after the move the measured rate decides - back to S, T barred for 2 h; the pull finishes with at most 2 moves.
static void h_move() {
  printf("H11 (C48) a holder that only looks faster: the move is taken back by the measured rate\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r = w.add("R", false).idx, p = w.add("P").idx, t = w.add("T").idx;
  w.link(s, r, 8, 0.05); w.link(r, p, 8, 0.05); w.link(s, t, 9, 0.05);
  const double tl = getenv("SIM_T_LOSS") ? atof(getenv("SIM_T_LOSS")) : 0.5;
  w.link(t, p, 12, 0.05, false); w.link(p, t, 2, tl, false);           // P hears T well; T hears P's requests badly
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(t, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true);
  circle(w, {s, p, t}); for (int i : {s, p, t}) w.cli(i, "fw duty 1");
  w.run(150000);
  const uint32_t t0 = g_sim_now;
  w.cli(p, "fw pull " + idHex(w, s) + " duty 1");
  std::string rt; uint8_t why = 0; bool back = false;
  uint32_t swm = 0;
  w.run(12 * 3600000, [&]() { auto* x = w[p].xfer.get();
    if (x->_sw_why && x->_sw_ms != swm) { swm = x->_sw_ms; why = x->_sw_why; rt = w.cli(p, "fw route"); back |= why == 3;
      if (getenv("SIM_TRACE")) printf("    +%u min change %u to %02X (have %u): %s\n", (g_sim_now - t0) / 60000, why, x->_sw_id[0], x->_cm.have, x->_last_err); }
    return w[p].app_fw == 301; });
  const uint32_t mv = ss(w, p).moves;
  printf("  last holder change: %s\n", rt.c_str());
  printf("  done %.0f min, moves %u (last change reason %u)\n", (g_sim_now - t0) / 60000.0, mv, why);
  CHECK(w[p].app_fw == 301, "P finished: %s", w.cli(p, "fw pull-status").c_str());
  CHECK(back && why == 3 && has(rt, "(back)"), "moved back to S by the measured rate and stayed; 'fw route': %s", rt.c_str());
  CHECK(mv <= 2, "at most one move and its way back (%u moves)", mv);
  CHECK(g_sim_now - t0 < 95 * 60000u, "within 95 min (%.0f min; staying on T: ~113)", (g_sim_now - t0) / 60000.0);
}

// H13 (rc20, C49): 'fw pull-abort' of a fleet pull whose two ABORT copies are lost (P's link to the relay is down for a minute): the repeat
// after 2 min frees the server at once - rc19 held the session's share until its hold ran out (up to 15 min).
static void h_abtrep() {
  printf("H13 (C49) the session ABORT is repeated: a server that missed it is freed within ~3 min\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, r = w.add("R", false).idx, p = w.add("P").idx;
  w.link(s, r, 8, 0.05); w.link(r, p, 8, 0.05);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true);
  circle(w, {s, p}); for (int i : {s, p}) w.cli(i, "fw duty 1");
  w.run(150000);
  w.cli(p, "fw pull " + idHex(w, s) + " duty 1 fleet");
  w.run(3 * 3600000, [&]() { return pctOf(w, p) >= 20; });
  w.unlink(p, r, false); std::string ab = w.cli(p, "fw pull-abort"); w.run(60000); w.link(p, r, 8, 0.05, false);
  bool held = false; for (auto& d : w[s].xfer->_dsess) if (d.sess) held = true;
  w.run(150000);
  bool freed = true; for (auto& d : w[s].xfer->_dsess) if (d.sess) freed = false;
  printf("  abort: %s | S after 1 min: session held %d; after 3.5 min: %s\n", ab.c_str(), held, w.cli(s, "fw pull-status").substr(0, 90).c_str());
  CHECK(held, "the ABORTs were lost (S still held the session after 1 min)");
  CHECK(freed, "the repeated ABORT freed S's session within 3.5 min");
}

// H4 (C45, C46, C47) texts
static void h_texts() {
  printf("H4 (C45-C47) texts: autoapply off, a weak battery before Bluetooth, the server status of a resumed pull\n");
  fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0;
  World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, p = w.add("P").idx; w.link(s, p, 9);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(p, 300, 60000, 1, true);
  circle(w, {s, p}); for (int i : {s, p}) w.cli(i, "fw duty 1");
  w.run(150000);
  w.cli(p, "fw autoapply off");
  std::string r = w.cli(p, "fw pull " + idHex(w, s) + " duty 1 fleet");
  CHECK(has(r, "auto-apply is off") && !has(r, "applies itself"), "C45 fleet pull reply with autoapply off: %s", r.c_str());
  w.cli(p, "fw pull-abort"); w.cli(p, "fw autoapply on");
  r = w.cli(p, "fw pull " + idHex(w, s) + " duty 1");
  CHECK(has(r, "applies itself"), "C45 with autoapply on: %s", r.c_str());
  // C47: P's pull resumes under a new session - the server's status keeps its position
  auto* xp = w[p].xfer.get();
  w.run(2 * 3600000, [&]() { return xp->_man.n_chunks && xp->_cm.have * 2 >= xp->_man.n_chunks; });
  enter(w[p]); xp->enterWait("test: stall");
  w.run(120000, [&]() { return xp->_phase == fw_ota::PH_DOWNLOADING; });
  std::string st = w.cli(s, "fw pull-status");
  CHECK(!has(st, "starting") && has(st, "/400 ("), "C47 the server shows the resumed pull's position: %s", st.c_str());
  // (rc20, C55) a repeated pause / resume (its first reply lost over LoRa) says what is, not an error
  r = w.cli(p, "fw pull-pause"); std::string r2 = w.cli(p, "fw pull-pause");
  CHECK(has(r2, "OK - already paused at") && has(r2, "until 'fw pull-resume'"), "C55 a second pause: %s", r2.c_str());
  r2 = w.cli(p, "fw pull-pause 3h"); CHECK(has(r2, "OK - already paused at") && has(r2, "in 3 h"), "C55 a new pause timer: %s", r2.c_str());
  w.cli(p, "fw pull-resume"); r2 = w.cli(p, "fw pull-resume");
  CHECK(has(r2, "OK - not paused: the pull runs at"), "C55 a second resume: %s", r2.c_str());
  w.run(3 * 3600000, [&]() { return w[p].app_fw == 301; });
  // C46: 'fw pull-ble' on a weak battery without USB warns (and still goes ahead); with USB power: the plain reply
  auto* xs = w[s].xfer.get(); enter(w[s]); xs->_bl_probed = true; xs->_own_bl_rev = 8;
  xs->noteBattery(3310); w[s].power.USBREGSTATUS = 0;
  r = w.cli(s, "fw pull-ble"); xs->_reset_at_ms = 0;
  CHECK(has(r, "OK - Bluetooth upload into the slot") && has(r, "3.31 V, no USB") && r.size() < 160, "C46 warning: %s", r.c_str());
  w[s].power.USBREGSTATUS = 1; r = w.cli(s, "fw pull-ble"); xs->_reset_at_ms = 0;
  CHECK(has(r, "OK - Bluetooth upload into the slot") && !has(r, "no USB"), "C46 on USB power: %s", r.c_str());
  xs->noteBattery(3900); w[s].power.USBREGSTATUS = 0; r = w.cli(s, "fw pull-ble"); xs->_reset_at_ms = 0;
  CHECK(!has(r, "no USB"), "C46 a full battery: %s", r.c_str());
}

// H15 (rc21, C57/C58): a node without an RTC takes the time of a circle neighbour that has one; a clock that runs ahead is held, never
// stepped back; a peer far off is refused once synced; no relaying of time; an apply keeps an RTC node's time (rc20 wrote it 30 s ahead)
static uint32_t tAns(World& w) { uint32_t n = 0; for (auto& x : w.nodes) if (x->xfer) n += x->xfer->_ta_sent; return n; }
static void h_time() {
  printf("H15 (rc21, C57/C58) time from a swarm member with an RTC; an apply keeps an RTC node's time\n");
  World w; W = &w; g_sim_now = 0;
  int a = w.add("A").idx, b = w.add("B").idx, r = w.add("R").idx, x = w.add("X").idx;   // A, B: RTC; R, X: none; X hears only R
  w.link(a, b, 10); w.link(a, r, 8); w.link(b, r, 6); w.link(r, x, 8);
  for (int i : {r, x}) { w[i].rtc.chip = false; w.boot(w[i], 0); }                          // R and X power up: clock at the build floor
  circle(w, {a, b, r, x});
  CHECK(w[r].rtc.off() < -3600 * 24, "R starts at the build floor (off %d s)", w[r].rtc.off());
  w.run(6 * 60000, [&]() { int o = w[r].rtc.off(); return o >= -2 && o <= 2; });
  std::string t = w.cli(r, "fw time");
  CHECK(abs(w[r].rtc.off()) <= 2 && has(t, "no RTC: from ") && has(t, "stepped"), "R took the time from A or B (off %d s): %s", w[r].rtc.off(), t.c_str());
  w.run(10000);
  { const uint32_t a0 = tAns(w);                                                          // ten more requests: about one answer each
    for (int k = 0; k < 10; k++) { w.cli(r, "fw time ask"); w.run(60000); }
    const uint32_t n = tAns(w) - a0;
    CHECK(n >= 10 && n <= 13, "one answer per request (A and B hear each other: the later one drops its own): %u answers to 10 requests, dropped A %u B %u",
          n, w[a].xfer->_ta_supp, w[b].xfer->_ta_supp); }
  CHECK(w[a].xfer->_tq_tries == 0 && w[b].xfer->_tq_tries == 0, "nodes with an RTC never ask");
  CHECK(w[x].rtc.off() < -3600 * 24 && has(w.cli(x, "fw time"), "no swarm time yet"), "X (no RTC neighbour) gets no time - R does not relay it: off %d s", w[x].rtc.off());
  CHECK(has(w.cli(a, "fw time"), "RTC chip: answers"), "A shows its RTC: %s", w.cli(a, "fw time").c_str());
  // R's software clock runs ahead (5 min, like a field relay on 2026-10-14): the next answer HOLDS it, it never steps back
  w[r].rtc.setRaw(w[r].rtc.raw() + 300); w.run(2000);
  CHECK(w[r].rtc.off() >= 298, "R is 5 min ahead now (%d s)", w[r].rtc.off());
  w.cli(r, "fw time ask"); w.run(30000);
  t = w.cli(r, "fw time");
  CHECK(has(t, "held"), "R holds: %s", t.c_str());
  w.run(6 * 60000);
  CHECK(abs(w[r].rtc.off()) <= 2 && !w[r].rtc.went_back, "R back in step after the hold (off %d s), never stepped back (%d)", w[r].rtc.off(), (int)w[r].rtc.went_back);
  // a peer far off once synced (A's chip 2 h behind, B gone): refused
  w.unlink(b, r); w[a].rtc.chip_off = -7200; w.boot(w[a], POWER_RESETREAS_SREQ_Msk); w.run(60000);
  w.cli(r, "fw time ask"); w.run(30000);
  t = w.cli(r, "fw time");
  CHECK(has(t, "refused") && abs(w[r].rtc.off()) <= 3, "R refuses a time 2 h off (off %d s): %s", w[r].rtc.off(), t.c_str());
  // C58: an apply on a node WITH an RTC (a short restart: 10 s) keeps its time and its chip
  w[a].rtc.chip_off = 0; w.boot(w[a], POWER_RESETREAS_SREQ_Msk); w.run(60000);
  enter(w[a]); w[a].xfer->saveClock(); w[a].down_until = g_sim_now + 10000; w.boot(w[a], POWER_RESETREAS_SREQ_Msk); w.run(60000);
  CHECK(abs(w[a].rtc.off()) <= 1 && w[a].rtc.chip_off == 0, "C58: an apply leaves an RTC node's time (off %d s, chip %+d s)", w[a].rtc.off(), w[a].rtc.chip_off);
  // C58: an apply on R (no RTC, 60 s restart): never ahead after it, back in step from a neighbour soon
  w.link(b, r, 6); enter(w[r]); w[r].xfer->saveClock(); w[r].down_until = g_sim_now + 60000; w.boot(w[r], POWER_RESETREAS_SREQ_Msk);
  w.run(61000); w.run(2000);
  CHECK(w[r].rtc.off() <= 0, "C58: R is not ahead after an apply (off %d s)", w[r].rtc.off());
  w.run(3 * 60000, [&]() { return abs(w[r].rtc.off()) <= 2; });
  CHECK(abs(w[r].rtc.off()) <= 2, "R in step again after the apply (off %d s)", w[r].rtc.off());
  printf("  answers sent %u, dropped A %u B %u\n", tAns(w), w[a].xfer->_ta_supp, w[b].xfer->_ta_supp);
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "STUDY") return study(argc, argv);
  const std::string only = argc > 1 ? argv[1] : "";
  auto want = [&](const char* n) { return only.empty() || has("," + only + ",", (std::string(",") + n + ",").c_str()); };
  if (want("S0")) s0_basic();
  if (want("K")) k_duty();
  if (want("K5")) k_failover();
  if (want("K6")) k_relay();
  if (want("K7")) k_share();
  if (want("R2")) r_bottleneck();
  if (want("R3")) r_quiet();
  if (want("R4")) r_suppress();
  if (want("R5")) r_chain();
  if (want("R7")) r_reboot();
  if (want("R8")) r_wdt_drain();
  if (want("R9")) { r_shared_n(1); r_shared_n(2); r_shared_n(5); }
  if (want("R10")) r_late();
  if (want("R11")) r_lossy_joiners();
  if (want("R12")) r_leader_dies();
  if (want("R14")) r_finished_serves();
  if (want("C37")) f_cli();
  if (want("F1")) f_mode1(0.10);
  if (want("F2")) f_mode2(0.10, false);
  if (want("F2R")) f_mode2(0.10, true);
  if (want("F3")) f_safety();
  if (want("F4")) f_gaps();
  if (want("G1")) g_valley("G1", "T2", 1);
  if (want("G2")) g_valley("G2", "T3", 1);
  if (want("G3")) g_valley("G3", "T4", 7);
  if (want("H5")) g_valley("H5", "T4", 2);                         // (rc19) a neighbour's own path pull (mode 1) made a fleet fill passive
  if (want("H9")) h_storm();                                        // (rc20, C41) the scan answer storm
  if (want("H11")) h_move();                                        // (rc20, C48)
  if (want("H13")) h_abtrep();                                      // (rc20, C49)
  if (want("H15")) h_time();                                        // (rc21, C57/C58)
  if (want("H6")) h_benchD();
  if (want("H7")) h_benchE();
  if (want("H8")) h_benchF();
  if (want("G4")) g_fleet_reboot();
  if (want("G5")) g_rest();
  if (want("H1")) h_cancel();
  if (want("H2")) h_passive();
  if (want("H3")) h_behind();
  if (want("H4")) h_texts();
  if (only == "F0") f_baseline(0.10);                               // measurements on request (long)
  if (only == "F5") f_alone(0.10);
  if (only == "F5L30") f_alone(0.30);
  if (only == "FLOSS") { for (double l : {0.0, 0.2, 0.3}) f_mode2(l, false); }
  CHECK(sim::g_audit_fail == 0, "text audit: %d of %d status texts contradict the node's state (see AUDIT FAIL)", sim::g_audit_fail, sim::g_audit_texts);
  printf("  text audit: %d status texts read and compared with the node state\n", sim::g_audit_texts);
  printf("simulator: %d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
