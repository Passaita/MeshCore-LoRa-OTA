#!/bin/bash
# Host test of the BLE slot fill (`fw pull-ble`, v1.17.28 + bootloader rev 5): the REAL FwOtaTransfer::slotFromLota() / bleFilled() are
# extracted from src/helpers/fw_ota/FwOtaTransfer.cpp and run on a simulated flash into which a real "slot zip" image (tools/lota_slot_zip.py)
# is written from the slot start - exactly what bootloader rev 5 does. Checks: a good upload becomes a VALID slot and is marked BLE-filled
# (served without being installed); every kind of bad upload (header CRC, board, kind, size, compression, truncated payload, SHA-256)
# leaves the slot EMPTY and says why; no LOTA header = nothing happens. SHA-256 is the real rweather code (test/crypto).
# usage: test/fw_ota_slotfill_test.sh <some .lota app file>
set -e
cd "$(dirname "$0")/.."
LOTA=${1:?usage: $0 <LoRa-OTA-RAK3401-app-....lota from a release>}
W=$(mktemp -d)
python3 tools/lota_slot_zip.py "$LOTA" "$W/slot.zip" > /dev/null
(cd "$W" && unzip -q slot.zip && mv slot.bin image.bin)
python3 - "$W/t.cpp" <<'PY'
import sys
src = open("src/helpers/fw_ota/FwOtaTransfer.cpp").read()
def fn(sig):
    i = src.index(sig); j = src.index("{", i); d = 0
    for k in range(j, len(src)):
        if src[k] == "{": d += 1
        elif src[k] == "}":
            d -= 1
            if d == 0: return src[i:k + 1]
HEAD = r"""#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <vector>
#include <string>
#include <SHA256.h>
static const uint32_t SLOT_ADDR = 0x96000, SLOT_SIZE = 0xEC000 - 0x96000, FLASH_PAGE_SIZE = 0x1000, SLOT_PAYLOAD_OFFSET = 0x1000;
static const uint32_t SLOT_PAYLOAD_MAX = SLOT_SIZE - FLASH_PAGE_SIZE, APP_MAX_SIZE = 0x70000, SLOT_MAGIC = 0x4F544C46;
static const uint8_t KIND_APP = 0; static const uint16_t COMP_LZMA = 2; static const uint32_t LOTA_HDR = 160;
static const char* BLEFILL_FILE = "/fwota.blefill";
static std::vector<uint8_t> FL(SLOT_SIZE, 0xFF);                       // the slot's flash
static inline uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static void yield() {}
struct { void print(const char*) {} void println(const char*) {} } Serial;
struct { uint8_t board[8]; } g_fwpull_tag = {{'R','A','K','3','4','0','1',0}};
static void flashRead(void* d, uint32_t a, uint32_t n) { memcpy(d, &FL[a - SLOT_ADDR], n); }
static uint32_t crc32_flash(uint32_t a, size_t n) { uint32_t c = 0xFFFFFFFF; for (size_t i = 0; i < n; i++) { c ^= FL[a - SLOT_ADDR + i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); } return ~c; }
struct SlotHeader { uint32_t magic, payload_len, raw_len, payload_crc32; uint16_t compression, hdr_version; uint32_t fw_version, raw_crc32; uint8_t lzma_props[5], reserved[3]; };
static bool eraseSlotHeader() { memset(&FL[0], 0xFF, FLASH_PAGE_SIZE); return true; }
static bool slotValid() { SlotHeader h; memcpy(&h, &FL[0], sizeof h); return h.magic == SLOT_MAGIC && h.payload_len && h.payload_len <= SLOT_PAYLOAD_MAX && crc32_flash(SLOT_ADDR + SLOT_PAYLOAD_OFFSET, h.payload_len) == h.payload_crc32; }
struct SlotWriter { bool finishSparse(SlotHeader h, uint32_t pl, uint32_t pc) { h.magic = SLOT_MAGIC; h.hdr_version = 1; h.payload_len = pl; h.payload_crc32 = pc;
  memset(&FL[0], 0xFF, FLASH_PAGE_SIZE); memcpy(&FL[0], &h, sizeof h); return true; } };
struct File { std::string* s = nullptr; size_t pos = 0; explicit operator bool() const { return s != nullptr; }
  int read(uint8_t* b, size_t n) { size_t k = umin(n, s->size() - pos); memcpy(b, s->data() + pos, k); pos += k; return (int)k; }
  size_t write(const uint8_t* b, size_t n) { s->append((const char*)b, n); return n; } void close() {} };
static const int FILE_O_WRITE = 1;
struct FS { std::string files[1]; bool has = false;
  bool exists(const char*) { return has; } void remove(const char*) { has = false; files[0].clear(); }
  File open(const char*, int = 0) { has = true; File f; f.s = &files[0]; return f; } } g_fs;
class FwOtaTransfer { public:
  FS* _fs = &g_fs; SlotWriter _writer; char _last_err[48] = {0};
  void slotFromLota(); bool bleFilled(const SlotHeader& h) const;
};
"""
TAIL = r"""
static std::vector<uint8_t> IMG;
static void load(const std::vector<uint8_t>& img) { std::fill(FL.begin(), FL.end(), 0xFF); memcpy(&FL[0], img.data(), img.size()); g_fs.remove(""); }
static void fixhdr(std::vector<uint8_t>& im) { uint32_t o = 0x1000 - 160, c = 0xFFFFFFFF; for (int i = 0; i < 156; i++) { c ^= im[o + i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); } c = ~c; memcpy(&im[o + 156], &c, 4); }
int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); IMG.resize(n); fread(IMG.data(), 1, n, f); fclose(f);
  setvbuf(stdout, NULL, _IONBF, 0); int checks = 0; const uint32_t H = 0x1000 - 160;
  // 1) the real slot image -> VALID slot, marked BLE-filled, message
  { load(IMG); FwOtaTransfer t; t.slotFromLota(); assert(slotValid()); checks++;
    SlotHeader h; memcpy(&h, &FL[0], sizeof h); assert(t.bleFilled(h) && strstr(t._last_err, "in the slot, served")); checks++;
    uint32_t fv; memcpy(&fv, &IMG[H + 16], 4); assert(h.fw_version == fv && h.compression == COMP_LZMA && h.lzma_props[0] == 0x5D); checks++;
    FwOtaTransfer t2; t2.slotFromLota(); assert(slotValid()); checks++;                      // a second boot: no LOTA header any more, slot stays
    printf("  good upload: %s (fw-%u, %u B payload)\n", t._last_err, h.fw_version, h.payload_len); }
  // 2) bad uploads -> slot EMPTY, reason given, never BLE-filled
  struct Case { const char* name; int off; uint8_t val; bool fix; const char* why; } cs[] = {
    {"header CRC", 16, 0x01, false, "header CRC"}, {"format version", 4, 2, true, "format version"}, {"bootloader kind", 6, 1, true, "not an app"},
    {"other board", 8, 'X', true, "another board"}, {"huge payload", 23, 0x7F, true, "size"}, {"compression", 36, 2, true, "compression"},
    {"dictionary", 46, 0x40, true, "compression"}, {"payload byte", 0x1000 + 1000, 0x5A, false, "payload CRC"} };
  for (auto& c : cs) { std::vector<uint8_t> im = IMG; int o = c.off < 0x1000 ? (int)H + c.off : c.off; im[o] ^= c.val; if (c.fix) fixhdr(im);
    load(im); FwOtaTransfer t; t.slotFromLota();
    SlotHeader h; memcpy(&h, &FL[0], sizeof h);
    if (slotValid() || !strstr(t._last_err, c.why) || FL[H] != 0xFF) { printf("case %s: valid=%d msg='%s'\n", c.name, slotValid(), t._last_err); assert(0); }
    checks++; printf("  %-15s -> %s\n", c.name, t._last_err); }
  // 3) an upload cut off halfway (the rest of the slot still erased) -> payload CRC
  { std::vector<uint8_t> im(IMG.begin(), IMG.begin() + IMG.size() / 2); load(im); FwOtaTransfer t; t.slotFromLota(); assert(!slotValid() && strstr(t._last_err, "payload CRC")); checks++; }
  // 4) SHA-256 mismatch with a consistent CRC: change the header's SHA and fix the header CRC
  { std::vector<uint8_t> im = IMG; im[H + 52] ^= 1; fixhdr(im); load(im); FwOtaTransfer t; t.slotFromLota(); assert(!slotValid() && strstr(t._last_err, "SHA-256")); checks++; }
  // 5) no LOTA header (a normal slot, or empty flash) -> nothing touched
  { std::fill(FL.begin(), FL.end(), 0xFF); FL[0] = 0x46; FwOtaTransfer t; t.slotFromLota(); assert(FL[0] == 0x46 && !t._last_err[0]); checks++; }
  printf("BLE slot fill (slotFromLota) checks: %d passed\n", checks); return 0;
}
"""
body = fn("static uint32_t le32(") + "\n" + fn("bool FwOtaTransfer::bleFilled(") + "\n" + fn("void FwOtaTransfer::slotFromLota()")
open(sys.argv[1], "w").write(HEAD + body + TAIL)
PY
CR=test/crypto
g++ -std=c++17 -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=all -I$CR -o "$W/t" "$W/t.cpp" $CR/SHA256.cpp $CR/Hash.cpp $CR/Crypto.cpp
"$W/t" "$W/image.bin"
rm -rf "$W"
