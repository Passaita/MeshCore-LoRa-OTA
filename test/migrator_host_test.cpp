// Host test of the LoRa-OTA migrator engine (examples/lora_ota_migrator/MigrateEngine.*), run on a build host.
// A 16 MB RAM flash (NOR: erase -> 0xFF, program = AND), an ESP-IDF 4.4 bootloader model (partition table + MD5 entry, otadata with
// app rollback: NEW -> PENDING_VERIFY -> ABORTED, fallback order, factory), the apps' behaviour at boot (stock and the migrator mark
// themselves valid, the main app confirms on probation or fails), and a power cut at EVERY flash operation of the migration.
// Pass: the migration ends in the LoRa-OTA app on probation; after any cut the node reaches LoRa-OTA, stock MeshCore or the migrator
// page by itself - "needs USB" only for cuts inside the partition-table sector's erase + page programs (the reported window);
// identity/settings (nvs, spiffs) never change. Mutants (engine switches) must each be caught.
// Usage: migrator_host_test <new_table.bin> <stock_table.bin> [--images stock.bin main.bin seeder.bin] [--otadata-dump flash.bin]
#include "../examples/lora_ota_migrator/MigrateEngine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace mig;
static std::vector<uint8_t> g_new, g_stock;

// ---------------------------------------------------------------------------------------------------------------------------------
// MD5 (the bootloader checks the table's MD5 entry)
struct Md5 {
  uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
  void block(const uint8_t* m) {
    static const uint32_t s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t K[64]; for (int i = 0; i < 64; i++) { double v = __builtin_fabs(__builtin_sin((double)(i + 1))); K[i] = (uint32_t)(v * 4294967296.0); }
    uint32_t M[16]; for (int i = 0; i < 16; i++) M[i] = m[4 * i] | m[4 * i + 1] << 8 | m[4 * i + 2] << 16 | (uint32_t)m[4 * i + 3] << 24;
    uint32_t A = a0, B = b0, C = c0, D = d0;
    for (int i = 0; i < 64; i++) {
      uint32_t F; int g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; } else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; } else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      F = F + A + K[i] + M[g]; A = D; D = C; C = B; B = B + rol(F, s[i]);
    }
    a0 += A; b0 += B; c0 += C; d0 += D;
  }
  void run(const uint8_t* d, size_t n, uint8_t out[16]) {
    std::vector<uint8_t> v(d, d + n); v.push_back(0x80); while (v.size() % 64 != 56) v.push_back(0);
    uint64_t bits = (uint64_t)n * 8; for (int i = 0; i < 8; i++) v.push_back((uint8_t)(bits >> (8 * i)));
    for (size_t i = 0; i < v.size(); i += 64) block(&v[i]);
    uint32_t r[4] = {a0, b0, c0, d0}; for (int i = 0; i < 16; i++) out[i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
  }
};

// ---------------------------------------------------------------------------------------------------------------------------------
struct Cut {};
// Bench 2026-11-19 (bench Heltec): ESP-IDF's default flash chip refuses (abort()) any erase/write overlapping the WHOLE running partition
// as the table at 0x8000 describes it (stock app1 = 0x650000-0xC90000 holds the seeder target 0x810000; stock app0 holds the R0 main
// target 0x310000). The board driver must go around that guard (chip copy without region_protected). g_idfGuard models a driver
// that does not: the mutant 'driver through the default chip' sets it and the migration must then fail.
static bool g_idfGuard = false;
struct RamFlash : Flash {
  uint32_t runAddr = 0;                                       // the running app's start (for the IDF guard model)
  bool idfRefuses(uint32_t a, uint32_t l) const {
    if (!g_idfGuard || !runAddr) return false;
    for (uint32_t i = 0; i < TABLE_LEN; i += 32) {
      const uint8_t* e = &m[TABLE_ADDR + i];
      if (e[0] != 0xAA || e[1] != 0x50) break;
      uint32_t off, sz; memcpy(&off, e + 4, 4); memcpy(&sz, e + 8, 4);
      if (e[2] == 0 && off == runAddr) return a < off + sz && a + l > off;
    }
    return false;
  }
  std::vector<uint8_t> m = std::vector<uint8_t>(0x1000000, 0xFF);
  long ops = 0, cutAt = -1; bool counting = false;
  long tableFirstOp = -1, tableLastOp = -1, critOp = -1;     // critOp: the engine's first otadata or table change
  uint32_t rng = 12345;
  uint8_t rnd() { rng = rng * 1103515245 + 12345; return (uint8_t)(rng >> 16); }
  bool read(uint32_t a, void* d, uint32_t l) override { if ((uint64_t)a + l > m.size()) return false; memcpy(d, &m[a], l); return true; }
  void doErase(uint32_t a) {
    if (counting) { ops++; if (ops == cutAt) { for (uint32_t i = 0; i < SECTOR; i++) m[a + i] = rnd() | (i & 1 ? 0xF0 : 0); throw Cut(); } }
    memset(&m[a], 0xFF, SECTOR);
  }
  void doProg(uint32_t a, const uint8_t* s, uint32_t l) {      // one page program
    if (counting) { ops++; if (ops == cutAt) { for (uint32_t i = 0; i < l / 2; i++) m[a + i] &= s[i]; throw Cut(); } }
    for (uint32_t i = 0; i < l; i++) m[a + i] &= s[i];
  }
  bool erase(uint32_t a, uint32_t l) override {
    if (a < 0x9000 || (a & (SECTOR - 1)) || (l & (SECTOR - 1))) return false;   // the driver refuses bootloader + table
    if (idfRefuses(a, l)) return false;
    if (counting && critOp < 0 && (a == OTADATA_ADDR || a == OTADATA_ADDR + SECTOR)) critOp = ops + 1;
    for (uint32_t o = 0; o < l; o += SECTOR) doErase(a + o);
    return true;
  }
  void prog(uint32_t a, const uint8_t* s, uint32_t l) {
    while (l) { uint32_t k = 256 - (a & 255); if (k > l) k = l; doProg(a, s, k); a += k; s += k; l -= k; }
  }
  bool write(uint32_t a, const void* s, uint32_t l) override {
    if (a < 0x9000 || (uint64_t)a + l > m.size()) return false;
    if (idfRefuses(a, l)) return false;
    prog(a, (const uint8_t*)s, l); return true;
  }
  bool writeTable(const void* s, uint32_t l) override {
    tableFirstOp = ops + 1;
    if (counting && critOp < 0) critOp = ops + 1;
    doErase(TABLE_ADDR); prog(TABLE_ADDR, (const uint8_t*)s, l);
    tableLastOp = ops;
    return true;
  }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// images
static std::vector<uint8_t> makeImage(const char* name, uint32_t size, bool tag) {
  std::vector<uint8_t> v(24, 0);
  v[0] = 0xE9; v[1] = 2; v[2] = 2; v[3] = 0x4F; v[12] = 9; v[23] = 1;
  uint8_t ck = 0xEF;
  uint32_t lens[2] = {size / 3, size - size / 3 - 64};
  for (int s = 0; s < 2; s++) {
    uint32_t load = s ? 0x42000020 : 0x3C000020;
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(load >> (8 * i)));
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(lens[s] >> (8 * i)));
    size_t st = v.size();
    for (uint32_t i = 0; i < lens[s]; i++) v.push_back((uint8_t)(i * 7 + s + name[4]));
    if (s == 0) { char mk[16] = {0}; snprintf(mk, sizeof(mk), "IMG:%s", name); memcpy(&v[st], mk, 16); }
    if (s == 1 && tag) { static const uint8_t t[16] = {0x46, 0x57, 0x50, 0x4C, 1, 0, 0, 0, 'H', 'E', 'L', 'T', 'E', 'C', 'V', '4'}; memcpy(&v[st + 1001], t, 16); }
    for (size_t i = st; i < v.size(); i++) ck ^= v[i];
  }
  while ((v.size() + 1) % 16) v.push_back(0);
  v.push_back(ck);
  uint8_t d[32]; sha256(v.data(), v.size(), d); v.insert(v.end(), d, d + 32);
  return v;
}
static std::vector<uint8_t> IMG_STOCK, IMG_MIGR, IMG_MAIN, IMG_SEED;

static std::string imageAt(RamFlash& f, uint32_t a, uint32_t max) {
  Image r = checkImage(f, a, max);
  if (!r.ok) return "";
  char mk[17] = {0}; memcpy(mk, &f.m[a + 32], 16);
  return strncmp(mk, "IMG:", 4) == 0 ? std::string(mk + 4) : std::string("REAL");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// ESP-IDF 4.4 bootloader model (rollback enabled) + the apps' first moments
struct Boot { std::string img; uint32_t addr; uint32_t entryState; int sector; };

static void writeEntry(RamFlash& f, int sector, const OtaSel& e) {
  const uint32_t at = OTADATA_ADDR + sector * SECTOR; f.doErase(at); f.prog(at, (const uint8_t*)&e, sizeof(e));
}

static bool g_trace = false;
static void dumpOta(RamFlash& f, const char* w) {
  if (!g_trace) return;
  OtaSel s[2]; memcpy(&s[0], &f.m[OTADATA_ADDR], 32); memcpy(&s[1], &f.m[OTADATA_ADDR + SECTOR], 32);
  printf("    [%s] table %d | s0 seq %u st %u %s | s1 seq %u st %u %s\n", w, (int)classifyTable(f), s[0].seq, s[0].state, s[0].crc == otaCrc(s[0].seq) ? "ok" : "bad",
         s[1].seq, s[1].state, s[1].crc == otaCrc(s[1].seq) ? "ok" : "bad");
}
static bool bootloader(RamFlash& f, Boot& out) {
  dumpOta(f, "boot");
  uint8_t t[TABLE_LEN]; memcpy(t, &f.m[TABLE_ADDR], TABLE_LEN);
  Part p[16]; int n = 0;
  for (uint32_t i = 0; i + 32 <= TABLE_LEN; i += 32) {         // esp_partition_table_verify + load
    const uint8_t* e = t + i;
    if (e[0] == 0xAA && e[1] == 0x50) { Part& q = p[n++]; q.type = e[2]; q.subtype = e[3]; q.offset = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24; q.size = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24; if (n >= 16) return false; }
    else if (e[0] == 0xEB && e[1] == 0xEB) { uint8_t d[16]; Md5().run(t, i, d); if (memcmp(d, e + 16, 16) != 0) return false; }
    else if (e[0] == 0xFF && e[1] == 0xFF) break;
    else return false;
  }
  if (!n) return false;
  const Part *ota[2] = {nullptr, nullptr}, *fac = nullptr;
  for (int i = 0; i < n; i++) if (p[i].type == 0) { if (p[i].subtype == 0x10) ota[0] = &p[i]; else if (p[i].subtype == 0x11) ota[1] = &p[i]; else if (p[i].subtype == 0) fac = &p[i]; }
  OtaSel s[2]; memcpy(&s[0], &f.m[OTADATA_ADDR], 32); memcpy(&s[1], &f.m[OTADATA_ADDR + SECTOR], 32);
  int idx = -2, sec = -1;                                       // -1 = factory
  for (;;) {
    auto inval = [](const OtaSel& e) { return e.seq == 0xFFFFFFFFu || e.state == OTA_INVALID || e.state == OTA_ABORTED; };
    if (inval(s[0]) && inval(s[1])) { idx = fac ? -1 : 0; sec = -1; break; }
    idx = otaActive(s, &sec);
    if (idx < 0) { idx = fac ? -1 : 0; sec = -1; break; }
    if (s[sec].state == OTA_PENDING) { s[sec].state = OTA_ABORTED; writeEntry(f, sec, s[sec]); continue; }
    if (s[sec].state == OTA_NEW) { s[sec].state = OTA_PENDING; writeEntry(f, sec, s[sec]); }
    break;
  }
  auto tryLoad = [&](int i) -> bool {
    const Part* q = i < 0 ? fac : ota[i];
    if (!q) return false;
    std::string img = imageAt(f, q->offset, q->size);
    if (img.empty()) return false;
    out.img = img; out.addr = q->offset; out.sector = (i == idx) ? sec : -1;
    out.entryState = (i == idx && sec >= 0) ? s[sec].state : OTA_VALID;
    return true;
  };
  for (int i = idx; i >= -1; i--) if (tryLoad(i)) return true;
  for (int i = idx + 1; i < 2; i++) if (tryLoad(i)) return true;
  return false;
}

static void markValid(RamFlash& f, const Boot& b) {            // esp_ota_mark_app_valid_cancel_rollback()
  if (b.sector < 0 || b.entryState != OTA_PENDING) return;
  OtaSel e; memcpy(&e, &f.m[OTADATA_ADDR + b.sector * SECTOR], 32); e.state = OTA_VALID; writeEntry(f, b.sector, e);
}

enum Outcome { O_MAIN, O_MAIN_NOPROB, O_STOCK, O_PAGE, O_PAGE_ERR, O_FAILED, O_RESCUE, O_BRICK, O_LOOP };
static const char* oname[] = {"LoRa-OTA", "LoRa-OTA WITHOUT probation", "stock", "migrator page", "migrator error", "migrator: app failed",
                              "factory seeder (rescue)", "NEEDS USB", "boot loop"};

// boots until something stable runs. mainGood: the LoRa-OTA app passes probation.
static Outcome runNode(RamFlash& f, bool mainGood, std::string* why = nullptr) {
  for (int boots = 0; boots < 30; boots++) {
    Boot b;
    if (!bootloader(f, b)) return O_BRICK;
    if (g_trace) printf("    -> %s at 0x%x state %u\n", b.img.c_str(), b.addr, b.entryState);
    if (b.img == "STOCK" || b.img == "REAL") { markValid(f, b); return O_STOCK; }
    if (b.img == "SEED") return O_RESCUE;
    if (b.img == "MAIN") {
      if (b.entryState == OTA_PENDING) { if (mainGood) { markValid(f, b); return O_MAIN; } continue; }   // bad: resets on probation
      if (!mainGood) continue;
      return O_MAIN_NOPROB;
    }
    if (b.img == "MIGR") {
      markValid(f, b);
      f.runAddr = b.addr;
      Engine e(f, g_new.data(), b.addr);
      if (!e.plan()) { if (why) *why = e.why(); return O_PAGE_ERR; }
      const int r = e.resume();
      if (r == 1) continue;
      return r < 0 ? O_FAILED : O_PAGE;
    }
    return O_BRICK;
  }
  return O_LOOP;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// scenarios
static uint32_t g_dataHash;
static uint32_t dataHash(RamFlash& f) { return crc32(&f.m[0x9000], 0x5000) ^ crc32(&f.m[DATA_START], 0x1000000 - DATA_START, 7); }

static void put(RamFlash& f, uint32_t a, const std::vector<uint8_t>& v) { for (size_t i = 0; i < v.size(); i++) f.m[a + i] = v[i]; }
static void sel(RamFlash& f, int sector, uint32_t seq, uint32_t state) {
  OtaSel e; memset(&e, 0xFF, sizeof(e)); e.seq = seq; e.state = state; e.crc = otaCrc(seq); memcpy(&f.m[OTADATA_ADDR + sector * SECTOR], &e, 32);
}
// a node on stock MeshCore that just took the migrator through `start ota` (ElegantOTA -> esp_ota_set_boot_partition -> NEW)
static void setupStock(RamFlash& f, bool r1) {
  f.m.assign(0x1000000, 0xFF);
  put(f, TABLE_ADDR, g_stock);
  for (uint32_t i = 0x9000; i < 0xE000; i++) f.m[i] = (uint8_t)(i * 13);            // nvs
  for (uint32_t i = DATA_START; i < 0x1000000; i += 1) f.m[i] = (uint8_t)(i * 31 + 5);  // spiffs + coredump
  if (r1) { put(f, STOCK_APP0, IMG_STOCK); put(f, STOCK_APP1, IMG_MIGR); sel(f, 0, 2, OTA_NEW); }
  else { put(f, STOCK_APP1, IMG_STOCK); put(f, STOCK_APP0, IMG_MIGR); sel(f, 0, 2, OTA_VALID); sel(f, 1, 3, OTA_NEW); }
  g_dataHash = dataHash(f);
}
static std::vector<uint8_t> bundle() {
  BundleHdr h; memset(&h, 0, sizeof(h)); memcpy(h.magic, BUNDLE_MAGIC, 8);
  h.main_len = IMG_MAIN.size(); h.seeder_len = IMG_SEED.size(); h.fw = 330; h.crc = crc32(&h, sizeof(h) - 4);
  std::vector<uint8_t> v((uint8_t*)&h, (uint8_t*)&h + sizeof(h));
  v.insert(v.end(), IMG_MAIN.begin(), IMG_MAIN.end()); v.insert(v.end(), IMG_SEED.begin(), IMG_SEED.end());
  return v;
}
// the page: upload in 1436-byte pieces, then Migrate
static bool userUploadAndCommit(RamFlash& f, uint32_t runAddr, std::string& why, bool upload = true) {
  f.runAddr = runAddr;
  Engine e(f, g_new.data(), runAddr);
  if (!e.plan()) { why = e.why(); return false; }
  if (upload) {
    std::vector<uint8_t> b = bundle();
    if (!e.uploadBegin()) { why = e.why(); return false; }
    for (size_t o = 0; o < b.size(); o += 1436) { size_t k = std::min<size_t>(1436, b.size() - o); if (!e.uploadData(o, &b[o], k)) { why = e.why(); return false; } }
    if (!e.uploadEnd()) { why = e.why(); return false; }
  }
  if (!e.commit()) { why = e.why(); return false; }
  return true;
}

static int g_fail = 0;
static void check(bool ok, const char* what) { if (!ok) { g_fail++; printf("  FAIL: %s\n", what); } }

// One full sweep: no cut, then a cut at every flash operation from the upload to a stable node; with mainGood false/true.
static void sweep(bool r1, bool mainGood, int& window, bool quiet) {
  RamFlash f; std::string why;
  const uint32_t mig = r1 ? STOCK_APP1 : STOCK_APP0;
  setupStock(f, r1);
  check(runNode(f, true) == O_PAGE, "first start lands on the migrator page");
  // count the operations of a clean run
  f.counting = true; f.ops = 0;
  bool ok = userUploadAndCommit(f, mig, why);
  if (!ok) printf("  clean run refused: %s\n", why.c_str());
  check(ok, "clean upload + commit");
  Outcome o = runNode(f, mainGood);
  const long total = f.ops;
  if (mainGood) { check(o == O_MAIN, "clean run -> LoRa-OTA on probation"); check(classifyTable(f) == TABLE_NEW, "clean run: full LoRa-OTA table"); }
  else check(o == (r1 ? O_STOCK : O_FAILED), r1 ? "bad main app -> back to stock" : "bad main app -> migrator page after retries");
  check(dataHash(f) == g_dataHash, "nvs + MeshCore files unchanged (clean)");
  if (!quiet) printf("  %s %s: clean -> %s, %ld flash operations\n", r1 ? "R1" : "R0", mainGood ? "good app" : "bad app ", oname[o], total);
  window = 0;
  int counts[9] = {0};
  for (long k = 1; k <= total; k++) {
    setupStock(f, r1); runNode(f, true);
    f.counting = true; f.ops = 0; f.cutAt = k; f.tableFirstOp = f.tableLastOp = f.critOp = -1;
    try { userUploadAndCommit(f, mig, why); runNode(f, mainGood); } catch (Cut&) {}
    const bool inTable = f.tableFirstOp > 0 && k >= f.tableFirstOp && (f.tableLastOp < 0 || k <= f.tableLastOp);
    f.counting = false; f.cutAt = -1;
    Outcome a = runNode(f, mainGood, &why);
    counts[a]++;
    char msg[160];
    if (inTable && (a == O_BRICK || classifyTable(f) == TABLE_PARTIAL)) {   // the window: no complete table (needs USB)
      window++; continue;
    }
    if (a == O_BRICK) {
      if (inTable) window++;
      else { snprintf(msg, sizeof(msg), "cut at op %ld (outside the table write) needs USB", k); check(false, msg); }
      continue;
    }
    snprintf(msg, sizeof(msg), "cut at op %ld: nvs/spiffs changed", k); check(dataHash(f) == g_dataHash, msg);
    if (a == O_MAIN_NOPROB || a == O_LOOP || a == O_RESCUE || a == O_PAGE_ERR) {
      snprintf(msg, sizeof(msg), "cut at op %ld -> %s %s", k, oname[a], a == O_PAGE_ERR ? why.c_str() : ""); check(false, msg); continue;
    }
    if (a == O_PAGE && f.critOp > 0 && k >= f.critOp) {
      snprintf(msg, sizeof(msg), "cut at op %ld (after the first otadata/table change at %ld): the migration did not finish by itself", k, f.critOp);
      check(false, msg); continue;
    }
    // the user finishes: on the page press Migrate again (re-upload if needed); on stock run `start ota` + the migrator again
    if (a == O_PAGE || a == O_FAILED) {
      f.runAddr = mig;
      Engine e(f, g_new.data(), mig); e.plan();
      bool up = !e.filesReady();
      if (!userUploadAndCommit(f, mig, why, up)) { snprintf(msg, sizeof(msg), "cut at op %ld: retry refused: %s", k, why.c_str()); check(false, msg); continue; }
    } else if (a == O_STOCK && classifyTable(f) == TABLE_STOCK) {
      OtaSel s[2]; memcpy(&s[0], &f.m[OTADATA_ADDR], 32); memcpy(&s[1], &f.m[OTADATA_ADDR + SECTOR], 32);
      int sec; int cur = otaActive(s, &sec);
      if (cur < 0) cur = 0;                                     // no valid entry: the bootloader started ota_0
      const uint32_t other = cur == 0 ? STOCK_APP1 : STOCK_APP0;
      put(f, other, IMG_MIGR); OtaSel e; memset(&e, 0xFF, 32);
      uint32_t seq = (sec < 0 ? 0 : s[sec].seq) + 1; while ((seq - 1) % 2 != (uint32_t)(cur == 0 ? 1 : 0)) seq++;
      e.seq = seq; e.state = OTA_NEW; e.crc = otaCrc(seq); memcpy(&f.m[OTADATA_ADDR + (sec < 0 ? 0 : 1 - sec) * SECTOR], &e, 32);
      if (runNode(f, true) != O_PAGE) { snprintf(msg, sizeof(msg), "cut at op %ld: stock -> migrator again failed", k); check(false, msg); continue; }
      if (!userUploadAndCommit(f, other, why)) { snprintf(msg, sizeof(msg), "cut at op %ld: second migration refused: %s", k, why.c_str()); check(false, msg); continue; }
    } else if (a == O_STOCK) {
      // stock on the LoRa-OTA layout: a failed app (R1, expected), or a cut inside ESP-IDF's own otadata update (NEW -> PENDING, mark
      // valid) that dropped the main entry. The user installs the LoRa-OTA app with stock's own update page (ElegantOTA -> ota_1).
      if (!mainGood) continue;
      if (imageAt(f, MAIN_ADDR, APP_SIZE) != "MAIN") put(f, MAIN_ADDR, IMG_MAIN);
      OtaSel s[2]; memcpy(&s[0], &f.m[OTADATA_ADDR], 32); memcpy(&s[1], &f.m[OTADATA_ADDR + SECTOR], 32);
      int sec; otaActive(s, &sec);
      uint32_t seq = (sec < 0 ? 0 : s[sec].seq) + 1; while ((seq - 1) % 2 != 1) seq++;
      OtaSel e; memset(&e, 0xFF, 32); e.seq = seq; e.state = OTA_NEW; e.crc = otaCrc(seq);
      memcpy(&f.m[OTADATA_ADDR + (sec < 0 ? 0 : 1 - sec) * SECTOR], &e, 32);
    } else {                                                   // O_MAIN
      snprintf(msg, sizeof(msg), "cut at op %ld: LoRa-OTA runs without its full table", k); check(classifyTable(f) == TABLE_NEW, msg);
      continue;
    }
    Outcome fin = runNode(f, mainGood);
    const Outcome want = mainGood ? O_MAIN : (r1 ? O_STOCK : O_FAILED);
    snprintf(msg, sizeof(msg), "cut at op %ld: after the user's retry -> %s", k, oname[fin]); check(fin == want, msg);
    if (fin == O_MAIN) { snprintf(msg, sizeof(msg), "cut at op %ld: LoRa-OTA runs without its full table", k); check(classifyTable(f) == TABLE_NEW, msg); }
  }
  if (!quiet) {
    printf("    cuts: ");
    for (int i = 0; i < 9; i++) if (counts[i]) printf("%s %d, ", oname[i], counts[i]);
    printf("needs-USB window = %d operations (table erase + its page programs)\n", window);
  }
}

static std::vector<uint8_t> load(const char* p) {
  FILE* fp = fopen(p, "rb"); if (!fp) { printf("cannot open %s\n", p); exit(2); }
  std::vector<uint8_t> v; uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof(b), fp)) > 0) v.insert(v.end(), b, b + n); fclose(fp); return v;
}

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: %s new_table.bin stock_table.bin [--images stock main seeder] [--otadata-dump flash.bin]\n", argv[0]); return 2; }
  g_new = load(argv[1]); g_stock = load(argv[2]);
  g_new.resize(TABLE_LEN, 0xFF); g_stock.resize(TABLE_LEN, 0xFF);
  // small images for the sweeps (the logic does not depend on the size; the size limits have their own refusal cases below)
  IMG_STOCK = makeImage("STOCK", 48000, false);
  IMG_MIGR = makeImage("MIGR", 40000, false);
  IMG_MAIN = makeImage("MAIN", 30000, true);
  IMG_SEED = makeImage("SEED", 36000, false);
  bool sweeps = true;

  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--images") && i + 3 < argc) {         // real builds: the image checker must read them
      const char* nm[3] = {"stock", "main", "seeder"};
      for (int k = 0; k < 3; k++) {
        std::vector<uint8_t> v = load(argv[i + 1 + k]); RamFlash f; put(f, 0x10000, v);
        Image r = checkImage(f, 0x10000, 0x640000);
        printf("real %s image %s: %s, %u B (file %zu B), tag %s\n", nm[k], argv[i + 1 + k], r.why, r.len, v.size(), r.tag ? "yes" : "no");
        check(r.ok && r.len == v.size(), "real image accepted with its exact length");
        check(r.tag == (k == 1), "HELTECV4 tag only in the LoRa-OTA app");
      }
      i += 3;
    } else if (!strcmp(argv[i], "--real") && i + 4 < argc) {       // --real stock migrator main seeder: one clean migration, R1 and R0
      std::vector<uint8_t> st = load(argv[i + 1]), mg = load(argv[i + 2]), mn = load(argv[i + 3]), sd = load(argv[i + 4]);
      for (int r1 = 1; r1 >= 0; r1--) {
        RamFlash f; f.m.assign(0x1000000, 0xFF); put(f, TABLE_ADDR, g_stock);
        const uint32_t mig = r1 ? STOCK_APP1 : STOCK_APP0, stk = r1 ? STOCK_APP0 : STOCK_APP1;
        put(f, stk, st); put(f, mig, mg);
        if (r1) sel(f, 0, 2, OTA_VALID); else { sel(f, 0, 2, OTA_VALID); sel(f, 1, 3, OTA_VALID); }
        f.runAddr = mig;
        Engine e(f, g_new.data(), mig);
        bool ok = e.plan();
        printf("real %s: plan %s (migrator %u B, stock kept at 0x%x)\n", r1 ? "R1" : "R0", ok ? "ok" : e.why(), e.runLen(), e.stockAddr());
        check(ok && e.stockAddr() == stk, "real images: plan keeps stock");
        BundleHdr h; memset(&h, 0, sizeof(h)); memcpy(h.magic, BUNDLE_MAGIC, 8); h.main_len = mn.size(); h.seeder_len = sd.size(); h.crc = crc32(&h, sizeof(h) - 4);
        std::vector<uint8_t> b((uint8_t*)&h, (uint8_t*)&h + sizeof(h)); b.insert(b.end(), mn.begin(), mn.end()); b.insert(b.end(), sd.begin(), sd.end());
        ok = ok && e.uploadBegin();
        for (size_t o = 0; ok && o < b.size(); o += 1436) ok = e.uploadData(o, &b[o], std::min<size_t>(1436, b.size() - o));
        ok = ok && e.uploadEnd() && e.commit();
        if (!ok) printf("  refused: %s\n", e.why());
        check(ok, "real images: upload + commit");
        Boot bt; check(bootloader(f, bt) && bt.addr == MAIN_ADDR && bt.entryState == OTA_PENDING, "real images: the LoRa-OTA app boots on probation");
        check(memcmp(&f.m[MAIN_ADDR], mn.data(), mn.size()) == 0 && memcmp(&f.m[SEEDER_ADDR], sd.data(), sd.size()) == 0, "real images in place");
        check(r1 ? memcmp(&f.m[STOCK_APP0], st.data(), st.size()) == 0 : true, "real stock kept as the fallback (R1)");
        printf("  -> boots 0x%x state %u, table %s\n", bt.addr, bt.entryState, classifyTable(f) == TABLE_NEW ? "LoRa-OTA" : "?");
      }
      i += 4;
    } else if (!strcmp(argv[i], "--bundle") && i + 3 < argc) {     // --bundle stock migrator <-migrate.bin from tools/make_migrate_bundle.py>
      std::vector<uint8_t> st = load(argv[i + 1]), mg = load(argv[i + 2]), b = load(argv[i + 3]);
      RamFlash f; f.m.assign(0x1000000, 0xFF); put(f, TABLE_ADDR, g_stock); put(f, STOCK_APP0, st); put(f, STOCK_APP1, mg); sel(f, 0, 2, OTA_VALID);
      f.runAddr = STOCK_APP1;
      Engine e(f, g_new.data(), STOCK_APP1);
      bool ok = e.plan() && e.uploadBegin();
      for (size_t o = 0; ok && o < b.size(); o += 1436) ok = e.uploadData(o, &b[o], std::min<size_t>(1436, b.size() - o));
      ok = ok && e.uploadEnd() && e.commit();
      Boot bt; ok = ok && bootloader(f, bt) && bt.addr == MAIN_ADDR && bt.entryState == OTA_PENDING;
      printf("bundle file %s (fw-%u): %s\n", argv[i + 3], e.bundle().fw, ok ? "accepted, LoRa-OTA boots on probation" : e.why());
      check(ok, "tool-made bundle goes through the engine");
      i += 3;
    } else if (!strcmp(argv[i], "--no-sweep")) { sweeps = false;
    } else if (!strcmp(argv[i], "--trace") && i + 3 < argc) {      // --trace <r1 0|1> <good 0|1> <op>
      const bool r1 = atoi(argv[i + 1]), good = atoi(argv[i + 2]); const long k = atol(argv[i + 3]);
      RamFlash f; std::string why; setupStock(f, r1); runNode(f, true);
      f.counting = true; f.ops = 0; f.cutAt = k;
      try { userUploadAndCommit(f, r1 ? STOCK_APP1 : STOCK_APP0, why); g_trace = true; runNode(f, good); } catch (Cut&) { printf("cut at op %ld (table ops %ld..%ld)\n", k, f.tableFirstOp, f.tableLastOp); }
      g_trace = true; f.counting = false; f.cutAt = -1;
      Outcome a = runNode(f, good, &why); printf("outcome: %s %s\n", oname[a], why.c_str()); return 0;
    } else if (!strcmp(argv[i], "--otadata-dump") && i + 1 < argc) {   // the CRC convention against a real board's otadata
      std::vector<uint8_t> d = load(argv[++i]);
      for (int s = 0; s < 2; s++) {
        OtaSel e; memcpy(&e, &d[OTADATA_ADDR + s * SECTOR], 32);
        if (e.seq == 0xFFFFFFFFu) { printf("real otadata sector %d: empty\n", s); continue; }
        printf("real otadata sector %d: seq %u state %u crc %s\n", s, e.seq, e.state, e.crc == otaCrc(e.seq) ? "matches" : "DIFFERS");
        check(e.crc == otaCrc(e.seq), "otadata CRC convention matches a real board");
      }
    }
  }
  // the tables: what the engine and the bootloader model see
  { RamFlash f; put(f, TABLE_ADDR, g_new); check(classifyTable(f) == TABLE_NEW, "new table classified"); Boot b; put(f, 0x10000, IMG_MAIN); check(bootloader(f, b), "new table boots"); }
  { RamFlash f; put(f, TABLE_ADDR, g_stock); check(classifyTable(f) == TABLE_STOCK, "stock table classified"); }

  int w = 0, wmax = 0;
  if (!sweeps) { printf("== result: %s (%d failures, no sweeps)\n", g_fail ? "FAIL" : "PASS", g_fail); return g_fail != 0; }
  printf("== sweeps (power cut at every flash operation)\n");
  for (int r1 = 1; r1 >= 0; r1--) for (int good = 1; good >= 0; good--) { sweep(r1, good, w, false); if (w > wmax) wmax = w; }

  printf("== refusals\n");
  {  // R1 with a migrator too big to leave room for the seeder / control sectors
    RamFlash f; std::string why; setupStock(f, true);
    std::vector<uint8_t> big = makeImage("MIGR", 0x1C0000, false); put(f, STOCK_APP1, big);
    f.runAddr = STOCK_APP1;
    Engine e(f, g_new.data(), STOCK_APP1);
    check(!e.plan(), "R1: oversized migrator refused at plan"); printf("  oversized migrator: %s\n", e.why());
  }
  {  // a bundle with another board's app (no tag)
    RamFlash f; std::string why; setupStock(f, true);
    std::vector<uint8_t> keep = IMG_MAIN; IMG_MAIN = makeImage("MAIN", 660000, false);
    check(!userUploadAndCommit(f, STOCK_APP1, why), "app without the HELTECV4 tag refused"); printf("  untagged app: %s\n", why.c_str());
    check(memcmp(&f.m[TABLE_ADDR], g_stock.data(), TABLE_LEN) == 0, "table untouched after a refusal");
    IMG_MAIN = keep;
  }
  {  // an unknown partition table
    RamFlash f; setupStock(f, true); f.m[TABLE_ADDR + 32 * 2 + 8] ^= 0x01;      // app0 size changed
    f.runAddr = STOCK_APP1;
    Engine e(f, g_new.data(), STOCK_APP1); check(!e.plan(), "unknown table refused"); printf("  unknown table: %s\n", e.why());
  }
  const int realFails = g_fail;
  printf("== result: %s (%d failures), needs-USB window <= %d flash operations\n", realFails ? "FAIL" : "PASS", realFails, wmax);

  // mutants: each must make the test fail
  struct Mut { const char* name; bool* flag; } muts[] = {{"otadata after the table in R1", &Engine::mutNoOtaFirstR1},
      {"no fallback entry", &Engine::mutNoFallback}, {"no intent record", &Engine::mutNoMark}, {"no protection of the kept apps", &Engine::mutNoProtect},
      {"driver through the default chip (IDF running-partition guard)", &g_idfGuard}};
  int caught = 0;
  for (auto& m : muts) {
    g_fail = 0; *m.flag = true;
    for (int r1 = 1; r1 >= 0; r1--) for (int good = 1; good >= 0; good--) sweep(r1, good, w, true);
    { RamFlash f; setupStock(f, true); std::vector<uint8_t> big = makeImage("MIGR", 0x1C0000, false); put(f, STOCK_APP1, big);
      f.runAddr = STOCK_APP1;
      Engine e(f, g_new.data(), STOCK_APP1); check(!e.plan(), "mutant: oversized migrator"); }
    *m.flag = false;
    printf("mutant '%s': %s (%d failures)\n", m.name, g_fail ? "caught" : "NOT CAUGHT", g_fail);
    if (g_fail) caught++;
  }
  printf("== mutants caught %d/%zu\n", caught, sizeof(muts) / sizeof(muts[0]));
  return realFails || caught != (int)(sizeof(muts) / sizeof(muts[0]));
}
