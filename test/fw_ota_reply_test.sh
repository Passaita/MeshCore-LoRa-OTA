#!/bin/bash
# Host test of what the operator READS (owner, 2026-10-05: text bugs were found only on hardware - "(last request 2 min) ago",
# "rssi?" next to a 0-hop SNR, "resumed" over "duty re-agreed"). The REAL reply code is extracted on every run from
# src/helpers/fw_ota/FwOtaTransfer.cpp (cmdStatus, cmdDuty, cmdWdt + their text helpers; the types from FwOtaTransfer.h, ChunkMap.h and
# the real Watchdog.h on simulated registers), driven through the states a node really goes through (idle, serving 0/1/2 pulls,
# starting, done, downloading, re-agreed, shared by PACE, waiting, paused, complete, failed, watchdog switching / planned), and EVERY
# reply is checked by the same text rules:
#   fits 160 B with room (not cut off), brackets and quotes balanced, nothing after a closing bracket that belongs inside ("..) ago"),
#   no double / leading / trailing space, no empty "()" or "| |", no printf leftovers ("%" + letter), no wrapped unsigned numbers
#   (4294967..., 65535), no "-1" counters, duty always "x.yy";
# plus what each state must say. All replies are printed so a reviewer reads them once per change.
# usage: test/fw_ota_reply_test.sh [-v]
set -e
cd "$(dirname "$0")/.."
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/inc"
cat > "$W/inc/Arduino.h" <<'EOF'
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
struct WdtRegs { uint32_t CONFIG, CRV, RREN, TASKS_START, RR[8], RUNSTATUS; };
static WdtRegs g_wdt_regs;
#define NRF_WDT (&g_wdt_regs)
#define WDT_CONFIG_SLEEP_Pos 0
#define WDT_CONFIG_SLEEP_Run 1
#define WDT_CONFIG_HALT_Pos 3
#define WDT_CONFIG_HALT_Pause 0
#define WDT_RREN_RR0_Msk 1
#define POWER_RESETREAS_RESETPIN_Msk 0x1
#define POWER_RESETREAS_DOG_Msk 0x2
#define POWER_RESETREAS_SREQ_Msk 0x4
#define POWER_RESETREAS_LOCKUP_Msk 0x8
#define POWER_RESETREAS_OFF_Msk 0x10000
static uint32_t g_resetreas;
static inline uint32_t readResetReason() { return g_resetreas; }
static uint32_t g_now = 100000000;
static inline uint32_t millis() { return g_now; }
EOF
cat > "$W/inc/nrf_wdt.h" <<'EOF'
#pragma once
enum nrf_wdt_rr_register_t { NRF_WDT_RR0 = 0 };
static inline bool nrf_wdt_started(WdtRegs* w) { return w->RUNSTATUS != 0; }
static inline void nrf_wdt_reload_request_set(WdtRegs* w, nrf_wdt_rr_register_t r) { w->RR[r] = 0x6E524635UL; }
EOF
python3 - "$W" <<'PY'
import sys, re
W = sys.argv[1]
src = open("src/helpers/fw_ota/FwOtaTransfer.cpp").read()
hdr = open("src/helpers/fw_ota/FwOtaTransfer.h").read()
def fn(text, sig):
    i = text.index(sig); j = text.index("{", i); d = 0
    for k in range(j, len(text)):
        if text[k] == "{": d += 1
        elif text[k] == "}":
            d -= 1
            if d == 0: return text[i:k + 1]
def block(text, start, end="};"):
    i = text.index(start); return text[i:text.index(end, i) + len(end)]
manifest = block(hdr, "struct XferManifest {")
phase = block(hdr, "enum XferPhase : uint8_t {")
dsess = block(hdr, "struct DutySess {")
peerinfo = re.search(r"static const uint8_t KIND_APP = 0, KIND_BOOTLOADER = 1;", hdr).group(0) + "\n" + block(hdr, "struct PeerInfo {") + "\n" + "\n".join(re.findall(r"static const uint8_t (?:MAX_PEERS|SWARM_BLK|KIND_SWARM|SWARM_HOLDERS)\s*=[^;]*;", hdr)) + "\n" + block(hdr, "struct ImgInfo {")
consts = "\n".join(re.findall(r"static const uint\d+_t\s+(?:DUTY_MIN_PCT|DUTY_DEFAULT_PCT|DUTY_FULL_PCT|XFER_WINDOW|SERVE_ACTIVE_MS|DUTY_ACTIVE_MS|MAX_RETRIES|POWER_CRIT_MV|MAX_HOPS)\s*=[^;]*;", hdr + src))
helpers = "\n".join(fn(src, s) for s in ("static void agoText(", "static bool serveActive(", "static uint32_t shareHoldMs(", "static uint8_t chanPct(", "static uint32_t holderRate(", "static uint8_t dutyParse("))
members = [fn(src, s) for s in ("bool FwOtaTransfer::cmdStatus(", "bool FwOtaTransfer::cmdDuty(", "bool FwOtaTransfer::cmdWdt(",
                                "FwOtaTransfer::Txt16 FwOtaTransfer::lastAnsText()", "bool FwOtaTransfer::sessActive(",
                                "bool FwOtaTransfer::cmdMinBatt(", "uint8_t FwOtaTransfer::powerState()",
                                "bool FwOtaTransfer::cmdPeers(", "bool FwOtaTransfer::cmdPeersHosts(", "int FwOtaTransfer::imageList(",
                                "uint32_t FwOtaTransfer::holderScore(")]
prog = r'''
#include <Arduino.h>
#include <string>
#include <vector>
#include <cassert>
#include "src/helpers/fw_ota/Watchdog.h"
#include "src/helpers/fw_ota/ChunkMap.h"
namespace fw_ota { Watchdog g_wdt; }
using namespace fw_ota;
static const char* DUTY_FILE = "/d"; static const char* WDT_FILE = "/w"; static const char* MINBATT_FILE = "/m";
struct File { operator bool() const { return false; } void write(const uint8_t*, int) {} void close() {} };
static const int FILE_O_WRITE = 1;
struct FS { void remove(const char*) {} File open(const char*, int = 0) { return File(); } };
static inline uint8_t rtCount(uint8_t enc) { return enc & 0x3F; }
static const uint8_t MAX_ROUTE_BYTES = 18;
''' + consts + "\n" + peerinfo.split("\n")[0] + "\n" + manifest + "\n" + phase + "\n" + "\n".join(peerinfo.split("\n")[1:]) + "\n" + helpers + r'''
class FwOtaTransfer { public:
  struct Txt16 { char c[16]; };
  ''' + dsess + r'''
  static const uint8_t DUTY_SESS_N = 6;   // = FwOtaTransfer.h (rc15)
  bool _listening = false; void dutyRebalance(uint32_t) {}
  DutySess _dsess[DUTY_SESS_N] = {};
  FS* _fs = nullptr;
  bool _resolving = false, _res_bl = false, _res_scanning = false; uint8_t _res_id[3] = {0}, _res_len = 2; uint16_t _res_scans = 0;
  uint32_t _res_next_ms = 0; char _res_msg[48] = {0};
  XferPhase _phase = PH_IDLE; bool _paused = false; uint32_t _pause_until_ms = 0;
  XferManifest _man = {}, _serve_man = {}, _bl_man = {}; ChunkMap _cm;
  bool _resuming = false; uint16_t _attempts = 0; uint32_t _wait_started_ms = 0, _last_ans_ms = 0; char _last_err[48] = {0};
  uint8_t _ret_len = 0, _route_len = 0; bool _ret_set = false;
  uint8_t _agreed_pct = 0, _duty_pct = 30, _pull_duty_pct = 0, _win_target = 8; uint16_t _retries = 0;
  uint32_t _crc_bad = 0; int32_t _crc_bad_idx = -1; uint32_t _mac_fail = 0; uint32_t _rate_t0_ms = 0; uint16_t _rate_have0 = 0;
  bool _serving = false, _serve_disabled = false;
  uint32_t _serve_last_req_ms = 0, _serve_offer_reqs = 0, _serve_reqs = 0, _heals_sent = 0, _bl_offer_reqs = 0, _bl_last_idx = 0;
  uint16_t _wdt_resets = 0; bool _wdt_planned = false; uint32_t _wdt_hang_at = 0;
  uint8_t askedPct() const { return _pull_duty_pct ? _pull_duty_pct : _duty_pct; }
  uint32_t _reagree_at_ms = 0; bool _reagree_why = false;
  uint16_t _batt_mv = 0, _minbatt_mv = 0; uint8_t powerState() const; bool cmdMinBatt(const char* arg, char* reply);
  PeerInfo _peers[MAX_PEERS] = {}; bool _scan_active = false; uint32_t _scan_end_ms = 0;
  bool _scanned = true, _gap_phase = false, _fill = false, _sess_set = true; struct { uint32_t repairs = 0; } stats;   // (rc17)
  bool _fill_paused = false;                                                                                         // (rc19, C42)
  uint8_t _fill_mode = 2; static const uint8_t FLEET_FILL_EMPTY = 1;                                                // (rc20, C52)
  uint32_t chunkAirMs(uint8_t) const { return 600; }
  bool cmdPeers(const char* arg, char* reply); bool cmdPeersHosts(const char* arg, char* reply);
  int imageList(ImgInfo* out, int cap) const; uint32_t holderScore(const PeerInfo& p) const;
  Txt16 lastAnsText() const; bool sessActive(const DutySess& d, uint32_t now) const;
  bool cmdStatus(char* reply); bool cmdDuty(const char* arg, char* reply); bool cmdWdt(const char* arg, char* reply);
};
''' + "\n".join(members) + r'''

static int g_checks = 0, g_fail = 0; static bool g_verbose = false;
static void bad(const std::string& state, const std::string& r, const char* why) { printf("FAIL [%s] %s:\n   '%s'\n", state.c_str(), why, r.c_str()); g_fail++; }
// the text rules - every reply, every state
static void lint(const std::string& state, const char* reply) {
  std::string r(reply); g_checks++;
  if (g_verbose) printf("  [%s] %s\n", state.c_str(), reply);
  if (r.size() >= 158) bad(state, r, "too long - cut off at 160 B (or no room left)");
  if (r.empty()) bad(state, r, "empty reply");
  if (r.front() == ' ' || r.back() == ' ') bad(state, r, "leading / trailing space");
  if (r.find("  ") != std::string::npos) bad(state, r, "double space");
  int par = 0, sq = 0, q = 0;
  for (size_t i = 0; i < r.size(); i++) { char c = r[i];
    if (c == '(') par++; if (c == ')') { if (--par < 0) break; } if (c == '[') sq++; if (c == ']') sq--; if (c == '\'') q++;
    if (c == '%' && i + 1 < r.size() && isalpha((unsigned char)r[i + 1])) bad(state, r, "printf leftover '%x'"); }
  if (par || sq) bad(state, r, "unbalanced brackets");
  if (q % 2) bad(state, r, "unbalanced quotes");
  for (const char* x : {") ago", "()", "( ", " )", "| |", "||", " ,", ",,", "4294967", "65535", "=-1", " -1 ", "@-1"})
    if (r.find(x) != std::string::npos) { std::string m = std::string("contains '") + x + "'"; bad(state, r, m.c_str()); }
  // a duty is always "x.yy": "duty 0.3" or "at 1.0" would read differently from the CLI's own format
  for (const char* k : {"duty ", " at ", "agreed "}) for (size_t p = r.find(k); p != std::string::npos; p = r.find(k, p + 1)) {
    size_t s = p + strlen(k); if (s < r.size() && isdigit((unsigned char)r[s])) {
      if (!(s + 3 < r.size() + 1 && r[s + 1] == '.' && isdigit((unsigned char)r[s + 2]) && isdigit((unsigned char)r[s + 3]))) bad(state, r, "duty not x.yy"); } }
}
static void must(const std::string& state, const char* reply, const char* want) { g_checks++; if (!strstr(reply, want)) { std::string m = std::string("must contain '") + want + "'"; bad(state, reply, m.c_str()); } }
static void mustNot(const std::string& state, const char* reply, const char* no) { g_checks++; if (strstr(reply, no)) { std::string m = std::string("must not contain '") + no + "'"; bad(state, reply, m.c_str()); } }

static FwOtaTransfer fresh() { FwOtaTransfer t; strcpy(t._serve_man.name, "fw-294"); t._serve_man.n_chunks = 1621; t._bl_man.n_chunks = 260; strcpy(t._man.name, "@3823F1DC"); t._man.n_chunks = 1621; t._cm.reset(1621); return t; }
static FwOtaTransfer::DutySess sess(uint32_t id, uint16_t top, uint8_t pct, uint32_t ago_ms) { FwOtaTransfer::DutySess d = {}; d.sess = id; d.keyed = true; d.top = top; d.pct = pct; d.last_ms = g_now - ago_ms; d.ivl_ms = 20000; return d; }

int main(int argc, char**) {
  g_verbose = argc > 1;
  char r[160];
  // ---- puller side ----
  { auto t = fresh(); t.cmdStatus(r); lint("idle", r); must("idle", r, "idle"); t.cmdDuty("", r); lint("idle duty", r); }
  { auto t = fresh(); t._phase = PH_DOWNLOADING; t._agreed_pct = 100; for (int i = 0; i < 433; i++) t._cm.set(i); t._rate_t0_ms = g_now - 1600000; t._rate_have0 = 0;
    strcpy(t._last_err, "resumed"); t.cmdStatus(r); lint("downloading", r); must("downloading", r, "433/1621"); must("downloading", r, "duty 1.00"); must("downloading", r, "c/m ETA");
    t.cmdDuty("", r); lint("downloading duty", r); must("downloading duty", r, "agreed 1.00");
    strcpy(t._last_err, "duty re-agreed 0.10 -> 1.00"); t.cmdStatus(r); lint("re-agreed", r); must("re-agreed", r, "duty re-agreed 0.10 -> 1.00");
    strcpy(t._last_err, "duty shared by the server"); t._agreed_pct = 50; t.cmdStatus(r); lint("shared", r); must("shared", r, "duty 0.50");
    t._last_err[0] = 0; t.cmdStatus(r); lint("downloading, no reason", r);
    t._rate_t0_ms = g_now - 5000; t.cmdStatus(r); lint("downloading, measuring", r);
    t._crc_bad = 2; t._crc_bad_idx = 117; t._mac_fail = 1; t._retries = 3; t._route_len = 0x42; t._ret_set = true; t._ret_len = 0x41; t.cmdStatus(r); lint("downloading, relays + errors", r); }
  { auto t = fresh(); t._phase = PH_OFFERING; t._resuming = true; t._attempts = 7; t._wait_started_ms = g_now - 412000; t._last_ans_ms = g_now - 3 * 60000;
    for (int i = 0; i < 82; i++) t._cm.set(i); strcpy(t._last_err, "server silent - waiting"); t.cmdStatus(r); lint("waiting", r); must("waiting", r, "WAITING");
    t._last_ans_ms = 0; t._last_err[0] = 0; t.cmdStatus(r); lint("waiting, no answer, no reason", r); t.cmdDuty("", r); lint("waiting duty", r); must("waiting duty", r, "at the offer"); }
  { auto t = fresh(); t._phase = PH_OFFERING; t._attempts = 2; t.cmdStatus(r); lint("offering", r); }
  { auto t = fresh(); t._phase = PH_DOWNLOADING; t._fill = true; t._fill_paused = true; t._sess_set = false; t._man.fw_version = 9304;   // (rc19, C42)
    for (int i = 0; i < 1520; i++) t._cm.set(i); strcpy(t._last_err, "fleet pull cancelled by its requester"); t.cmdStatus(r);
    lint("paused fill", r); must("paused fill", r, "PAUSED filling 'fw-9304'"); must("paused fill", r, "pull-resume"); }
  { auto t = fresh(); t._phase = PH_DOWNLOADING; t._fill = true; t._fill_mode = 1; t._sess_set = false; t._man.fw_version = 9304;   // (rc20, C52)
    for (int i = 0; i < 700; i++) t._cm.set(i); t.cmdStatus(r); lint("passive fill", r); must("passive fill", r, "(passive, no install)"); }
  { auto t = fresh(); t._phase = PH_DOWNLOADING; t._paused = true; for (int i = 0; i < 149; i++) t._cm.set(i); t.cmdStatus(r); lint("paused", r); must("paused", r, "PAUSED");
    t._pause_until_ms = g_now + 3 * 3600000 + 5 * 60000; t.cmdStatus(r); lint("paused 3h", r); must("paused 3h", r, "resumes in 3h"); }
  { auto t = fresh(); t._phase = PH_COMPLETE; for (int i = 0; i < 1621; i++) t._cm.set(i); t._agreed_pct = 100; strcpy(t._last_err, "already installed: slot filled, served");
    t._serving = true; t.cmdStatus(r); lint("complete", r); must("complete", r, "COMPLETE"); }
  { auto t = fresh(); t._phase = PH_FAILED; strcpy(t._res_msg, "no server offers @3823F1DC"); t.cmdStatus(r); lint("failed", r); }
  { auto t = fresh(); t._resolving = true; t._res_id[0] = 0x1F; t._res_id[1] = 0x2E; t._res_scans = 3; t._res_next_ms = g_now + 90000; t.cmdStatus(r); lint("resolving", r); }
  // ---- server side (C22) ----
  { auto t = fresh(); t._serving = true; t.cmdStatus(r); lint("serving, none since boot", r); must("serving, none since boot", r, "no active pull (none since boot)");
    t._serve_last_req_ms = g_now - 125000; t._serve_reqs = 462; t._serve_offer_reqs = 4; t.cmdStatus(r); lint("serving, idle 2 min", r); must("serving, idle 2 min", r, "(last request 2 min ago)");
    t._serve_last_req_ms = g_now - (2 * 3600 + 5 * 60) * 1000u; t.cmdStatus(r); lint("serving, idle 2h05m", r); must("serving, idle 2h05m", r, "2h05m ago)");
    t._serve_last_req_ms = g_now - 3 * 86400000u; t.cmdStatus(r); lint("serving, idle 3 d", r); t._mac_fail = 3; t.cmdStatus(r); lint("serving, mac fails", r); must("serving, mac fails", r, "mac_fail=3");
    t.cmdDuty("", r); lint("serving idle duty", r); mustNot("serving idle duty", r, "serving 0"); }
  { auto t = fresh(); t._serving = true; t._serve_last_req_ms = g_now - 4000; t._dsess[0] = sess(0x11, 640, 100, 4000); t.cmdStatus(r); lint("serving 1 pull", r);
    must("serving 1 pull", r, "to 1 pull: 640/1621 (39%) at 1.00"); mustNot("serving 1 pull", r, "pulls");
    t._dsess[1] = sess(0x22, 0, 50, 1000); t.cmdStatus(r); lint("serving + one starting", r); must("serving + one starting", r, "starting at 0.50");
    t._dsess[1].top = 12; t._dsess[0].pct = 50; t.cmdStatus(r); lint("serving 2 pulls", r); must("serving 2 pulls", r, "to 2 pulls:");
    t._dsess[0].top = 1621; t.cmdStatus(r); lint("serving, one done", r); must("serving, one done", r, " done"); mustNot("serving, one done", r, "1621/1621");
    t._dsess[2] = sess(0x33, 1500, 10, 2000); t._dsess[3] = sess(0x44, 1499, 10, 2000); t._mac_fail = 12; t._heals_sent = 4; t.cmdStatus(r); lint("serving 4 pulls (longest)", r);
    t.cmdDuty("", r); lint("serving 4 duty", r); must("serving 4 duty", r, "serving 4 pull(s) at"); }
  // ---- rc16 (F1/F2 on hardware 2026-10-07, sim text audit): a listener, the gap phase, a crowded server - longest values ----
  { auto t = fresh(); t._phase = PH_DOWNLOADING; strcpy(t._man.name, "fw-1234567890123"); t._man.n_chunks = 1706; t._cm.reset(1706);
    for (int i = 105; i < 1700; i++) t._cm.set(i); t._rate_t0_ms = g_now - 6000000; t._rate_have0 = 100; t._agreed_pct = 10; t._listening = true;
    t._crc_bad = 123; t._crc_bad_idx = 1705; t._mac_fail = 99; t._retries = 12; t._route_len = 0x45; t._ret_set = true; t._ret_len = 0x45;
    t.cmdStatus(r); lint("listener, longest", r); must("listener, longest", r, "saving shared stream"); mustNot("listener, longest", r, "duty ");
    t.cmdDuty("", r); lint("listener duty", r); must("listener duty", r, "listening (no share needed)"); mustNot("listener duty", r, "agreed");
    t._listening = false; strcpy(t._last_err, "gaps after the shared stream"); t.cmdStatus(r); lint("gap phase, longest", r);
    must("gap phase, longest", r, "duty 0.10"); t._crc_bad = 0; t._mac_fail = 0; t._retries = 0; t.cmdStatus(r); lint("gap phase", r);
    strcpy(t._man.name, "@1F2E3D4C"); t._route_len = 0x42; t._ret_len = 0x42; t.cmdStatus(r); lint("gap phase, 2 hops", r); must("gap phase, 2 hops", r, "gaps after the shared stream"); }
  { auto t = fresh(); t._serving = true; t._serve_last_req_ms = g_now - 4000; t._serve_man.n_chunks = 1706;
    t._dsess[0] = sess(0x11, 1640, 100, 4000); t._dsess[0].target = 50; t.cmdStatus(r); lint("server re-split", r); must("server re-split", r, "at 1.00->0.50");
    for (int i = 1; i < 6; i++) t._dsess[i] = sess(0x20 + i, 1600 + i, 17, 3000);
    t._serve_offer_reqs = 4321; t._serve_reqs = 65432; t._heals_sent = 321; t._mac_fail = 77; t._bl_offer_reqs = 99; t._bl_last_idx = 244;
    t.cmdStatus(r); lint("server 6 pulls, longest", r); must("server 6 pulls, longest", r, "more");
    t._dsess[4].listen = true; t._dsess[5].listen = true; t._dsess[3].last_ms = g_now - 5 * 60000; t.cmdStatus(r); lint("server pulls + listening + quiet", r);
    must("server pulls + listening + quiet", r, "+2 listening"); must("server pulls + listening + quiet", r, "+1 quiet"); }
  { auto t = fresh(); t._serving = true; t._dsess[0] = sess(0x11, 300, 30, 15 * 60000); t._serve_last_req_ms = g_now - 15 * 60000; t.cmdStatus(r); lint("serving, puller gone 15 min", r);
    must("serving, puller gone 15 min", r, "no active pull (last request 15 min ago)"); }
  { auto t = fresh(); t._serving = true; t._bl_offer_reqs = 2; t._bl_last_idx = 120; t._serve_last_req_ms = g_now - 60000; t.cmdStatus(r); lint("serving + bootloader offers", r); }
  // ---- watchdog (C27) ----
  { memset(&g_wdt_regs, 0, sizeof g_wdt_regs); g_resetreas = POWER_RESETREAS_SREQ_Msk; g_wdt = Watchdog(); g_wdt.start(); auto t = fresh();
    t.cmdWdt("", r); lint("wdt armed", r); must("wdt armed", r, "armed, 10 min");
    g_wdt_regs.CRV = 72000u * 32768u - 1u; g_wdt_regs.RUNSTATUS = 1; g_wdt = Watchdog(); g_wdt.start(); t.cmdWdt("", r); lint("wdt switching", r); must("wdt switching", r, "switching 20 h -> 10 min");
    memset(&g_wdt_regs, 0, sizeof g_wdt_regs); g_resetreas = POWER_RESETREAS_DOG_Msk; g_wdt = Watchdog(); g_wdt.start(); t._wdt_planned = true; t.cmdWdt("", r); lint("wdt planned", r);
    must("wdt planned", r, "last reset: watchdog (planned: timeout change)"); t._wdt_planned = false; t._wdt_resets = 3; t.cmdWdt("", r); lint("wdt 3 resets", r); t.cmdWdt("x", r); lint("wdt usage", r); }
  // ---- rc14: fw duty set - live on both sides ----
  { auto t = fresh(); t.cmdDuty("0.6", r); lint("duty set idle", r); must("duty set idle", r, "re-split at their next window"); mustNot("duty set idle", r, "asks again");
    t._phase = PH_DOWNLOADING; t.cmdDuty("0.3", r); lint("duty set while pulling", r); must("duty set while pulling", r, "our pull asks again now");
    g_checks++; if (!t._reagree_at_ms || !t._reagree_why) bad("duty set while pulling", r, "the running pull must re-offer at once");
    auto u = fresh(); u._phase = PH_DOWNLOADING; u._pull_duty_pct = 50; u.cmdDuty("0.3", r); lint("duty set, pull has its own duty", r); mustNot("duty set, pull has its own duty", r, "asks again");
    u.cmdDuty("7", r); lint("duty bad value", r); must("duty bad value", r, "ERR"); }
  // ---- swarm v1: fw minbatt ----
  { auto t = fresh(); t.cmdMinBatt("", r); lint("minbatt off", r); must("minbatt off", r, "minbatt off");
    t._batt_mv = 4122; t.cmdMinBatt("3400", r); lint("minbatt OK", r); must("minbatt OK", r, ": OK");
    t._batt_mv = 3350; t.cmdMinBatt("", r); lint("minbatt LOW", r); must("minbatt LOW", r, ": LOW");
    t._batt_mv = 3100; t.cmdMinBatt("", r); lint("minbatt CRITICAL", r); must("minbatt CRITICAL", r, ": CRITICAL");
    t.cmdMinBatt("99", r); lint("minbatt usage", r); must("minbatt usage", r, "usage"); t.cmdMinBatt("off", r); lint("minbatt set off", r); }
  // ---- swarm v1: fw peers (images) and fw peers hosts, 0..8 offers (C31: the hint was cut at 160 B with 6 images) ----
  for (int n = 0; n <= 8; n++) {
    auto t = fresh(); char st[32];
    for (int i = 0; i < n; i++) { PeerInfo& p = t._peers[i]; p.used = true; p.id[0] = (uint8_t)(0xA0 + i); p.id[1] = 0x68; p.id[2] = 0x3B; p.id[3] = 0x7E;
      snprintf(p.name, 17, "Repeater-Node-%02d", i); p.kind = i >= 5 ? KIND_BOOTLOADER : KIND_APP; p.fw_version = i >= 5 ? 8 - (i - 5) : 297 - i; p.rssi = -40 - i; p.snr4 = 40 - 8 * i;
      p.route_len = (uint8_t)(i % 3); p.swarm = true; p.sduty = 100; p.spulls = (uint8_t)(i % 2); p.spower = 1; memset(p.digest, 0x10 + i, 8); }
    snprintf(st, sizeof st, "fw peers, %d offers", n); t.cmdPeers("", r); lint(st, r);
    snprintf(st, sizeof st, "fw peers hosts, %d offers", n); t.cmdPeersHosts("", r); lint(st, r);
    if (n >= 6) { t.cmdPeers("6", r); snprintf(st, sizeof st, "fw peers 6, %d offers", n); lint(st, r); } }
  printf("reply texts (C22/C24/C26/C27/C28 states x text rules): %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
'''
open(W + "/t.cpp", "w").write(prog)
PY
g++ -std=c++17 -O1 -g -Wno-format-truncation -fsanitize=address,undefined -fno-sanitize-recover=all -I "$W/inc" -I . -o "$W/t" "$W/t.cpp"
"$W/t" "$@"
# sequence bugs the states above cannot show: a later step must not overwrite an earlier, more useful message
grep -q 'if (strncmp(_last_err, "duty re-agreed", 14) != 0)' src/helpers/fw_ota/FwOtaTransfer.cpp || { echo "FAIL: the resume overwrites 'duty re-agreed' (C28)"; exit 1; }
echo "reply sequence checks: 1 passed"
