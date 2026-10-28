#pragma once
// LoRa-OTA network simulator ("cyber test", owner 2026-10-06): the REAL MeshCore core (Mesh, Dispatcher, Packet, Identity, Utils) and the
// REAL LoRa-OTA sources (FwOtaTransfer.cpp, FwOtaSlots.cpp, FwOtaCli.h) run once per virtual node, on a simulated LoRa channel with a
// virtual clock. Models what our unit tests never did (docs/TEST-STRATEGY-20261006.md): time, several nodes overhearing each other,
// collisions, lossy / one-way links, reboots that keep flash + files + the running watchdog, mixed firmware.
#include <Arduino.h>
#include <Mesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/fw_ota/FwOtaTransfer.h>
#include <helpers/fw_ota/Watchdog.h>
#include <helpers/fw_ota/ClockHold.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace sim {

static const uint32_t FLASH_SIZE = 0x100000;

struct Link { bool up = true; double loss = 0.0; float snr = 10.0f; float rssi = -60.0f; };
struct Fade { uint32_t good_ms, bad_ms; double loss; bool bad = false; uint32_t next = 0; };   // (study) Gilbert-Elliott: a link fades for minutes
struct Tx { int from; std::vector<uint8_t> bytes; uint32_t t0, t1; bool done = false; };
struct Rx { std::vector<uint8_t> bytes; float snr, rssi; };

struct Node;

class Radio : public mesh::Radio {
public:
  Node* n = nullptr;
  std::vector<Rx> rxq; float last_rssi = 0, last_snr = 0; int tx_idx = -1;
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len) override;
  float packetScore(float snr, int) override { return snr > -10 ? 1.0f : 0.1f; }
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override { tx_idx = -1; }
  bool isInRecvMode() const override { return tx_idx < 0; }
  bool isReceiving() override;
  float getLastRSSI() const override { return last_rssi; }
  float getLastSNR() const override { return last_snr; }
};
struct Ms : public mesh::MillisecondClock { unsigned long getMillis() override { return millis(); } };   // per node, from its boot
// (rc21, C57/C58) the node's clock as on the device (AutoDiscoverRTCClock, RTC_RV3028_ONLY): a software clock (raw, with a rate error)
// behind the shared hold logic; with an RTC chip it is set from the chip at every boot and a set writes the chip; without one it starts at
// the build floor after a power loss and resumes where it stopped after a soft reset (MeshCore keeps it in no-init RAM).
struct Rtc : public mesh::RTCClock {
  static const uint32_t T0 = 1791300000, FLOOR = 1772323200;   // the true time at g_sim_now 0; RTC_TIME_MIN (1 Mar 2026)
  bool chip = true; int32_t chip_off = 0;                       // an RTC chip (true time + chip_off)
  double drift_ppm = 0;                                         // the software clock's rate error
  uint32_t base = T0, at_ms = 0, frozen = 0; bool lost = false; // raw = base + (now - at_ms) * (1 + drift)
  fw_ota::ClockHold h{};
  uint32_t mono = 0; bool went_back = false;                    // the time handed out stepped back while the app ran (must never happen)
  uint32_t truth() const { return T0 + g_sim_now / 1000; }
  uint32_t raw() const { return base + (uint32_t)((double)(g_sim_now - at_ms) / 1000.0 * (1.0 + drift_ppm * 1e-6)); }
  void setRaw(uint32_t t) { base = t; at_ms = g_sim_now; }
  uint32_t getCurrentTime() override { uint32_t o = h.now(raw()); if (o < mono) went_back = true; mono = o; return o; }
  void setCurrentTime(uint32_t t) override { if (t < mono) went_back = true; setRaw(t); h.restart(t, t); mono = t; if (chip) chip_off = (int32_t)(t - truth()); }
  void discipline(uint32_t ref) { if (chip) return; if (h.toward(raw(), ref)) { setRaw(ref); h.restart(ref, ref); mono = ref; } }
  void reset(bool power_loss) { frozen = h.now(raw()); lost = power_loss; }   // at the reset: what no-init RAM keeps
  void boot() { setRaw(chip ? truth() + chip_off : lost ? FLOOR : frozen); h.restart(raw(), raw()); mono = raw(); }
  int32_t off() { return (int32_t)(getCurrentTime() - truth()); }
};
struct Rng : public mesh::RNG {
  uint64_t s;
  explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ULL + 7) {}
  void random(uint8_t* d, size_t n) override {   // splitmix64 (the xorshift byte 3 of close seeds was correlated: every node drew ~the same delay)
    for (size_t i = 0; i < n; i++) { uint64_t z = (s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; d[i] = (uint8_t)((z ^ (z >> 31)) >> 56); } }
};

class SimMesh : public mesh::Mesh {
public:
  Node* n;
  struct Host : fw_ota::FloodHost {
    SimMesh* m = nullptr;
    void floodScoped(mesh::Packet* p, uint32_t d, uint8_t hs) override { m->sendFlood(p, d, hs); }
    bool regionSet() const override { return false; }
    bool sendOta(mesh::Packet* p, const uint8_t* path, uint8_t path_len, uint32_t d) override;
    uint32_t airtimeMs(uint32_t len) override;
    bool sharedSecret(uint8_t* out, const uint8_t* peer_pub) override { m->self_id.calcSharedSecret(out, peer_pub); return true; }
    uint8_t clockSource() override;                     // (rc21, C57)
    void clockDiscipline(uint32_t ref) override;
  } host;
  SimMesh(Node* node, mesh::Radio& r, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& t)
    : mesh::Mesh(r, ms, rng, rtc, mgr, t), n(node) { host.m = this; }
  bool allowPacketForward(const mesh::Packet* p) override;
  void onRawDataRecv(mesh::Packet* p) override;
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* p, uint8_t type, const mesh::GroupChannel& ch, uint8_t* data, size_t len) override;
  void logRx(mesh::Packet* p, int len, float score) override;
  void sendOtaPacket(mesh::Packet* p, const uint8_t* path, uint8_t path_len, uint32_t d) {   // = MyMesh::fwSendOta
    p->header &= ~PH_ROUTE_MASK; p->header |= ROUTE_TYPE_DIRECT;
    p->path_len = mesh::Packet::copyPath(p->path, path, path_len);
    getTables()->markSeen(p);
    sendPacket(p, fw_ota::OTA_TX_PRIORITY, d);
  }
};

struct Stats { uint32_t tx = 0, tx_air = 0, tx_ota = 0, floods = 0, disc = 0, rx = 0, collisions = 0, lost = 0, resets = 0, wdt_resets = 0, erases = 0; };
// (study) a solar node: LiFePO4 cell, RX all the time, TX at the 1 W PA's current, a charger fed by the sun; empty = off (the BMS cuts)
// until it charged back to on_pct
struct Battery { bool on = false; double cap_mah = 7000, mah = 4200, idle_ma = 12, tx_ma = 1000, peak_chg_ma = 300, on_pct = 3;
                 double min_mah = 1e9, tx_mah = 0; uint32_t deaths = 0, last_air = 0; bool dead = false; uint32_t dead_ms = 0, dead_since = 0; };

struct Node {
  int idx = 0; std::string name; bool lota = true; bool relay = true; uint8_t flood_max = 64; uint16_t batt_mv = 4000;
  std::vector<uint8_t> flash; Adafruit_LittleFS fs; SimNrfPower power{}; SimNrfUicr uicr{}; WdtRegs wdt{}; uint32_t wdt_fed = 0;
  mesh::LocalIdentity id; uint8_t hash_mode = 1; Rng rng; Radio radio; Ms ms; Rtc rtc;
  std::unique_ptr<StaticPoolPacketManager> mgr; std::unique_ptr<SimpleMeshTables> tables; std::unique_ptr<SimMesh> mesh;
  std::unique_ptr<fw_ota::FwOtaTransfer> xfer; std::unique_ptr<fw_ota::Watchdog> wdog;
  bool hung = false;            // the main loop stopped (a hang: the watchdog must restart it)
  uint32_t down_until = 0;      // off the air (Bluetooth DFU window, power cut)
  uint32_t app_fw = 0;          // the image running now (fw number), set by the simulated bootloader
  uint32_t audit_at = 0;
  uint32_t boot_ms = 0;
  bool app_pending = false;     // the bootloader is installing: the app starts at down_until         // g_sim_now at its last boot (its millis() counts from here)        // next text audit (1 s after a boot, then every 15 s)
  Stats st;
  Battery bat;
};

// ---- the world ----
struct World {
  std::vector<std::unique_ptr<Node>> nodes;
  std::map<std::pair<int, int>, Link> links;   // directed: (a, b) = b hears a
  std::vector<Tx> air;
  uint32_t step_ms = 2;
  Rng chance{12345};
  std::vector<std::string> events;
  bool verbose = false;
  uint32_t apply_ms = 60000;
  std::map<std::pair<int, int>, Fade> fades;   // (study) by (low, high) node index: both directions fade together
  uint32_t day_off_ms = 0;                     // (study) local time of day at g_sim_now = 0
  std::vector<double> weather;                 // (study) sun factor per day (1 = clear)
  uint32_t bat_at = 0;
  void fade(int a, int b, uint32_t good_ms, uint32_t bad_ms, double loss);
  double fadeLoss(int a, int b);
  void powerTick();                            // batteries, chargers, brownouts   // install time in the bootloader (hardware: ~40-90 s for a 260 KB image)

  Node& add(const std::string& name, bool lota = true, bool relay = true);
  void link(int a, int b, float snr = 10, double loss = 0.0, bool both = true);
  void unlink(int a, int b, bool both = true);
  Node& operator[](int i) { return *nodes[i]; }
  int find(const std::string& name) const;
  void boot(Node& n, uint32_t reason);
  void startApp(Node& n);
  void run(uint32_t ms, std::function<bool()> stop = nullptr);
  std::string cli(int i, const std::string& cmd);   // a CLI line on node i (as over serial), the reply
  void seedImage(int i, uint32_t fw, uint32_t raw_len, uint32_t seed, bool installed);   // a holder: slot + (installed: app region)
  void log(const std::string& e);
};
extern World* W;
extern uint32_t g_seed;          // (study) varies every node's RNG and the channel
void audit(Node& n);            // audit.cpp: the node's status texts against its real state
extern int g_audit_fail, g_audit_texts;
void enter(Node& n);            // point every sim_cur_* at this node

}  // namespace sim
