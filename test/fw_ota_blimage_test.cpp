// Host test for fw_ota/BlImage.h against REAL bootloader images.
//   g++ -std=c++11 -I src/helpers/fw_ota test/fw_ota_blimage_test.cpp -o /tmp/blimg && /tmp/blimg <new_with_descriptor.bin> <old_without_descriptor.bin>
// The .bin files are the served image of a bootloader build: python3 <otafix>/tools/hexbin.py <build>.hex <out>.bin  (0xF4000 .. last programmed byte)
#include "BlImage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
using namespace fw_ota;
typedef std::vector<uint8_t> Img;

static void rd(void* ctx, uint32_t off, void* dst, uint32_t n) { memcpy(dst, &(*(Img*)ctx)[off], n); }
static Img load(const char* p) {
  FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
  Img v; uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n); fclose(f); return v;
}
static BlCheck chk(Img& i, const char* board = "RAK3401", uint32_t bl = BL_ADDR, uint32_t mbr = BL_MBR_PARAMS, BlDesc* d = nullptr) {
  char b[8] = {0}; strncpy(b, board, 8);
  return blImageCheck(rd, &i, (uint32_t)i.size(), b, bl, mbr, d);
}
static void put(Img& i, uint32_t off, const void* p, size_t n) { memcpy(&i[off], p, n); }

// synthetic image with valid vectors and one descriptor at `at`
static Img synth(uint32_t len, uint32_t at, const char* board = "RAK3401") {
  Img i(len, 0xFF);
  uint32_t vec[2] = { 0x20010000u, BL_ADDR + 0x101 }; put(i, 0, vec, 8);
  BlDesc d; memset(&d, 0, sizeof(d)); d.magic = BL_MAGIC; d.desc_version = BL_DESC_VER; strncpy(d.board, board, 8);
  d.bl_rev = 7; d.bl_addr = BL_ADDR; d.mbr_params = BL_MBR_PARAMS; put(i, at, &d, sizeof(d));
  return i;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s new.bin old.bin\n", argv[0]); return 2; }
  int n = 0;

  // ---- real images ----
  Img nw = load(argv[1]), old = load(argv[2]);
  BlDesc d; assert(chk(nw, "RAK3401", BL_ADDR, BL_MBR_PARAMS, &d) == BL_OK);
  assert(d.bl_rev >= 1 && strncmp(d.board, "RAK3401", 8) == 0 && d.bl_addr == BL_ADDR && d.mbr_params == BL_MBR_PARAMS);
  printf("real new image: %zu B, rev %lu, board %.8s\n", nw.size(), (unsigned long)d.bl_rev, d.board); n++;
  assert(chk(old) == BL_NO_DESC); n++;                                               // the pre-descriptor bootloader cannot be served / accepted
  assert(chk(nw, "HELTEC") == BL_WRONG_BOARD); n++;
  { BlCheck c = chk(nw, "RAK3401", 0xEA000, BL_MBR_PARAMS); assert(c == BL_BAD_VECTORS || c == BL_BAD_LAYOUT); n++; }   // node with another bootloader address
  assert(chk(nw, "RAK3401", BL_ADDR, 0xFF000) == BL_BAD_LAYOUT); n++;
  Img t;
  t = nw; t.push_back(0); assert(chk(t) == BL_BAD_SIZE); n++;                  // not a multiple of 4
  t = nw; t.resize(BL_MAX_SIZE + 4, 0xFF); assert(chk(t) == BL_BAD_SIZE); n++;                            // larger than the region
  t = nw; t.resize(0x80); assert(chk(t) == BL_BAD_SIZE); n++;
  t = nw; t[3] = 0x00; assert(chk(t) == BL_BAD_VECTORS); n++;                                           // initial SP no longer in RAM
  t = nw; { uint32_t rv = 0xFC000; put(t, 4, &rv, 4); } assert(chk(t) == BL_BAD_VECTORS); n++;            // reset vector without Thumb bit
  t = nw; { uint32_t rv = 0x26001; put(t, 4, &rv, 4); } assert(chk(t) == BL_BAD_VECTORS); n++;            // reset vector inside the application, not the bootloader
  t = nw; { uint32_t rv = BL_ADDR + (uint32_t)nw.size() + 1; put(t, 4, &rv, 4); } assert(chk(t) == BL_BAD_VECTORS); n++;   // beyond the end of the image

  // every single flipped bit of the descriptor's identity is refused or changes the result (magic, version, board, layout)
  { size_t at = 0; for (size_t i = 0; i + 4 <= nw.size(); i += 4) { uint32_t w; memcpy(&w, &nw[i], 4); if (w == BL_MAGIC) at = i; }
    assert(at);
    for (int bit = 0; bit < 32 * 8; bit++) {
      t = nw; t[at + bit / 8] ^= (uint8_t)(1u << (bit % 8));
      size_t byte = bit / 8;
      BlCheck c = chk(t);
      if (byte < 4 || byte == 4 || byte == 5) assert(c == BL_NO_DESC);            // magic / desc_version
      else if (byte >= 8 && byte < 16) assert(c == BL_WRONG_BOARD);               // board
      else if (byte >= 20 && byte < 28) assert(c == BL_BAD_LAYOUT);               // bl_addr / mbr_params
      else assert(c == BL_OK);                                                    // flags, bl_rev, reserved: informational
      n++;
    } }
  // a second copy of the descriptor is refused
  t = nw; t.resize(nw.size() + 32 > BL_MAX_SIZE ? nw.size() : nw.size() + 32, 0xFF);
  { size_t at = 0; for (size_t i = 0; i + 4 <= nw.size(); i += 4) { uint32_t w; memcpy(&w, &nw[i], 4); if (w == BL_MAGIC) at = i; }
    Img u = nw; u.resize(nw.size() + 32); memcpy(&u[nw.size()], &nw[at], 32); assert(chk(u) == BL_DESC_AMBIGUOUS); n++; }

  // ---- scanner boundaries: the descriptor at EVERY aligned position of images of many lengths is found exactly once ----
  for (uint32_t len = 0x104; len <= 1200; len += 4) {
    for (uint32_t at = 8; at + 32 <= len; at += 4) {
      Img s = synth(len, at); BlDesc dd;
      assert(chk(s, "RAK3401", BL_ADDR, BL_MBR_PARAMS, &dd) == BL_OK && dd.bl_rev == 7);
    }
    n++;
  }
  // unaligned magic is not a descriptor
  { Img s = synth(0x400, 0x200); Img u(s); memmove(&u[0x203 - 0], &s[0x200], 32); memset(&u[0x200], 0xFF, 3); assert(chk(u) == BL_NO_DESC); n++; }
  // the magic alone (e.g. inside a literal pool) does not count
  { Img s = synth(0x400, 0x200); memset(&s[0x200 + 4], 0, 28); assert(chk(s) == BL_NO_DESC); n++; }      // magic + zeros: version 0, not a descriptor

  // ---- served length: last programmed byte, word aligned ----
  { Img b(BL_MAX_SIZE, 0xFF); assert(blImageLength(rd, &b) == 0); n++;
    b[0] = 1; assert(blImageLength(rd, &b) == 4); n++;
    b[4] = 0; assert(blImageLength(rd, &b) == 8); n++;                              // a real 0x00 byte counts
    b[BL_MAX_SIZE - 1] = 0x12; assert(blImageLength(rd, &b) == BL_MAX_SIZE); n++;
    b[BL_MAX_SIZE - 1] = 0xFF; b[38999] = 0x34; assert(blImageLength(rd, &b) == 39000); n++;
    b[38999] = 0xFF; b[38996] = 0x34; assert(blImageLength(rd, &b) == 39000); n++;
    b[38996] = 0xFF; b[38993] = 0x34; assert(blImageLength(rd, &b) == 38996); n++; }
  // a real image padded with 0xFF to the region size has the same length as the image
  { Img p = nw; p.resize(BL_MAX_SIZE, 0xFF); uint32_t l = blImageLength(rd, &p); assert(l <= nw.size() && l + 4 >= nw.size() - 0x100); n++; }   // trailing 0xFF of the image itself are not served

  printf("BlImage host test: %d checks passed\n", n);
  return 0;
}
