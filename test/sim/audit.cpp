// Text audit (owner 2026-10-07, after F1/F2 on hardware): while every scenario runs, each LoRa-OTA node's status texts are read 1 s after
// every boot and then every 15 s, and compared with the node's REAL state. A text that claims something the state contradicts (a listener
// showing a duty it does not hold, "not installed here" for an installed image, a cut-off reply) fails the run.
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <Arduino.h>
#include <Mesh.h>
#define private public          // the audit reads the state behind the text (this file only; the layout is unchanged)
#define protected public
#include "sim.h"
#undef private
#undef protected

namespace sim {
int g_audit_fail = 0, g_audit_texts = 0;
static std::map<std::string, int> g_kinds;      // each kind of mismatch printed 3 times at most

static void bad(Node& n, const char* kind, const std::string& cmd, const std::string& r, const std::string& why) {
  g_audit_fail++;
  if (++g_kinds[kind] <= 3) printf("  AUDIT FAIL [%s] %s '%s': %s\n     '%s'\n", kind, n.name.c_str(), cmd.c_str(), why.c_str(), r.c_str());
}
static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }
static std::string pct(uint8_t p) { char b[8]; snprintf(b, sizeof b, "%u.%02u", p / 100, p % 100); return b; }

void audit(Node& n) {
  fw_ota::FwOtaTransfer& x = *n.xfer;
  // ---- the pull ----
  std::string ps = W->cli(n.idx, "fw pull-status"); g_audit_texts++;
  if (x._phase == fw_ota::PH_DOWNLOADING && x._erased && !x._paused && !has(ps, "WAITING")) {
    char want[24]; snprintf(want, sizeof want, " %u/%u chunks", (unsigned)x._cm.have, (unsigned)x._man.n_chunks);
    if (!has(ps, want)) bad(n, "chunks", "fw pull-status", ps, std::string("the node holds") + want);
    const bool lis = x._listening || x._gap_phase || (x._fill && !x._sess_set);   // (rc17, C36) it requests no windows of its own
    if (x._fill) {
      if (!has(ps, "filling") || !has(ps, "no install")) bad(n, "fill", "fw pull-status", ps, "a fleet fill (never installed) - the text does not say so");
      if (x._fill_paused != has(ps, "PAUSED")) bad(n, "fill", "fw pull-status", ps, x._fill_paused ? "the fill is paused" : "the fill is not paused");
      if (has(ps, " win=") && lis) bad(n, "listener duty", "fw pull-status", ps, "a collector holds no duty share - the text shows one");
    } else if (lis) {
      if (!has(ps, "saving shared stream")) bad(n, "listener", "fw pull-status", ps, "it saves a shared stream - the text does not say so");
      if (has(ps, " win=")) bad(n, "listener duty", "fw pull-status", ps, "a listener holds no duty share - the text shows one");
    } else {
      if (has(ps, "shared stream")) bad(n, "listener", "fw pull-status", ps, "it requests its own chunks - the text says shared stream");
      const uint8_t d = x._agreed_pct ? x._agreed_pct : x.askedPct();
      if (!has(ps, ("duty " + pct(d)).c_str())) bad(n, "duty", "fw pull-status", ps, "it pulls at duty " + pct(d));
    }
  }
  if (ps.rfind("serving '", 0) == 0) {                    // the server's line: the shares it shows never add up to more than its duty
    unsigned sum = 0; for (size_t p = ps.find(" at "); p != std::string::npos; p = ps.find(" at ", p + 1)) {
      unsigned a = 0, b = 0, c = 0, e = 0; const char* q = ps.c_str() + p + 4;
      if (sscanf(q, "%u.%2u->%u.%2u", &a, &b, &c, &e) == 4) sum += c * 100 + e;   // a re-split under way: its new share counts
      else if (sscanf(q, "%u.%2u", &a, &b) == 2) sum += a * 100 + b; }
    if (sum > x._duty_pct + 1) {
      std::string tb; char t[96];
      for (auto& d : x._dsess) if (d.sess) { snprintf(t, sizeof t, " [%08X pct %u tgt %u act %d listen %d top %u ask %u cap %d]", d.sess, d.pct, d.target,
                                                      x.sessActive(d, millis()), d.listen, d.top, d.ask, d.capable); tb += t; }
      bad(n, "server shares", "fw pull-status", ps, "shares shown add up to " + pct(sum > 255 ? 255 : sum) + " > its duty " + pct(x._duty_pct) + tb);
    }
  }
  std::string du = W->cli(n.idx, "fw duty"); g_audit_texts++;
  if (x._phase == fw_ota::PH_DOWNLOADING && x._listening && has(du, "agreed"))
    bad(n, "listener duty", "fw duty", du, "a listener has no agreed share");
  // ---- serving ----
  std::string sv = W->cli(n.idx, "fw serve"); g_audit_texts++;
  if (x._circle_set && !x._serve_disabled) {
    fw_ota::SlotHeader h; const bool valid = fw_ota::readSlotHeader(h) && fw_ota::slotValid();
    const bool installed = valid && n.app_fw == h.fw_version;
    if (has(sv, "slot empty") && valid && !x.slotInUse()) bad(n, "serve", "fw serve", sv, "the slot holds a valid image");
    if (has(sv, "refused") && installed) bad(n, "serve", "fw serve", sv, "the slot image IS installed here");
    if (has(sv, "no bootloader") && !x._bl_probed) bad(n, "serve", "fw serve", sv, "the bootloader has not been checked yet");
  }
  // ---- (rc17, C39) 'fw slot': fw-N, size, CRC and the states against the header and the app region ----
  std::string sl = W->cli(n.idx, "fw slot"); g_audit_texts++;
  fw_ota::SlotHeader h;
  if (x._fill_paused && x._phase == fw_ota::PH_DOWNLOADING) {   // (rc19, C42) a paused fill: "partial fw-N, xx% of its chunks"
    char want[48]; snprintf(want, sizeof want, "partial fw-%u, %u%% of", (unsigned)x._man.fw_version, x._man.n_chunks ? (unsigned)(100u * x._cm.have / x._man.n_chunks) : 0u);
    if (!has(sl, want)) bad(n, "slot", "fw slot", sl, std::string("a paused fill: ") + want);
  }
  else if (x._phase == fw_ota::PH_DOWNLOADING || x._phase == fw_ota::PH_VERIFYING || (x._phase == fw_ota::PH_OFFERING && (x._erased || x._man.n_chunks))) { if (!has(sl, "being pulled into")) bad(n, "slot", "fw slot", sl, "a pull writes the slot now"); }
  else if (!fw_ota::readSlotHeader(h)) { if (sl != "slot empty") bad(n, "slot", "fw slot", sl, "the slot is empty"); }
  else {
    char want[64]; snprintf(want, sizeof want, "fw-%u, %u B, crc %08X", (unsigned)h.fw_version, (unsigned)h.raw_len, (unsigned)h.raw_crc32);
    if (!has(sl, want)) bad(n, "slot", "fw slot", sl, std::string("the header says ") + want);
    const bool valid = fw_ota::slotValid(), inst = valid && n.app_fw == h.fw_version;
    if (has(sl, "installed here") && !has(sl, "not installed here") && !inst) bad(n, "slot", "fw slot", sl, "the slot image is not the running app");
    if (inst && !has(sl, "installed here")) bad(n, "slot", "fw slot", sl, "the slot image IS the running app");
    if (has(sl, "offered to neighbours") && !x._serving) bad(n, "slot", "fw slot", sl, "it is not served");
    if (!valid && !has(sl, "INVALID")) bad(n, "slot", "fw slot", sl, "the slot fails its CRC");
  }
}
}  // namespace sim
