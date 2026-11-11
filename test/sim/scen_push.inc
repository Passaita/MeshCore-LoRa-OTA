// (rc25, C95, owner 2026-11-04/11-07) `fw push`: the source starts the swarm fill of its slot image (fill only, nobody installs) and a
// 7-day CAMPAIGN that late joiners still find: the invite flood, the holders' zero-hop announces (the rc18 wave), the newcomer query at
// boot, a daily reminder for the whole circle, island pulls behind stock relays (farthest first), STUCK results, cancel. `fw pull fw-N
// fleet` runs the same campaign. Included by scenarios.cpp (its helpers). Design: docs/protocol/FW-PUSH-DESIGN-C95-20261104.md.

static uint32_t islandPulls(World& w) { uint32_t n = 0; for (auto& x : w.nodes) if (x->xfer) n += x->xfer->stats.island_pulls; return n; }
static uint32_t remindersAll(World& w) { uint32_t n = 0; for (auto& x : w.nodes) if (x->xfer) n += x->xfer->stats.camp_reminders; return n; }
static bool allSlot(World& w, const std::vector<int>& ids, uint32_t fw) { for (int i : ids) if (slotFw(w, i) != fw) return false; return true; }
static void pushTune() { fw_ota::g_tune = fw_ota::SwarmTune(); g_seed = 0; }
static void pushDump(World& w, int origin) {   // per member: island pulls, results sent, path back, what the origin knows of it
  auto* o = w[origin].xfer.get();
  for (auto& n : w.nodes) { auto* x = n->xfer.get(); if (!x || n->idx == origin) continue;
    int st = -1; for (uint8_t k = 0; k < o->_ncpeers; k++) if (memcmp(o->_cpeers[k].id, n->id.pub_key, 4) == 0) st = o->_cpeers[k].st;
    printf("    %-3s joins %u island %u results %u back %u hops %u | origin has %d | %s\n", n->name.c_str(), x->stats.camp_joins, x->stats.island_pulls,
           x->stats.camp_results, x->_camp.back_ok, x->_camp.hops, st, w.cli(n->idx, "fw push-status").c_str()); }
}
static void noInstall(World& w, const std::vector<int>& ids, uint32_t fw) {
  for (int i : ids) CHECK(w[i].app_fw != fw, "%s did not install fw-%u (app %u): a campaign only fills", w[i].name.c_str(), fw, w[i].app_fw);
}
// a LoRa-OTA node behind its own stock relay X, X linked to `to` (only if link): no LoRa-OTA node in its zero-hop reach
static int behindStock(World& w, const char* name, int to, bool link, uint32_t fw_inst) {
  int x = w.add(std::string("X") + name, false).idx, n = w.add(name).idx;
  w.link(x, n, 8, 0.05); if (link) w.link(x, to, 8, 0.05);
  if (fw_inst) w.seedImage(n, fw_inst, 60000, 1, true);
  return n;
}

// P1 the owner's chain: one `fw push` on S fills every LoRa-OTA node of its board; P and C3 are islands behind the stock R3 (one routed
// pull reaches both). Refusals: the Heltec next to S (other board) and NW (its slot holds a NEWER image) are not touched.
static void p_chain() {
  printf("P1 fw push on the owner's chain: every LoRa-OTA node of the board fills from one command, nobody installs\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  int hz = w.add("HZ", true, true, "HELTECV4").idx; w.link(hz, f.s, 8, 0.10);
  int nw = w.add("NW").idx; w.link(nw, f.r1, 8, 0.10); w.seedImage(nw, 302, 60000, 3, false);
  circle(w, {hz, nw}); w.run(150000);
  const std::vector<int> mem = {f.r1, f.r2, f.p, f.c1, f.c2, f.c3, f.b1};
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w);
  std::string r = w.cli(f.s, "fw push");
  CHECK(has(r, "OK") && has(r, "fw-301"), "'fw push' starts the push of fw-301: %s", r.c_str());
  w.run(24 * 3600000, [&]() { return allSlot(w, mem, 301); });
  const double took = (g_sim_now - t0) / 60000.0;
  for (int i : mem) CHECK(slotFw(w, i) == 301, "%s filled fw-301 (slot %u): %s", w[i].name.c_str(), slotFw(w, i), w.cli(i, "fw push-status").c_str());
  noInstall(w, mem, 301);
  CHECK(slotFw(w, nw) == 302, "NW keeps its newer slot image fw-302 (slot %u)", slotFw(w, nw));
  CHECK(slotFw(w, hz) == 0 && w[hz].xfer->_phase == 0, "the Heltec (other board) is not touched (slot %u, phase %u)",
        slotFw(w, hz), w[hz].xfer->_phase);
  CHECK(islandPulls(w) >= 1 && islandPulls(w) <= 2, "%u island pull(s) behind the stock relay (1-2: P and C3 share one stream)", islandPulls(w));
  w.run(3600000);                                        // the results reach the source
  std::string st = w.cli(f.s, "fw push-status");
  CHECK(has(st, "7 complete"), "S counts 7 complete: %s", st.c_str());
  for (int i : mem) CHECK(!w[i].st.wdt_resets, "%s: no watchdog reset", w[i].name.c_str());
  printf("  all 7 filled in %.0f min, channel air %.0f s, island pulls %u, invites %u; S: %s\n", took, (airAll(w) - a0) / 1000.0, islandPulls(w),
         w[f.s].xfer->stats.camp_invites, st.c_str());
  pushDump(w, f.s);
}

// P2 a tree: two LoRa-OTA branches (10 % and 20 % loss per link) and one behind a stock relay
static void p_tree() {
  printf("P2 fw push on a tree: three branches, 10 / 20 %% loss, one behind a stock relay\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx; std::vector<int> mem;
  int prev = s; for (int k = 1; k <= 3; k++) { int a = w.add("A" + std::to_string(k)).idx; w.link(prev, a, 8, 0.10); mem.push_back(a); prev = a; }
  prev = s; for (int k = 1; k <= 3; k++) { int b = w.add("B" + std::to_string(k)).idx; w.link(prev, b, 7, 0.20); mem.push_back(b); prev = b; }
  int x = w.add("X", false).idx; w.link(s, x, 8, 0.10); prev = x;
  for (int k = 1; k <= 2; k++) { int c = w.add("C" + std::to_string(k)).idx; w.link(prev, c, 8, 0.10); mem.push_back(c); prev = c; }
  w.seedImage(s, 301, 64000, 9, true); for (int i : mem) w.seedImage(i, 300, 60000, 1, true);
  std::vector<int> all = mem; all.push_back(s); circle(w, all); for (int i : all) w.cli(i, "fw duty 1");
  w.run(150000);
  const uint32_t t0 = g_sim_now; const uint64_t a0 = airAll(w);
  w.cli(s, "fw push");
  w.run(24 * 3600000, [&]() { return allSlot(w, mem, 301); });
  for (int i : mem) CHECK(slotFw(w, i) == 301, "%s filled fw-301 (slot %u): %s", w[i].name.c_str(), slotFw(w, i), w.cli(i, "fw push-status").c_str());
  noInstall(w, mem, 301);
  printf("  all 8 filled in %.0f min, channel air %.0f s, island pulls %u\n", (g_sim_now - t0) / 60000.0, (airAll(w) - a0) / 1000.0, islandPulls(w));
}

// P3 an island behind two stock relays: the FARTHEST island node pulls first, the others listen to its stream (one stream, not three)
static void p_island() {
  printf("P3 island behind two stock relays: the farthest node pulls, its neighbours listen\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, a = w.add("A").idx, x1 = w.add("X1", false).idx, x2 = w.add("X2", false).idx;
  int i1 = w.add("I1").idx, i2 = w.add("I2").idx, i3 = w.add("I3").idx;
  w.link(s, a, 8, 0.05); w.link(a, x1, 8, 0.05); w.link(x1, x2, 8, 0.05); w.link(x2, i1, 8, 0.05); w.link(i1, i2, 8, 0.05); w.link(i2, i3, 8, 0.05);
  w.seedImage(s, 301, 64000, 9, true); for (int i : {a, i1, i2, i3}) w.seedImage(i, 300, 60000, 1, true);
  circle(w, {s, a, i1, i2, i3}); for (int i : {s, a, i1, i2, i3}) w.cli(i, "fw duty 1");
  w.run(150000);
  w.cli(s, "fw push");
  if (getenv("SIM_DBG")) for (int k = 0; k < 30; k++) { w.run(60000); printf("  +%2d min\n", k + 1);
    for (int i : {i1, i2, i3}) { auto* x = w[i].xfer.get(); printf("    %s ph%u isl%u heard %u at %u | %s\n", w[i].name.c_str(), x->_phase, x->stats.island_pulls,
      x->_camp_heard_ms ? (g_sim_now - w[i].boot_ms - x->_camp_heard_ms) / 1000 : 9999, x->_camp_island_ms ? (x->_camp_island_ms - (g_sim_now - w[i].boot_ms)) / 1000 : 0,
      w.cli(i, "fw pull-status").substr(0, 110).c_str()); } }
  int first = -1;                                        // the first island node that started its own pull
  w.run(24 * 3600000, [&]() { if (first < 0) for (int i : {i1, i2, i3}) if (w[i].xfer->stats.island_pulls) { first = i; break; }
                              return allSlot(w, {a, i1, i2, i3}, 301); });
  for (int i : {a, i1, i2, i3}) CHECK(slotFw(w, i) == 301, "%s filled fw-301 (slot %u): %s", w[i].name.c_str(), slotFw(w, i), w.cli(i, "fw push-status").c_str());
  CHECK(first == i3 && islandPulls(w) <= 2, "the farthest node I3 pulls first (first %s), at most 2 island pulls - never one per node (I1 %u, I2 %u, I3 %u)",
        first < 0 ? "-" : w[first].name.c_str(), w[i1].xfer->stats.island_pulls, w[i2].xfer->stats.island_pulls, w[i3].xfer->stats.island_pulls);
  noInstall(w, {a, i1, i2, i3}, 301);
}

// P4 disturbances: the island node P reboots before its island timer, R2 loses power mid-fill, C1 is CRITICAL for 12 h, B1 misses the
// invite (its only link is down for 2 h): every node still fills, nobody installs
static void p_disturb() {
  printf("P4 fw push with a reboot, a power cut, a CRITICAL battery and a missed invite\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  const std::vector<int> mem = {f.r1, f.r2, f.p, f.c1, f.c2, f.c3, f.b1};
  w.cli(f.c1, "fw minbatt 3200"); w[f.c1].batt_mv = 2950; w[f.c1].xfer->noteBattery(2950);   // CRITICAL
  w.unlink(f.b1, f.c2);
  const uint32_t t0 = g_sim_now; auto until = [&](uint32_t ms) { if (g_sim_now < t0 + ms) w.run(t0 + ms - g_sim_now); };
  w.cli(f.s, "fw push");
  w.run(60000); const bool pj = w[f.p].xfer->_camp.set; powerCut(w, f.p, 30000); w.log("TEST: P power cut (before its island timer)");
  w.run(6 * 60000);                                      // (a member keeps its campaign across the cut: no newcomer query flood)
  CHECK(!pj || (w[f.p].xfer->_camp.set && w[f.p].xfer->stats.camp_queries == 0), "P kept the campaign across the power cut (joined %d, kept %u, queries %u)",
        pj, w[f.p].xfer->_camp.set, w[f.p].xfer->stats.camp_queries);
  auto pct = [&](int i) { auto* x = w[i].xfer.get(); return x && x->_man.n_chunks ? 100u * x->_cm.have / x->_man.n_chunks : 0u; };
  w.run(6 * 3600000, [&]() { return pct(f.r2) >= 40 || slotFw(w, f.r2) == 301; }); powerCut(w, f.r2, 40000); w.log("TEST: R2 power cut");
  until(2 * 3600000); w.link(f.b1, f.c2, 9, 0.10); w.log("TEST: B1 link back");
  until(12 * 3600000); w[f.c1].batt_mv = 3300; w[f.c1].xfer->noteBattery(3300); w.log("TEST: C1 battery back");
  w.run(24 * 3600000, [&]() { return allSlot(w, mem, 301); });
  for (int i : mem) CHECK(slotFw(w, i) == 301, "%s filled fw-301 (slot %u): %s", w[i].name.c_str(), slotFw(w, i), w.cli(i, "fw push-status").c_str());
  noInstall(w, mem, 301);
}

// P5 a node with no usable route (7 stock relays: more than MAX_HOPS): it reports STUCK and the source shows it; past the campaign's end
// it stops its own pulls
static void p_stuck() {
  printf("P5 fw push to a node out of route reach: STUCK reported, retries end with the campaign\n");
  pushTune(); fw_ota::g_tune.camp_life_ms = 12 * 3600000u; World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx; int prev = s;
  for (int k = 1; k <= 7; k++) { int x = w.add("X" + std::to_string(k), false).idx; w.link(prev, x, 8, 0.02); prev = x; }
  int z = w.add("Z").idx; w.link(prev, z, 8, 0.02);
  w.seedImage(s, 301, 64000, 9, true); w.seedImage(z, 300, 60000, 1, true);
  circle(w, {s, z}); w.run(150000);
  const uint32_t t0 = g_sim_now;
  w.cli(s, "fw push");
  w.run(13 * 3600000, [&]() { return has(w.cli(s, "fw push-status"), "1 stuck"); });
  std::string st = w.cli(s, "fw push-status");
  CHECK(has(st, "1 stuck"), "S shows Z stuck: %s", st.c_str());
  CHECK(slotFw(w, z) != 301, "Z could not fill (slot %u)", slotFw(w, z));
  if (g_sim_now < t0 + 13 * 3600000u) w.run(t0 + 13 * 3600000u - g_sim_now);   // past the campaign's end
  const uint32_t ip = w[z].xfer->stats.island_pulls; w.run(8 * 3600000);
  CHECK(w[z].xfer->stats.island_pulls == ip, "no island pulls after the campaign ended (%u -> %u)", ip, w[z].xfer->stats.island_pulls);
  printf("  Z: %u island pulls; %s\n", w[z].xfer->stats.island_pulls, w.cli(z, "fw push-status").c_str());
}

// P6 a node that comes up 30 h after the push, behind a stock relay (no member in zero-hop reach): its newcomer query finds the campaign
static void p_newcomer() {
  printf("P6 a node that comes up 30 h after the push finds it by its newcomer query\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  int n = behindStock(w, "N", f.r2, true, 300); circle(w, {n}); w.run(60000);
  powerCut(w, n, 30 * 3600000u); w.log("TEST: N off for 30 h");
  w.cli(f.s, "fw push");
  auto answers = [&]() { uint32_t k = 0; for (auto& x : w.nodes) if (x->xfer) k += x->xfer->stats.camp_answers; return k; };
  w.run(30 * 3600000u - 60000); const uint32_t ans0 = answers();   // (the answers to N's query only: other nodes asked earlier)
  w.run(120000);
  CHECK(slotFw(w, n) != 301, "N was off: nothing yet (slot %u)", slotFw(w, n));
  w.run(12 * 3600000, [&]() { return slotFw(w, n) == 301; });
  CHECK(slotFw(w, n) == 301, "N filled fw-301 after its boot (slot %u): %s", slotFw(w, n), w.cli(n, "fw push-status").c_str());
  const uint32_t ans = answers() - ans0;
  CHECK(w[n].xfer->stats.camp_queries == 1 && ans >= 1 && ans <= 3, "one newcomer query, 1-3 answers (queries %u, answers %u)", w[n].xfer->stats.camp_queries, ans);
  noInstall(w, {n}, 301);
}

// P7 a link that appears 3 days after the push (the node was up all along): the daily reminder reaches it; about one reminder per day
// for the whole circle
static void p_reminder() {
  printf("P7 a link that appears 3 days after the push: the daily reminder; reminders per day for the whole circle\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  int l = behindStock(w, "L", f.r2, false, 300); circle(w, {l}); w.run(60000);
  const uint32_t t0 = g_sim_now;
  w.cli(f.s, "fw push");
  w.run(72 * 3600000u); const uint32_t r0 = remindersAll(w);
  w.link(l - 1, f.r2, 8, 0.05); w.log("TEST: the stock relay XL reaches R2");
  w.run(36 * 3600000, [&]() { return slotFw(w, l) == 301; });
  CHECK(slotFw(w, l) == 301, "L filled fw-301 after its link appeared (slot %u): %s", slotFw(w, l), w.cli(l, "fw push-status").c_str());
  const double days = (g_sim_now - t0) / 86400000.0, rpd = remindersAll(w) / days;
  CHECK(rpd >= 0.5 && rpd <= 2.0, "%.2f reminders per day for the whole circle (0.5-2; %u in %.1f days, %u in the first 3)", rpd, remindersAll(w), days, r0);
  noInstall(w, {l}, 301);
}

// P9 `fw push-cancel`: nobody joins the cancelled campaign afterwards, nothing installs, filled slots stay; a newer push replaces it
static void p_cancel() {
  printf("P9 fw push-cancel, then a newer push\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  int n = behindStock(w, "N", f.r2, true, 300); circle(w, {n}); w.run(60000);
  powerCut(w, n, 0x7FFFFFFF); w.log("TEST: N off until after the cancel");
  const std::vector<int> mem = {f.r1, f.r2, f.p, f.c1, f.c2, f.c3, f.b1};
  w.cli(f.s, "fw push");
  auto pct = [&](int i) { auto* x = w[i].xfer.get(); return x && x->_man.n_chunks ? 100u * x->_cm.have / x->_man.n_chunks : 0u; };
  w.run(12 * 3600000, [&]() { return slotFw(w, f.r1) == 301 && (pct(f.r2) >= 30 || slotFw(w, f.r2) == 301); });
  std::string c = w.cli(f.s, "fw push-cancel");
  CHECK(has(c, "OK") && has(c, "cancel"), "'fw push-cancel': %s", c.c_str());
  w.run(2 * 3600000); w[n].down_until = g_sim_now + 1000; w.log("TEST: N comes up after the cancel");
  w.run(12 * 3600000);
  CHECK(slotFw(w, f.r1) == 301, "R1 keeps its filled slot (slot %u)", slotFw(w, f.r1));
  CHECK(slotFw(w, n) != 301 && w[n].xfer->stats.island_pulls == 0, "N does not join the cancelled push (slot %u, island pulls %u): %s",
        slotFw(w, n), w[n].xfer->stats.island_pulls, w.cli(n, "fw push-status").c_str());
  std::vector<int> all = mem; all.push_back(n); noInstall(w, all, 301);
  w.seedImage(f.s, 302, 66000, 11, true); w.boot(w[f.s], POWER_RESETREAS_SREQ_Msk); w.run(120000);   // S now runs fw-302
  std::string r = w.cli(f.s, "fw push");
  CHECK(has(r, "fw-302"), "a newer push: %s", r.c_str());
  w.run(30 * 3600000, [&]() { return allSlot(w, all, 302); });
  for (int i : all) CHECK(slotFw(w, i) == 302, "%s filled the newer fw-302 (slot %u): %s", w[i].name.c_str(), slotFw(w, i), w.cli(i, "fw push-status").c_str());
}

// P10 `fw pull fw-N fleet` runs the same campaign (origin = the requester, invite once its slot is complete): a node that comes up later
// behind a stock relay finds it
static void p_fleetcamp() {
  printf("P10 a fleet pull starts the same campaign: a late node finds it\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  FleetNet f = fleetNet(w, 0.10);
  int n = behindStock(w, "N", f.r2, true, 300); circle(w, {n}); w.run(60000);
  powerCut(w, n, 10 * 3600000u); w.log("TEST: N off for 10 h");
  w.cli(f.p, "fw pull " + idHex(w, f.s) + " duty 1 fleet");
  w.run(10 * 3600000u + 60000);
  w.run(14 * 3600000, [&]() { return slotFw(w, n) == 301; });
  CHECK(slotFw(w, n) == 301, "N filled fw-301 from the fleet pull's campaign (slot %u): %s", slotFw(w, n), w.cli(n, "fw push-status").c_str());
  CHECK(w[n].app_fw != 301, "N did not install (app %u)", w[n].app_fw);
}

// (debug, on request: P3X) the P3 island without a push: does a plain pull from 5 hops (2 of them stock relays) reach the source?
static void p_island_plain() {
  printf("P3X plain pull from I3 (5 hops, 2 stock relays)\n");
  pushTune(); World w; W = &w; g_sim_now = 0;
  int s = w.add("S").idx, a = w.add("A").idx, x1 = w.add("X1", false).idx, x2 = w.add("X2", false).idx;
  int i1 = w.add("I1").idx, i2 = w.add("I2").idx, i3 = w.add("I3").idx;
  w.link(s, a, 8, 0.05); w.link(a, x1, 8, 0.05); w.link(x1, x2, 8, 0.05); w.link(x2, i1, 8, 0.05); w.link(i1, i2, 8, 0.05); w.link(i2, i3, 8, 0.05);
  w.seedImage(s, 301, 64000, 9, true); for (int i : {a, i1, i2, i3}) w.seedImage(i, 300, 60000, 1, true);
  circle(w, {s, a, i1, i2, i3}); for (int i : {s, a, i1, i2, i3}) { w.cli(i, "fw duty 1"); w.cli(i, "fw autoapply off"); }
  w.run(150000);
  w.cli(i3, "fw pull " + idHex(w, s) + " duty 1");
  for (int k = 0; k < 20; k++) { w.run(60000); printf("  +%2d min I3: %s\n", k + 1, w.cli(i3, "fw pull-status").substr(0, 120).c_str()); }
}
