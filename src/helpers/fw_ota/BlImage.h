#pragma once
// A bootloader image as it travels over LoRa (bootloader-over-LoRa): the bytes of the sender's own bootloader flash, from BL_ADDR to
// the last programmed byte. Pure logic (no flash, no radio), unit-tested on the host against the real bootloader builds
// (test/fw_ota_blimage_test.cpp). The bootloader carries a 32-byte identity descriptor (fw_ota_bl_desc_t in its fw_ota.h - keep in sync);
// everything a receiver needs to know before it copies the image over its own bootloader is checked here.
#include <stdint.h>
#include <string.h>

namespace fw_ota {

static const uint32_t BL_ADDR         = 0xF4000;             // bootloader region (UICR.NRFFW[0])
static const uint32_t BL_MBR_PARAMS   = 0xFE000;             // MBR params page  (UICR.NRFFW[1]) - also the end of the bootloader region
static const uint32_t BL_MAX_SIZE     = BL_MBR_PARAMS - BL_ADDR;   // 40,960 B
static const uint32_t BL_MAGIC        = 0x4C424F4C;          // bytes 'L','O','B','L'
static const uint16_t BL_DESC_VER     = 1;

struct BlDesc {
  uint32_t magic;
  uint16_t desc_version;
  uint16_t flags;
  char     board[8];
  uint32_t bl_rev;        // monotonic bootloader revision
  uint32_t bl_addr;       // where the image was built to run
  uint32_t mbr_params;    // the MBR params page it was built for
  uint32_t reserved;
};  // 32 bytes
static_assert(sizeof(BlDesc) == 32, "BlDesc must match the bootloader's fw_ota_bl_desc_t");

enum BlCheck : uint8_t {
  BL_OK = 0,
  BL_BAD_SIZE,        // empty, not a multiple of 4, or larger than the bootloader region
  BL_BAD_VECTORS,     // initial SP not in RAM, or reset vector not a Thumb address inside the bootloader region
  BL_NO_DESC,         // no identity descriptor (a bootloader older than LoRa-OTA bootloader-update support, or not a bootloader)
  BL_DESC_AMBIGUOUS,  // more than one valid-looking descriptor
  BL_BAD_LAYOUT,      // image was built for another bootloader / MBR-params address than this node's UICR says
  BL_WRONG_BOARD,     // board id differs from this node's
};

static inline const char* blCheckText(BlCheck c) {
  switch (c) {
    case BL_OK:             return "ok";
    case BL_BAD_SIZE:       return "bad size";
    case BL_BAD_VECTORS:    return "bad vector table";
    case BL_NO_DESC:        return "no identity descriptor";
    case BL_DESC_AMBIGUOUS: return "ambiguous identity descriptor";
    case BL_BAD_LAYOUT:     return "built for another flash layout";
    case BL_WRONG_BOARD:    return "other board";
  }
  return "?";
}

// Reads `n` bytes at image offset `off` (flash on the node, a buffer in tests).
typedef void (*BlReadFn)(void* ctx, uint32_t off, void* dst, uint32_t n);

// Finds the identity descriptor (magic + version, 4-byte aligned) in an image of `len` bytes. Returns BL_OK with *out filled, BL_NO_DESC or
// BL_DESC_AMBIGUOUS. Streams in 256-byte blocks that advance by 224 (= 256 - 32), so a descriptor never straddles two blocks unseen.
static inline BlCheck blFindDesc(BlReadFn rd, void* ctx, uint32_t len, BlDesc* out) {
  uint8_t buf[256]; uint32_t found = 0;
  for (uint32_t off = 0; off + sizeof(BlDesc) <= len; off += 224) {
    uint32_t n = len - off < sizeof(buf) ? len - off : (uint32_t)sizeof(buf);
    bool tail = (n == len - off);
    rd(ctx, off, buf, n);
    uint32_t last = tail ? n - sizeof(BlDesc) : 220;               // candidate positions of this block (the tail block takes them all)
    for (uint32_t i = 0; i <= last; i += 4) {
      BlDesc d; memcpy(&d, buf + i, sizeof(d));
      if (d.magic == BL_MAGIC && d.desc_version == BL_DESC_VER) { if (!found++) *out = d; }
    }
    if (tail) break;
  }
  return found == 1 ? BL_OK : found ? BL_DESC_AMBIGUOUS : BL_NO_DESC;
}

// Everything a receiver checks before it lets the MBR copy the image over its own bootloader.
//   own_board: this node's board id (8 bytes, NUL padded) - from its own firmware tag
//   uicr_bl / uicr_mbr: this node's UICR.NRFFW[0] / [1] (where ITS bootloader and MBR params page really are)
static inline BlCheck blImageCheck(BlReadFn rd, void* ctx, uint32_t len, const char own_board[8], uint32_t uicr_bl, uint32_t uicr_mbr,
                                   BlDesc* desc_out = nullptr) {
  if (len < 0x100 || (len & 3) || len > BL_MAX_SIZE) return BL_BAD_SIZE;
  uint32_t vec[2]; rd(ctx, 0, vec, sizeof(vec));
  if (vec[0] < 0x20000000u || vec[0] > 0x20040000u || (vec[0] & 3)) return BL_BAD_VECTORS;                    // initial SP inside RAM
  if (!(vec[1] & 1) || (vec[1] & ~1u) < uicr_bl || (vec[1] & ~1u) >= uicr_bl + len) return BL_BAD_VECTORS;   // reset handler (Thumb) inside the image
  BlDesc d; BlCheck c = blFindDesc(rd, ctx, len, &d);
  if (c != BL_OK) return c;
  if (d.bl_addr != uicr_bl || d.mbr_params != uicr_mbr || uicr_bl != BL_ADDR || uicr_mbr != BL_MBR_PARAMS) return BL_BAD_LAYOUT;
  if (memcmp(d.board, own_board, sizeof(d.board)) != 0) return BL_WRONG_BOARD;
  if (desc_out) *desc_out = d;
  return BL_OK;
}

// Length of the image a node serves from its own bootloader flash: up to the last programmed (non-0xFF) byte, rounded up to a word.
// `rd` reads from BL_ADDR. 0 if the region is blank.
static inline uint32_t blImageLength(BlReadFn rd, void* ctx) {
  uint8_t buf[256];
  for (uint32_t end = BL_MAX_SIZE; end > 0; ) {
    uint32_t n = end < sizeof(buf) ? end : (uint32_t)sizeof(buf);
    rd(ctx, end - n, buf, n);
    for (uint32_t i = n; i > 0; i--) if (buf[i - 1] != 0xFF) return (end - n + i + 3) & ~3u;
    end -= n;
  }
  return 0;
}

}  // namespace fw_ota
