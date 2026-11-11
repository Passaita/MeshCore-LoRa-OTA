// C99 stage 2 (owner 2026-11-06: "the simulator must include a VERSION UPGRADE"): the LoRa-OTA state written by the PREVIOUS release is
// booted by THIS build. test/fw_ota_upgrade_test.sh compiles this file twice - against the previous release's fw_ota + simulator (SAVE)
// and against this tree (LOAD) - so it uses only the World / CLI interface both have (version-neutral).
//   SAVE <dir>: holder A (fw-301) and B (fw-300): circle, duty 0.5, auto-apply off, minbatt 3300, B pulls fw-301 from A; at >= 40 % B's
//               flash + files are written to <dir> (power cut: the state as it was on the flash)
//   LOAD <dir>: the same world with THIS code; B starts on that flash + files and must keep its circle, duty, auto-apply, minbatt, resume
//               the pull from its saved chunks, complete it, serve fw-301 and NOT install it
#define private public
#define protected public
#include "sim.h"
#undef private
#undef protected
#include <cstdio>
#include <cstring>
#include <string>
using namespace sim;

static int g_ok = 0, g_bad = 0;
#define UCHECK(c, ...) do { if (c) g_ok++; else { g_bad++; printf("  FAIL C99: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
static std::string idHex(World& w, int i) { char t[12]; const uint8_t* k = w[i].id.pub_key; snprintf(t, sizeof t, "%02X%02X%02X%02X", k[0], k[1], k[2], k[3]); return t; }
static uint32_t pct(World& w, int i) { auto* x = w[i].xfer.get(); return x && x->_man.n_chunks ? 100u * x->_cm.have / x->_man.n_chunks : 0u; }
static uint32_t slotFw(World& w, int i) { enter(w[i]); fw_ota::SlotHeader h; return fw_ota::readSlotHeader(h) && fw_ota::slotValid() ? h.fw_version : 0; }

static void build(World& w, int& a, int& b) {
  a = w.add("A").idx; b = w.add("B").idx; w.link(a, b, 9, 0.05);
  w.seedImage(a, 301, 64000, 9, true); w.seedImage(b, 300, 60000, 1, true);
  for (int i : {a, b}) w.cli(i, "fw passphrase sim circle delta tango");
  w.run(120000, [&]() { for (int i : {a, b}) if (!has(w.cli(i, "fw passphrase"), "circle ") || has(w.cli(i, "fw passphrase"), "key")) return false; return true; });
}
static bool dumpNode(Node& n, const std::string& d) {
  FILE* f = fopen((d + "/flash.bin").c_str(), "wb"); if (!f) return false; fwrite(n.flash.data(), 1, n.flash.size(), f); fclose(f);
  f = fopen((d + "/files.bin").c_str(), "wb"); if (!f) return false;
  for (auto& e : n.fs.files) { uint32_t nl = e.first.size(), dl = e.second.size(); fwrite(&nl, 4, 1, f); fwrite(e.first.data(), 1, nl, f);
                               fwrite(&dl, 4, 1, f); if (dl) fwrite(e.second.data(), 1, dl, f); }
  fclose(f); return true;
}
static bool loadNode(Node& n, const std::string& d) {
  FILE* f = fopen((d + "/flash.bin").c_str(), "rb"); if (!f) return false;
  const bool ok = fread(n.flash.data(), 1, n.flash.size(), f) == n.flash.size(); fclose(f); if (!ok) return false;
  f = fopen((d + "/files.bin").c_str(), "rb"); if (!f) return false;
  n.fs.files.clear(); uint32_t nl, dl;
  while (fread(&nl, 4, 1, f) == 1) { std::string k(nl, 0); if (fread(&k[0], 1, nl, f) != nl || fread(&dl, 4, 1, f) != 1) break;
    std::vector<uint8_t> v(dl); if (dl && fread(v.data(), 1, dl, f) != dl) break; n.fs.files[k] = v; }
  fclose(f); return true;
}
static void saveText(const std::string& p, const std::string& s) { FILE* f = fopen(p.c_str(), "w"); if (f) { fputs(s.c_str(), f); fclose(f); } }
static std::string readText(const std::string& p) { std::string s; FILE* f = fopen(p.c_str(), "r"); if (!f) return s; char b[256]; while (fgets(b, sizeof b, f)) s += b; fclose(f); return s; }

int main(int argc, char** argv) {
  if (argc != 3 || (strcmp(argv[1], "SAVE") && strcmp(argv[1], "LOAD"))) { printf("usage: sim SAVE|LOAD <dir>\n"); return 2; }
  const std::string d = argv[2];
  World w; W = &w; g_sim_now = 0; int a, b; build(w, a, b);
  if (!strcmp(argv[1], "SAVE")) {
    w.cli(b, "fw duty 0.5"); w.cli(b, "fw autoapply off"); w.cli(b, "fw minbatt 3300"); w[b].batt_mv = 3600; w[b].xfer->noteBattery(3600);
    w.cli(a, "fw duty 1"); w.run(60000);
    w.cli(b, "fw pull " + idHex(w, a) + " duty 0.5");
    w.run(6 * 3600000, [&]() { return pct(w, b) >= 40; });
    const std::string circ = w.cli(b, "fw passphrase"), st = w.cli(b, "fw pull-status");
    printf("C99 SAVE: B at %u %% (%s), %s\n", pct(w, b), st.substr(0, 60).c_str(), circ.c_str());
    if (pct(w, b) < 40 || !dumpNode(w[b], d)) { printf("C99 SAVE failed\n"); return 1; }
    saveText(d + "/expect.txt", circ + "\n" + std::to_string(w[b].xfer->_cm.have) + "\n");
    printf("C99 SAVE ok: %zu files\n", w[b].fs.files.size());
    return 0;
  }
  const std::string exp = readText(d + "/expect.txt"); const std::string circ = exp.substr(0, exp.find('\n'));
  const uint32_t have0 = (uint32_t)atoi(exp.substr(exp.find('\n') + 1).c_str());
  UCHECK(loadNode(w[b], d), "the saved state of B could not be read from %s", d.c_str());
  w[b].batt_mv = 3600; w.boot(w[b], 0); w[b].xfer->noteBattery(3600);   // B starts on the old flash + files (a power cut and back)
  w.cli(a, "fw duty 1"); w.run(30000);
  const std::string c2 = w.cli(b, "fw passphrase"), du = w.cli(b, "fw duty"), aa = w.cli(b, "fw autoapply"), mb = w.cli(b, "fw minbatt");
  UCHECK(c2 == circ, "circle kept: '%s' (was '%s')", c2.c_str(), circ.c_str());
  UCHECK(has(du, "0.5"), "duty 0.5 kept: %s", du.c_str());
  UCHECK(has(aa, "OFF") || has(aa, "off"), "auto-apply off kept: %s", aa.c_str());
  UCHECK(has(mb, "3300"), "minbatt 3300 kept: %s", mb.c_str());
  const std::string st = w.cli(b, "fw pull-status");
  UCHECK(w[b].xfer->_phase == 1 || w[b].xfer->_phase == 2, "the pull resumes after the upgrade (phase %u): %s", w[b].xfer->_phase, st.c_str());
  w.run(8 * 3600000, [&]() { return slotFw(w, b) == 301; });
  UCHECK(slotFw(w, b) == 301, "the pull completed on the new firmware (slot %u): %s", slotFw(w, b), w.cli(b, "fw pull-status").c_str());
  UCHECK(w[b].app_fw == 300, "auto-apply off: not installed (app %u)", w[b].app_fw);
  w.run(60000);
  UCHECK(has(w.cli(b, "fw slot"), "fw-301"), "B serves fw-301: %s", w.cli(b, "fw slot").c_str());
  printf("  B had %u chunks before the upgrade\n", have0);
  printf("C99 upgrade: %d checks passed, %d failed\n", g_ok, g_bad);
  return g_bad ? 1 : 0;
}
