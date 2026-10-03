#pragma once
// Which chunks of the image are already in the slot. Pure logic (no flash, no radio) so it is unit-tested on the host
// (test/fw_ota_chunkmap_test.cpp). The puller keeps one of these: any valid chunk of its image is programmed straight to its place in
// the slot, whoever asked for it, so chunks may arrive in any order and from other pullers' requests ("overhear").
#include <stdint.h>
#include <string.h>

namespace fw_ota {

struct ChunkMap {
  static const uint16_t MAX_CHUNKS = 2176;          // SLOT_PAYLOAD_MAX (348,160 B) / 160 B chunks
  uint8_t  bits[MAX_CHUNKS / 8];
  uint16_t n = 0;                                   // chunks in the image
  uint16_t have = 0;                                // chunks present

  void reset(uint16_t total) { n = total; have = 0; memset(bits, 0, sizeof(bits)); }
  bool has(uint16_t i) const { return (bits[i >> 3] >> (i & 7)) & 1; }
  bool set(uint16_t i) {                            // false if out of range or already present
    if (i >= n || has(i)) return false;
    bits[i >> 3] |= (uint8_t)(1u << (i & 7)); have++;
    return true;
  }
  bool complete() const { return n && have == n; }
  uint16_t firstMissing(uint16_t from = 0) const { while (from < n && has(from)) from++; return from; }
  // bit i set = chunk (start + i) is still missing; only the first 8 chunks of a window fit the wire format
  uint8_t missingMask(uint16_t start, uint8_t count) const {
    uint8_t m = 0;
    for (uint8_t i = 0; i < count && i < 8 && (uint32_t)start + i < n; i++) if (!has(start + i)) m |= (uint8_t)(1u << i);
    return m;
  }
  template <class F> void rebuild(F present) {      // after a reboot: `present(i)` says whether chunk i's flash area is programmed
    have = 0; memset(bits, 0, sizeof(bits));
    for (uint16_t i = 0; i < n; i++) if (present(i)) set(i);
  }
};

}  // namespace fw_ota
