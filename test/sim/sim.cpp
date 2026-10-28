// LoRa-OTA network simulator - core (see sim.h)
#include "sim.h"
#include <helpers/fw_ota/FwOtaCli.h>
#include <nrf_mbr.h>
#include <nrf_soc.h>

uint32_t bootloaderVersion = 0x00090200;          // Adafruit core symbol (the running bootloader's version)
uint32_t g_sim_now = 0;
uint32_t g_sim_boot = 0;
SimSerial Serial;
SimNrfPower* sim_cur_power = nullptr;
SimNrfUicr*  sim_cur_uicr = nullptr;
WdtRegs*     sim_cur_wdt_regs = nullptr;
uint8_t*     sim_cur_flash = nullptr;            // FwOtaSlots.cpp (sim copy) reads/writes this node's flash
// g_wdt / g_xfer: compiled as -Dg_wdt=(*sim_cur_wdt) -Dg_xfer=(*sim_cur_xfer), so FwOtaTransfer.cpp's own definitions become the POINTERS
// fw_ota::sim_cur_wdt / fw_ota::sim_cur_xfer (they live in namespace fw_ota) - enter() points them at the node it runs

namespace sim {
World* W = nullptr;
uint32_t g_seed = 0;
static Node* cur = nullptr;

void enter(Node& n) {
  cur = &n; g_sim_boot = n.boot_ms; sim_cur_power = &n.power; sim_cur_uicr = &n.uicr; sim_cur_wdt_regs = &n.wdt; sim_cur_flash = n.flash.data();
  fw_ota::sim_cur_wdt = n.wdog.get(); fw_ota::sim_cur_xfer = n.xfer.get();
}
}  // namespace sim

// ---- hardware the firmware calls ----
uint32_t readResetReason() { return sim_cur_power ? sim_cur_power->RESETREAS : 0; }
void NVIC_SystemReset() { throw SimReset{0}; }
void sim_wdt_feed(WdtRegs*) { if (sim::cur) sim::cur->wdt_fed = g_sim_now; }
uint32_t sd_mbr_command(sd_mbr_command_t* c) {      // COPY_BL: the bootloader image from the slot to the bootloader area
  if (c->command == SD_MBR_COMMAND_COPY_BL && sim::cur) {
    uint32_t src = (uint32_t)(uintptr_t)c->params.copy_bl.bl_src;   // the firmware passes a flash ADDRESS
    memcpy(sim::cur->flash.data() + 0xF4000, sim::cur->flash.data() + src, c->params.copy_bl.bl_len * 4);
  }
  return 0;
}
// flash_nrf5x_* (Adafruit flash cache) on the node's flash: NOR semantics - an erase sets 0xFF, a write can only clear bits
extern "C" {
void flash_nrf5x_flush(void) {}
bool flash_nrf5x_erase(uint32_t addr) { memset(sim_cur_flash + (addr & ~0xFFFu), 0xFF, 4096); if (sim::cur) sim::cur->st.erases++; return true; }
int flash_nrf5x_write(uint32_t dst, void const* src, uint32_t len) {
  const uint8_t* s = (const uint8_t*)src; for (uint32_t i = 0; i < len; i++) sim_cur_flash[dst + i] &= s[i]; return (int)len; }
int flash_nrf5x_read(void* dst, uint32_t src, uint32_t len) { memcpy(dst, sim_cur_flash + src, len); return (int)len; }
}
const uint8_t* sim_flash_ptr(uint32_t addr) { return sim_cur_flash + addr; }
uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words) {
  flash_nrf5x_write((uint32_t)(uintptr_t)dst, src, words * 4); return NRF_SUCCESS; }

namespace sim {

// ---- the channel ----
uint32_t Radio::getEstAirtimeFor(int len) {        // LoRa SF7, BW 62.5 kHz, CR 4/5, preamble 16, explicit header, CRC on
  const double ts = 2.048;                          // ms per symbol
  int n = 8 + std::max((int)std::ceil((8.0 * len + 16) / 28.0) * 5, 0);
  return (uint32_t)((16 + 4.25 + n) * ts + 0.5);
}
bool Radio::startSendRaw(const uint8_t* bytes, int len) {
  Tx t; t.from = n->idx; t.bytes.assign(bytes, bytes + len); t.t0 = g_sim_now; t.t1 = g_sim_now + getEstAirtimeFor(len);
  W->air.push_back(t); tx_idx = (int)W->air.size() - 1;
  n->st.tx++; n->st.tx_air += t.t1 - t.t0;
  const uint8_t rt = bytes[0] & PH_ROUTE_MASK, pt = (bytes[0] >> PH_TYPE_SHIFT) & PH_TYPE_MASK;
  if (rt == ROUTE_TYPE_FLOOD || rt == ROUTE_TYPE_TRANSPORT_FLOOD) n->st.floods++;   // every flood transmission (own + forwarded) = mesh airtime
  if (pt == PAYLOAD_TYPE_GRP_TXT) n->st.disc++;                                      // discovery-channel traffic (searches, answers, adverts)
  return true;
}
bool Radio::isSendComplete() { return tx_idx < 0 || g_sim_now >= W->air[tx_idx].t1; }
bool Radio::isReceiving() {
  for (auto& t : W->air) if (!t.done && t.from != n->idx && g_sim_now >= t.t0 && g_sim_now < t.t1) {
    auto it = W->links.find({t.from, n->idx}); if (it != W->links.end() && it->second.up) return true; }
  return false;
}
int Radio::recvRaw(uint8_t* bytes, int sz) {
  if (rxq.empty()) return 0;
  Rx r = rxq.front(); rxq.erase(rxq.begin());
  int k = std::min((int)r.bytes.size(), sz); memcpy(bytes, r.bytes.data(), k); last_rssi = r.rssi; last_snr = r.snr;
  n->st.rx++;
  return k;
}

static void deliver() {                              // transmissions that end now reach every node that hears them (half duplex, collisions)
  for (size_t i = 0; i < W->air.size(); i++) {
    Tx& t = W->air[i];
    if (t.done || g_sim_now < t.t1) continue;
    t.done = true;
    for (auto& rn : W->nodes) {
      Node& r = *rn;
      if (r.idx == t.from || g_sim_now < r.down_until) continue;
      auto it = W->links.find({t.from, r.idx}); if (it == W->links.end() || !it->second.up) continue;
      bool busy = false, coll = false;
      for (auto& o : W->air) {                       // r was sending, or another audible transmission overlapped (no capture: both lost)
        if (&o == &t || o.t1 <= t.t0 || o.t0 >= t.t1) continue;
        if (o.from == r.idx) busy = true;
        else { auto jt = W->links.find({o.from, r.idx}); if (jt != W->links.end() && jt->second.up && std::fabs(jt->second.rssi - it->second.rssi) < 6) coll = true; }
      }
      if (busy) continue;
      if (coll) { r.st.collisions++; continue; }
      uint8_t c; W->chance.random(&c, 1);
      const double fl = W->fades.empty() ? 0.0 : W->fadeLoss(t.from, r.idx);
      const double ls = it->second.loss + (1.0 - it->second.loss) * fl;
      if (fl > 0) { uint16_t c2; W->chance.random((uint8_t*)&c2, 2); if (c2 < ls * 65536) { r.st.lost++; continue; } }
      else if (c < it->second.loss * 256) { r.st.lost++; continue; }
      r.radio.rxq.push_back(Rx{t.bytes, it->second.snr, it->second.rssi});
    }
  }
  while (!W->air.empty() && W->air.front().done && W->air.front().t1 + 2000 < g_sim_now) {   // keep recent ones for overlap checks
    bool used = false; for (auto& rn : W->nodes) if (rn->radio.tx_idx == 0) used = true;
    if (used) break;
    W->air.erase(W->air.begin()); for (auto& rn : W->nodes) if (rn->radio.tx_idx > 0) rn->radio.tx_idx--;
  }
}

// ---- the mesh glue = MyMesh's LoRa-OTA hooks ----
bool SimMesh::Host::sendOta(mesh::Packet* p, const uint8_t* path, uint8_t path_len, uint32_t d) { m->sendOtaPacket(p, path, path_len, d); m->n->st.tx_ota++; return true; }
uint32_t SimMesh::Host::airtimeMs(uint32_t len) { return m->n->radio.getEstAirtimeFor((int)len); }
uint8_t SimMesh::Host::clockSource() { return m->n->rtc.chip ? 1 : 0; }
void SimMesh::Host::clockDiscipline(uint32_t ref) { m->n->rtc.discipline(ref); }
bool SimMesh::allowPacketForward(const mesh::Packet* p) { return n->relay && p->getPathHashCount() < n->flood_max; }
void SimMesh::onRawDataRecv(mesh::Packet* p) { if (n->xfer) { n->xfer->noteRssi(n->radio.getLastRSSI()); n->xfer->onPacket(p); } }
int SimMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  return n->xfer ? n->xfer->channelMatch(hash, channels, max_matches) : 0; }
void SimMesh::onGroupDataRecv(mesh::Packet* p, uint8_t type, const mesh::GroupChannel&, uint8_t* data, size_t len) {
  if (n->xfer && type == PAYLOAD_TYPE_GRP_TXT) { n->xfer->noteRssi(n->radio.getLastRSSI()); n->xfer->onDiscovery(p, data, len); } }
void SimMesh::logRx(mesh::Packet* p, int, float) { if (n->xfer) n->xfer->noteRx(p->getPayloadType(), p->payload, p->payload_len, (int8_t)p->_snr); }

// ---- nodes ----
Node& World::add(const std::string& name, bool lota, bool relay) {
  nodes.emplace_back(new Node()); Node& n = *nodes.back();
  n.idx = (int)nodes.size() - 1; n.name = name; n.lota = lota; n.relay = relay;
  n.rng = Rng(1000 + n.idx + 7919ULL * g_seed); n.flash.assign(FLASH_SIZE, 0xFF); n.radio.n = &n;
  n.id = mesh::LocalIdentity(&n.rng);
  n.uicr.NRFFW[0] = 0xF4000; n.uicr.NRFFW[1] = 0xFE000;
  boot(n, 0);
  return n;
}
void World::link(int a, int b, float snr, double loss, bool both) {
  Link l; l.snr = snr; l.rssi = -120 + snr * 4; l.loss = loss; links[{a, b}] = l; if (both) links[{b, a}] = l; }
void World::fade(int a, int b, uint32_t good_ms, uint32_t bad_ms, double loss) {
  Fade f; f.good_ms = good_ms; f.bad_ms = bad_ms; f.loss = loss; f.next = 0; fades[{std::min(a, b), std::max(a, b)}] = f; }
double World::fadeLoss(int a, int b) {
  auto it = fades.find({std::min(a, b), std::max(a, b)}); return it != fades.end() && it->second.bad ? it->second.loss : 0.0; }
static uint32_t expo(uint32_t mean) { uint16_t c; W->chance.random((uint8_t*)&c, 2); double u = (c + 1) / 65537.0; return (uint32_t)(-std::log(u) * mean) + 1000; }
static uint16_t lfpMv(double pct) {                 // LiFePO4 resting voltage by charge (flat plateau 20-90 %)
  static const double P[][2] = {{0, 2800}, {2, 2950}, {5, 3050}, {10, 3150}, {20, 3220}, {50, 3270}, {90, 3310}, {100, 3380}};
  for (int i = 1; i < 8; i++) if (pct <= P[i][0]) return (uint16_t)(P[i - 1][1] + (pct - P[i - 1][0]) / (P[i][0] - P[i - 1][0]) * (P[i][1] - P[i - 1][1]));
  return 3380; }
void World::powerTick() {
  for (auto& f : fades) if (g_sim_now >= f.second.next) { f.second.bad = f.second.next && !f.second.bad; f.second.next = g_sim_now + expo(f.second.bad ? f.second.bad_ms : f.second.good_ms); }
  const uint32_t dt = 10000;                       // every 10 s
  const double h = ((g_sim_now + day_off_ms) % 86400000u) / 3600000.0; const uint32_t day = (g_sim_now + day_off_ms) / 86400000u;
  const double sun = (h > 7.5 && h < 18.0) ? std::sin(M_PI * (h - 7.5) / 10.5) * (day < weather.size() ? weather[day] : 1.0) : 0.0;
  for (auto& np : nodes) {
    Node& n = *np; Battery& b = n.bat; if (!b.on) continue;
    const double tx_ms = n.st.tx_air - b.last_air; b.last_air = n.st.tx_air;
    const double use = b.dead ? 0.0 : b.idle_ma * dt / 3600000.0 + b.tx_ma * tx_ms / 3600000.0;
    b.tx_mah += b.tx_ma * tx_ms / 3600000.0;
    b.mah = std::min(b.cap_mah, b.mah - use + b.peak_chg_ma * sun * dt / 3600000.0);
    if (b.mah < b.min_mah) b.min_mah = b.mah;
    if (!b.dead && b.mah <= 0) {                    // empty: the BMS cuts the board off (flash and files are kept)
      b.mah = 0; b.dead = true; b.deaths++; b.dead_since = g_sim_now; n.down_until = 0xFFFFFFFFu; n.radio.rxq.clear(); n.app_pending = false;
      log(n.name + ": BATTERY EMPTY - off");
    } else if (b.dead && b.mah >= b.cap_mah * b.on_pct / 100) {
      b.dead = false; b.dead_ms += g_sim_now - b.dead_since; n.down_until = g_sim_now; n.radio.rxq.clear(); log(n.name + ": battery back - on"); boot(n, 0);
    }
    if (!b.dead && n.xfer && !n.app_pending) { n.batt_mv = lfpMv(100.0 * b.mah / b.cap_mah); enter(n); n.xfer->noteBattery(n.batt_mv); }
  }
}
void World::unlink(int a, int b, bool both) { links.erase({a, b}); if (both) links.erase({b, a}); }
int World::find(const std::string& name) const { for (auto& n : nodes) if (n->name == name) return n->idx; return -1; }
void World::log(const std::string& e) {
  char t[24]; snprintf(t, sizeof t, "%02u:%02u:%02u.%03u ", g_sim_now / 3600000, (g_sim_now / 60000) % 60, (g_sim_now / 1000) % 60, g_sim_now % 1000);
  events.push_back(t + e); if (verbose) printf("%s%s\n", t, e.c_str()); }

// The simulated bootloader: install a pending app from the slot (uncompressed test images), keep everything else; then the app boots.
static void bootloader(Node& n) {
  enter(n);
  fw_ota::CtrlBlock c;
  if (fw_ota::readCtrl(c) && c.apply_state == fw_ota::APPLY_PENDING) {
    fw_ota::SlotHeader h;
    if (fw_ota::readSlotHeader(h) && h.magic == fw_ota::SLOT_MAGIC && h.compression == fw_ota::COMP_NONE && h.raw_len <= fw_ota::APP_MAX_SIZE) {
      memcpy(n.flash.data() + fw_ota::APP_REGION_START, n.flash.data() + fw_ota::SLOT_ADDR + fw_ota::SLOT_PAYLOAD_OFFSET, h.raw_len);
      n.app_fw = h.fw_version; c.apply_state = fw_ota::APPLY_DONE; c.boot_attempts = 0;
    } else c.apply_state = fw_ota::APPLY_FAILED;
    fw_ota::writeCtrl(c);
    W->log(n.name + ": bootloader applied the slot -> fw-" + std::to_string(n.app_fw));
    if (W->apply_ms) n.down_until = g_sim_now + W->apply_ms;   // the copy / decode takes time: the node is off the air meanwhile
  }
  if (n.wdt.RUNSTATUS) sim_wdt_feed(&n.wdt);           // the bootloader reloads a running watchdog
}

void World::boot(Node& n, uint32_t reason) {
  n.power.RESETREAS = reason;
  n.rtc.reset(reason == 0);                         // (rc21) reason 0 = power on / cut: the software clock is lost
  if (reason) { n.st.resets++; bootloader(n); }
  if (g_sim_now < n.down_until) { n.app_pending = true; return; }   // installing: the app starts when the bootloader is done (run())
  startApp(n);
}

void World::startApp(Node& n) {
  n.app_pending = false;
  n.rtc.boot();                                     // (rc21) the clock as the app finds it (RTC chip, build floor, or kept)
  n.boot_ms = g_sim_now;                            // millis() starts at 0 again (was: the world clock - every "N s after boot" timer had expired)
  g_sim_boot = n.boot_ms;                           // the constructors below already read millis() (before enter())
  n.xfer.reset(); n.mesh.reset(); n.tables.reset(); n.mgr.reset(); n.wdog.reset();
  n.mgr.reset(new StaticPoolPacketManager(32)); n.tables.reset(new SimpleMeshTables());
  n.mesh.reset(new SimMesh(&n, n.radio, n.ms, n.rng, n.rtc, *n.mgr, *n.tables));
  n.mesh->self_id = n.id; n.radio.rxq.clear(); n.radio.tx_idx = -1; n.hung = false; n.audit_at = g_sim_now + 1000;
  if (n.lota) { n.wdog.reset(new fw_ota::Watchdog()); n.xfer.reset(new fw_ota::FwOtaTransfer()); }
  enter(n);
  if (n.lota) n.wdog->start();                      // main.cpp: g_wdt.start() before the mesh
  if (n.wdt.TASKS_START) { n.wdt.TASKS_START = 0; n.wdt.RUNSTATUS = 1; n.wdt_fed = g_sim_now; }   // the hardware starts counting (was missing)
  n.mesh->begin();
  if (n.lota) {
    static char name_buf[64][24]; snprintf(name_buf[n.idx], 24, "%s", n.name.c_str());
    n.xfer->begin(n.mesh.get(), &n.fs, name_buf[n.idx], n.id.pub_key, &n.hash_mode);
    n.xfer->setFloodHost(&n.mesh->host);
    n.xfer->noteBattery(n.batt_mv);
  }
}

void World::run(uint32_t ms, std::function<bool()> stop) {
  const uint32_t end = g_sim_now + ms;
  while (g_sim_now < end) {
    g_sim_now += step_ms;
    deliver();
    if ((!fades.empty() || !weather.empty()) && g_sim_now >= bat_at) { bat_at = g_sim_now + 10000; powerTick(); }
    for (auto& np : nodes) {
      Node& n = *np;
      if (g_sim_now < n.down_until) continue;
      if (n.app_pending) { startApp(n); continue; }
      if (n.lota && n.wdt.RUNSTATUS && g_sim_now - n.wdt_fed > (n.wdt.CRV + 1) / 32768u * 1000u) {   // the watchdog bites
        n.st.wdt_resets++; n.wdt = WdtRegs{}; log(n.name + ": WATCHDOG reset"); boot(n, POWER_RESETREAS_DOG_Msk); continue;
      }
      if (n.hung) continue;
      enter(n);
      try {
        n.mesh->loop();
        if (n.xfer) { n.xfer->tick(); n.wdog->feed(); }
      } catch (SimReset&) {
        log(n.name + ": restart (soft reset)");
        boot(n, POWER_RESETREAS_SREQ_Msk);
        continue;
      }
      if (n.xfer && (int32_t)(g_sim_now - n.audit_at) >= 0) { n.audit_at = g_sim_now + 15000; audit(n); }
    }
    if (stop && (g_sim_now % 1000) < step_ms && stop()) break;
  }
}

std::string World::cli(int i, const std::string& cmd) {
  Node& n = *nodes[i];
  if (n.app_pending) return "(installing - off the air)";
  enter(n);
  char reply[200] = {0};
  try {
    if (!fw_ota::cliHandle(cmd.c_str(), reply)) snprintf(reply, sizeof reply, "(not a fw command)");
  } catch (SimReset&) { boot(n, POWER_RESETREAS_SREQ_Msk); strcpy(reply, "(restarted)"); }
  if (strlen(reply) >= 159) { if (++g_audit_fail <= 20) printf("  AUDIT FAIL [cut] %s '%s': reply cut at 160 B (the firmware truncates silently - F1)\n     '%s'\n", n.name.c_str(), cmd.c_str(), reply); }
  log(n.name + " > " + cmd + " -> " + reply);
  return reply;
}

// A holder: an uncompressed test image in the slot (header page + payload), and - installed - the same bytes in the app region.
void World::seedImage(int i, uint32_t fw, uint32_t raw_len, uint32_t seed, bool installed) {
  Node& n = *nodes[i]; enter(n);
  std::vector<uint8_t> img(raw_len); Rng r(seed); r.random(img.data(), raw_len);
  fw_ota::SlotHeader h{}; h.magic = fw_ota::SLOT_MAGIC; h.payload_len = raw_len; h.raw_len = raw_len; h.compression = fw_ota::COMP_NONE;
  h.hdr_version = 1; h.fw_version = fw; h.payload_crc32 = fw_ota::crc32(img.data(), raw_len); h.raw_crc32 = h.payload_crc32;
  memset(n.flash.data() + fw_ota::SLOT_ADDR, 0xFF, fw_ota::SLOT_SIZE);
  memcpy(n.flash.data() + fw_ota::SLOT_ADDR, &h, sizeof h);
  memcpy(n.flash.data() + fw_ota::SLOT_ADDR + fw_ota::SLOT_PAYLOAD_OFFSET, img.data(), raw_len);
  if (installed) { memcpy(n.flash.data() + fw_ota::APP_REGION_START, img.data(), raw_len); n.app_fw = fw; }
}

}  // namespace sim
