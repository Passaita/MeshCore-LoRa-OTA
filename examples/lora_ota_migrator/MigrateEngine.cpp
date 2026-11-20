// LoRa-OTA migrator engine - see MigrateEngine.h.
#include "MigrateEngine.h"
#include <string.h>

namespace mig {

bool Engine::mutNoOtaFirstR1 = false, Engine::mutNoFallback = false, Engine::mutNoMark = false, Engine::mutNoProtect = false;

// ---------------------------------------------------------------------------------------------------------------------------------
// CRC-32 (same convention as esp_rom_crc32_le and fw_ota: ~seed in, ~ out) and SHA-256
static const uint32_t kCrcNib[16] = {
  0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};
uint32_t crc32(const void* data, size_t len, uint32_t seed) {
  uint32_t crc = ~seed;
  const uint8_t* p = (const uint8_t*)data;
  while (len--) { crc ^= *p++; crc = (crc >> 4) ^ kCrcNib[crc & 0x0F]; crc = (crc >> 4) ^ kCrcNib[crc & 0x0F]; }
  return ~crc;
}

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
  0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
  0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
  0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
  0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void shaBlock(uint32_t h[8], const uint8_t* b) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
  }
  h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
void Sha256::init() {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h, H0, sizeof(h)); n = 0; used = 0;
}
void Sha256::add(const uint8_t* d, size_t len) {
  n += len;
  while (len) {
    size_t k = 64 - used; if (k > len) k = len;
    memcpy(buf + used, d, k); used += (uint32_t)k; d += k; len -= k;
    if (used == 64) { shaBlock(h, buf); used = 0; }
  }
}
void Sha256::done(uint8_t out[32]) {
  const uint64_t bits = n * 8;
  uint8_t pad = 0x80; add(&pad, 1); pad = 0;
  while (used != 56) add(&pad, 1);
  uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  add(l, 8);
  for (int i = 0; i < 8; i++) { out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16; out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = h[i]; }
}
void sha256(const uint8_t* data, size_t len, uint8_t out[32]) { Sha256 s; s.init(); s.add(data, len); s.done(out); }

// ---------------------------------------------------------------------------------------------------------------------------------
// partition table
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int parseTable(const uint8_t* t, uint32_t len, Part* out, int max) {
  int n = 0;
  for (uint32_t i = 0; i + 32 <= len; i += 32) {
    const uint8_t* e = t + i;
    if (e[0] == 0xAA && e[1] == 0x50) {
      if (n >= max) return -1;
      Part& p = out[n++];
      p.type = e[2]; p.subtype = e[3]; p.offset = le32(e + 4); p.size = le32(e + 8);
      memcpy(p.name, e + 12, 16); p.name[16] = 0;
    } else if (e[0] == 0xEB && e[1] == 0xEB) {
      continue;                                  // MD5 entry (checked by the bootloader; a table we wrote is compared byte by byte)
    } else if (e[0] == 0xFF && e[1] == 0xFF) {
      return n;
    } else {
      return -1;
    }
  }
  return n;
}

struct Want { uint8_t type, subtype; uint32_t offset, size; };
static const Want kStock[] = {{1, 0x02, 0x9000, 0x5000}, {1, 0x00, 0xE000, 0x2000}, {0, 0x10, 0x10000, 0x640000},
                              {0, 0x11, 0x650000, 0x640000}, {1, 0x82, 0xC90000, 0x360000}, {1, 0x03, 0xFF0000, 0x10000}};
static const Want kNew[] = {{1, 0x02, 0x9000, 0x5000}, {1, 0x00, 0xE000, 0x2000}, {0, 0x10, 0x10000, 0x300000},
                            {0, 0x11, 0x310000, 0x300000}, {1, 0x40, 0x610000, 0x200000}, {0, 0x00, 0x810000, 0x200000},
                            {1, 0x82, 0xC90000, 0x360000}, {1, 0x03, 0xFF0000, 0x10000}};
static bool same(const Part* p, int n, const Want* w, int wn) {
  if (n != wn) return false;
  for (int i = 0; i < n; i++)
    if (p[i].type != w[i].type || p[i].subtype != w[i].subtype || p[i].offset != w[i].offset || p[i].size != w[i].size) return false;
  return true;
}

TableKind classifyTable(Flash& f) {
  uint8_t t[TABLE_LEN];
  if (!f.read(TABLE_ADDR, t, sizeof(t))) return TABLE_OTHER;
  Part p[16];
  const int n = parseTable(t, sizeof(t), p, 16);
  if (n < 0) return TABLE_OTHER;
  if (same(p, n, kStock, 6)) return TABLE_STOCK;
  if (same(p, n, kNew, 8)) return TABLE_NEW;     // also a table whose MD5 page was cut off: the bootloader accepts it, leave it alone
  if (n > 0 && n < 8 && same(p, n, kNew, n)) return TABLE_PARTIAL;   // a cut inside our own table write: the migrator writes it again
  return TABLE_OTHER;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// ESP32-S3 app image (esp_image_header_t + segments + checksum + appended SHA-256)
// The LoRa-OTA tag (FwTag: 'FWPL', version 1, "HELTECV4"), kept XOR 0x5A: the plain 16 bytes must not appear in the migrator's own
// image, or the migrator would pass for a tagged LoRa-OTA app (found by the host test with the real build, 2026-11-19).
static const uint8_t kTagX[16] = {0x1c, 0x0d, 0x0a, 0x16, 0x5b, 0x5a, 0x5a, 0x5a, 0x12, 0x1f, 0x16, 0x0e, 0x1f, 0x19, 0x0c, 0x6e};

Image checkImage(Flash& f, uint32_t addr, uint32_t maxLen) {
  Image r{false, 0, false, ""};
  uint8_t kTag[16]; for (int i = 0; i < 16; i++) kTag[i] = kTagX[i] ^ 0x5A;
  uint8_t h[24];
  if (!f.read(addr, h, sizeof(h))) { r.why = "read"; return r; }
  if (h[0] != 0xE9) { r.why = "no app image"; return r; }
  const uint8_t segs = h[1];
  if (segs == 0 || segs > 16) { r.why = "bad segment count"; return r; }
  if ((h[12] | h[13] << 8) != 9) { r.why = "not an ESP32-S3 image"; return r; }
  if (h[23] != 1) { r.why = "no appended SHA-256"; return r; }
  Sha256 sh; sh.init(); sh.add(h, sizeof(h));
  uint8_t ck = 0xEF, buf[1024], carry[15]; uint32_t carried = 0;
  uint32_t pos = 24;
  for (int s = 0; s < segs; s++) {
    uint8_t sh8[8];
    if (pos + 8 > maxLen || !f.read(addr + pos, sh8, 8)) { r.why = "truncated"; return r; }
    sh.add(sh8, 8); pos += 8;
    uint32_t len = le32(sh8 + 4);
    if (len > maxLen || pos + len > maxLen) { r.why = "segment too long"; return r; }
    while (len) {
      uint32_t k = len < sizeof(buf) ? len : (uint32_t)sizeof(buf);
      if (!f.read(addr + pos, buf, k)) { r.why = "read"; return r; }
      sh.add(buf, k);
      for (uint32_t i = 0; i < k; i++) ck ^= buf[i];
      if (!r.tag) {                              // the 16-byte tag may straddle two blocks
        uint8_t w[sizeof(buf) + sizeof(carry)];
        memcpy(w, carry, carried); memcpy(w + carried, buf, k);
        const uint32_t wl = carried + k;
        for (uint32_t i = 0; i + 16 <= wl; i++) if (memcmp(w + i, kTag, 16) == 0) { r.tag = true; break; }
        carried = wl < 15 ? wl : 15; memcpy(carry, w + wl - carried, carried);
      }
      pos += k; len -= k;
    }
  }
  const uint32_t ckOff = ((pos + 16) & ~15u) - 1;
  if (ckOff + 1 + 32 > maxLen) { r.why = "truncated"; return r; }
  uint8_t tail[16 + 32];
  const uint32_t padLen = ckOff + 1 - pos;
  if (!f.read(addr + pos, tail, padLen + 32)) { r.why = "read"; return r; }
  if (tail[padLen - 1] != ck) { r.why = "checksum"; return r; }
  sh.add(tail, padLen);
  uint8_t dig[32]; sh.done(dig);
  if (memcmp(dig, tail + padLen, 32) != 0) { r.why = "SHA-256"; return r; }
  r.ok = true; r.len = ckOff + 1 + 32; r.why = "ok";
  return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// otadata (two sectors at 0xE000 / 0xF000; ESP-IDF 4.4 with app rollback)
uint32_t otaCrc(uint32_t seq) { uint8_t b[4] = {(uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24)}; return crc32(b, 4, 0xFFFFFFFFu); }
bool otaValid(const OtaSel& s) {
  return s.seq != 0xFFFFFFFFu && s.state != OTA_INVALID && s.state != OTA_ABORTED && s.crc == otaCrc(s.seq);
}
int otaActive(const OtaSel s[2], int* sector) {
  const bool v0 = otaValid(s[0]), v1 = otaValid(s[1]);
  int k = -1;
  if (v0 && v1) k = s[1].seq > s[0].seq ? 1 : 0; else if (v0) k = 0; else if (v1) k = 1;
  if (sector) *sector = k;
  return k < 0 ? -1 : (int)((s[k].seq - 1) % 2);
}

bool Engine::readOta(OtaSel s[2]) {
  return f_.read(OTADATA_ADDR, &s[0], sizeof(OtaSel)) && f_.read(OTADATA_ADDR + SECTOR, &s[1], sizeof(OtaSel));
}

bool Engine::writeSel(int sector, uint32_t seq, uint32_t state) {
  OtaSel e; memset(&e, 0xFF, sizeof(e));
  e.seq = seq; e.state = state; e.crc = otaCrc(seq);
  const uint32_t at = OTADATA_ADDR + (uint32_t)sector * SECTOR;
  if (!f_.erase(at, SECTOR) || !f_.write(at, &e, sizeof(e))) return false;
  OtaSel b; return f_.read(at, &b, sizeof(b)) && memcmp(&b, &e, sizeof(e)) == 0;
}

static uint32_t nextSeq(uint32_t after, int index) { uint32_t s = after + 1; while ((s - 1) % 2 != (uint32_t)index) s++; return s; }

// Two entries: the main app (ota_1) NEW and below it the fallback (ota_0 = stock MeshCore in R1, the migrator in R0) VALID. The first
// write goes over the older sector (the current choice stays until it is complete), the second over the sector of the current choice.
// Which comes first depends on the table, so that a power cut between or inside them starts something that finishes the migration:
//   stock table (R1): main entry first - ota_1 is still the migrator, which resumes; the fallback then goes below it.
//   LoRa-OTA table (R0, also a retry after a failed start): fallback first - ota_0 is the migrator; a main entry without a fallback
//   below it must never be the only valid one (a failing app would leave nothing but the factory seeder).
// When the main app fails its first start the bootloader marks its entry ABORTED and boots the VALID fallback.
bool Engine::writeOtadata() {
  OtaSel s[2]; if (!readOta(s)) return fail("otadata read");
  int cur = -1; otaActive(s, &cur);
  const uint32_t maxSeq = cur < 0 ? 0 : s[cur].seq;
  const int older = cur < 0 ? 0 : 1 - cur, newer = 1 - older;
  const uint32_t fSeq = nextSeq(maxSeq, 0), mSeq = nextSeq(fSeq, 1);
  if (mutNoFallback) return writeSel(older, mSeq, OTA_NEW) || fail("otadata write (main)");
  if (kind_ == TABLE_STOCK) {
    if (!writeSel(older, mSeq, OTA_NEW)) return fail("otadata write (main)");
    if (!writeSel(newer, fSeq, OTA_VALID)) return fail("otadata write (fallback)");
  } else {
    if (!writeSel(older, fSeq, OTA_VALID)) return fail("otadata write (fallback)");
    if (!writeSel(newer, mSeq, OTA_NEW)) return fail("otadata write (main)");
  }
  return true;
}

// Only the entries and the MD5 entry are programmed (the rest of the sector is 0xFF in the blob and after the erase): the window
// without a valid table is the sector erase plus two 256-byte page programs.
static uint32_t tableUsedLen(const uint8_t* t) {
  uint32_t i = 0;
  while (i + 32 <= TABLE_LEN && !(t[i] == 0xFF && t[i + 1] == 0xFF)) i += 32;
  return i;
}

bool Engine::writeTableChecked() {
  uint8_t b[TABLE_LEN];
  const uint32_t used = tableUsedLen(table_);
  for (uint32_t i = used; i < TABLE_LEN; i++) if (table_[i] != 0xFF) return fail("table blob: data after its end");
  for (int tries = 0; tries < 3; tries++) {                 // still running: a bad read-back is repaired here, not by USB
    if (!f_.writeTable(table_, used)) continue;
    if (f_.read(TABLE_ADDR, b, TABLE_LEN) && memcmp(b, table_, TABLE_LEN) == 0) return true;
  }
  return fail("partition table write did not verify");
}

// ---------------------------------------------------------------------------------------------------------------------------------
bool Engine::overlapsProtected(uint32_t a, uint32_t len) const {
  if (mutNoProtect) return false;
  const uint64_t e = (uint64_t)a + len;
  auto hit = [&](uint32_t s, uint32_t l) { return l && a < (uint64_t)s + l && e > s; };
  return hit(0, 0x10000) || hit(run_, runLen_) || hit(stockAddr_, stockLen_) || hit(DATA_START, 0x1000000 - DATA_START);
}

bool Engine::plan() {
  kind_ = classifyTable(f_);
  runLen_ = stockAddr_ = stockLen_ = 0;
  if (kind_ == TABLE_OTHER) return fail("unknown partition table (not stock MeshCore, not LoRa-OTA): nothing done");
  if (kind_ == TABLE_STOCK) {
    if (run_ != STOCK_APP0 && run_ != STOCK_APP1) return fail("running from an unexpected address");
    Image me = checkImage(f_, run_, STOCK_APP_SIZE);
    if (!me.ok) return fail("cannot read my own image");
    runLen_ = me.len;
    const uint32_t other = run_ == STOCK_APP0 ? STOCK_APP1 : STOCK_APP0;
    Image st = checkImage(f_, other, STOCK_APP_SIZE);
    if (st.ok) { stockAddr_ = other; stockLen_ = st.len; }
  } else {                                                   // LoRa-OTA table (or our cut-off one): only the R0 migrator in app0 runs here
    if (run_ == MAIN_ADDR) return fail("already on the LoRa-OTA layout: install the LoRa-OTA app with the update page directly");
    if (run_ != STOCK_APP0) return fail("running from an unexpected address");
    Image me = checkImage(f_, run_, APP_SIZE);
    if (!me.ok) return fail("cannot read my own image");
    runLen_ = me.len;
  }
  // the ranges the migration writes besides the images: the slot header (intent record) and the slot's control sectors
  if (overlapsProtected(MARK_ADDR, SECTOR) || overlapsProtected(CTRL_ADDR, 2 * SECTOR))
    return fail(run_ == STOCK_APP1 ? "the migrator is too big (must end below 0x80E000)" : "stock MeshCore is too big to keep");
  return true;
}

bool Engine::eraseAhead(uint32_t base, uint32_t limit, uint32_t& done, uint32_t to) {
  to = (to + SECTOR - 1) & ~(SECTOR - 1);
  if (to > limit) return false;
  while (done < to) { if (!f_.erase(base + done, SECTOR)) return false; done += SECTOR; }
  return true;
}

bool Engine::uploadBegin() {
  upOff_ = mainErased_ = seedErased_ = 0; upBad_ = false; memset(&hdr_, 0, sizeof(hdr_));
  if (!runLen_) return fail("no plan");
  if (!f_.erase(MARK_ADDR, SECTOR)) return fail("flash erase");        // an old intent record must never meet half-new files
  return true;
}

bool Engine::uploadData(uint32_t off, const uint8_t* d, uint32_t len) {
  if (upBad_) return false;
  if (off != upOff_) { upBad_ = true; return fail("upload out of order"); }
  while (len) {
    if (upOff_ < sizeof(BundleHdr)) {
      uint32_t k = sizeof(BundleHdr) - upOff_; if (k > len) k = len;
      memcpy(hdrBuf_ + upOff_, d, k); upOff_ += k; d += k; len -= k;
      if (upOff_ == sizeof(BundleHdr)) {
        memcpy(&hdr_, hdrBuf_, sizeof(hdr_));
        const char* w = nullptr;
        if (memcmp(hdr_.magic, BUNDLE_MAGIC, 8) != 0) w = "not a LoRa-OTA migration bundle";
        else if (hdr_.crc != crc32(hdrBuf_, sizeof(BundleHdr) - 4)) w = "bundle header damaged";
        else if (!hdr_.main_len || hdr_.main_len > APP_SIZE) w = "main app size";
        else if (!hdr_.seeder_len || hdr_.seeder_len > SEEDER_SIZE) w = "seeder size";
        else if (overlapsProtected(MAIN_ADDR, hdr_.main_len)) w = "the main app would overwrite a running/kept app";
        else if (overlapsProtected(SEEDER_ADDR, hdr_.seeder_len)) w = "the seeder would overwrite a running/kept app";
        if (w) { upBad_ = true; return fail(w); }
      }
      continue;
    }
    const uint32_t p = upOff_ - sizeof(BundleHdr);
    uint32_t base, rel, room, *erased, limit;
    if (p < hdr_.main_len) { base = MAIN_ADDR; rel = p; room = hdr_.main_len - p; erased = &mainErased_; limit = APP_SIZE; }
    else { base = SEEDER_ADDR; rel = p - hdr_.main_len; room = hdr_.main_len + hdr_.seeder_len - p; erased = &seedErased_; limit = SEEDER_SIZE; }
    if (!room) { upBad_ = true; return fail("longer than the bundle header says"); }
    uint32_t k = len < room ? len : room;
    if (!eraseAhead(base, limit, *erased, rel + k) || !f_.write(base + rel, d, k)) { upBad_ = true; return fail("flash write"); }
    upOff_ += k; d += k; len -= k;
  }
  return true;
}

bool Engine::uploadEnd() {
  if (upBad_) return false;
  if (upOff_ != sizeof(BundleHdr) + hdr_.main_len + hdr_.seeder_len) return fail("upload incomplete");
  return filesReady();
}

bool Engine::filesReady() {
  Image m = checkImage(f_, MAIN_ADDR, APP_SIZE);
  if (!m.ok) return fail("main app: not a valid image");
  if (!m.tag) return fail("main app: no HELTECV4 LoRa-OTA tag");
  Image s = checkImage(f_, SEEDER_ADDR, SEEDER_SIZE);
  if (!s.ok) return fail("seeder: not a valid image");
  if (overlapsProtected(MAIN_ADDR, m.len) || overlapsProtected(SEEDER_ADDR, s.len)) return fail("images overlap a kept app");
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The intent record: 32-byte records appended in the slot header sector (erased once, when a commit starts); the last valid one counts.
// A retry count is a new record, so a cut while counting never loses the record itself.
static bool markOk(const Mark& m, uint32_t run) {
  return m.magic == MARK_MAGIC && m.version == 1 && m.crc == crc32(&m, sizeof(m) - 4) && m.run_addr == run;
}
bool Engine::readMark(Mark& out) {
  bool found = false;
  for (uint32_t o = 0; o + sizeof(Mark) <= SECTOR; o += sizeof(Mark)) {
    Mark m; if (!f_.read(MARK_ADDR + o, &m, sizeof(m))) return false;
    if (markOk(m, run_)) { out = m; found = true; }
    else if (m.magic == 0xFFFFFFFFu && m.crc == 0xFFFFFFFFu) break;   // end of the records (a cut-off record is skipped)
  }
  return found;
}

bool Engine::writeMark(uint32_t tries, bool fresh) {
  Mark m; memset(&m, 0, sizeof(m));
  m.magic = MARK_MAGIC; m.version = 1; m.run_addr = run_; m.main_len = hdr_.main_len; m.seeder_len = hdr_.seeder_len;
  m.reserved[0] = tries; m.crc = crc32(&m, sizeof(m) - 4);
  uint32_t at = 0;
  if (fresh) { if (!f_.erase(MARK_ADDR, SECTOR)) return false; }
  else {
    for (;; at += sizeof(Mark)) {
      if (at + sizeof(Mark) > SECTOR) return false;
      Mark e; if (!f_.read(MARK_ADDR + at, &e, sizeof(e))) return false;
      const uint8_t* b = (const uint8_t*)&e; bool empty = true;
      for (size_t i = 0; i < sizeof(e); i++) if (b[i] != 0xFF) { empty = false; break; }
      if (empty) break;
    }
  }
  if (!f_.write(MARK_ADDR + at, &m, sizeof(m))) return false;
  Mark b; return readMark(b) && b.reserved[0] == tries;
}

// The LoRa-OTA app finds an empty slot: header sector (= the intent record, which it reads as "no slot") and both control copies
bool Engine::prepareSlot() { return f_.erase(CTRL_ADDR, SECTOR) && f_.erase(CTRL_ADDR + SECTOR, SECTOR); }

bool Engine::commit() {
  if (!runLen_) return fail("no plan");
  if (!filesReady()) return false;
  Mark old; const bool had = readMark(old);
  if (!prepareSlot()) return fail("flash erase (slot)");
  if (!mutNoMark && !had && !writeMark(0, true)) return fail("intent record");
  if (kind_ == TABLE_NEW) return writeOtadata();             // R0, the table is already ours (interrupted, or a retry after a rollback)
  if (kind_ == TABLE_PARTIAL) return writeTableChecked() && (kind_ = TABLE_NEW, writeOtadata());
  const bool otaFirst = run_ == STOCK_APP1 && !mutNoOtaFirstR1;   // R1: ota_1 is the migrator now and the main app after the table
  if (otaFirst) return writeOtadata() && writeTableChecked();
  if (!writeTableChecked()) return false;                     // R0: under the new table ota_0 is still the migrator
  kind_ = TABLE_NEW;                                          // the otadata order follows the table now on the flash
  return writeOtadata();
}

int Engine::resume() {
  Mark m;
  if (!readMark(m)) return 0;
  if (!filesReady()) return 0;
  if (kind_ == TABLE_NEW || kind_ == TABLE_PARTIAL) {
    OtaSel s[2]; if (!readOta(s)) return 0;
    bool aborted = false;
    for (int i = 0; i < 2; i++)
      if (s[i].seq != 0xFFFFFFFFu && s[i].crc == otaCrc(s[i].seq) && (s[i].seq - 1) % 2 == 1 &&
          (s[i].state == OTA_ABORTED || s[i].state == OTA_INVALID)) aborted = true;
    if (aborted) {                                             // the main app failed its first start (or lost power on probation)
      if (m.reserved[0] >= (uint32_t)AUTO_RETRIES) return -1;
      if (!writeMark(m.reserved[0] + 1, false)) return -1;
      hdr_.main_len = m.main_len; hdr_.seeder_len = m.seeder_len;
      return writeOtadata() ? 1 : -1;
    }
  }
  hdr_.main_len = m.main_len; hdr_.seeder_len = m.seeder_len;
  return commit() ? 1 : 0;
}

bool Engine::abortToStock() {
  if (kind_ != TABLE_STOCK || !stockAddr_) return fail("no stock firmware to return to");
  OtaSel s[2]; if (!readOta(s)) return fail("otadata read");
  int cur = -1; otaActive(s, &cur);
  const int idx = stockAddr_ == STOCK_APP0 ? 0 : 1;
  const uint32_t maxSeq = cur < 0 ? 0 : s[cur].seq;
  return writeSel(cur < 0 ? 0 : 1 - cur, nextSeq(maxSeq, idx), OTA_VALID) || fail("otadata write");
}

}  // namespace mig
