// Host test for fw_ota::ChunkMap (g++ -std=c++11 -I src/helpers/fw_ota test/fw_ota_chunkmap_test.cpp && ./a.out)
#include "ChunkMap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
using fw_ota::ChunkMap;

int main() {
  // out-of-order fill (as with overheard chunks) always ends complete, duplicates and out-of-range are refused
  for (int trial = 0; trial < 200; trial++) {
    uint16_t n = 1 + rand() % 2176;
    ChunkMap m; m.reset(n);
    std::vector<uint16_t> order(n); for (uint16_t i = 0; i < n; i++) order[i] = i;
    std::random_shuffle(order.begin(), order.end());
    for (size_t k = 0; k < order.size(); k++) {
      assert(!m.complete());
      assert(m.set(order[k]));
      assert(!m.set(order[k]));                       // duplicate
      assert(m.have == k + 1);
      uint16_t fm = m.firstMissing();
      for (uint16_t i = 0; i < fm; i++) assert(m.has(i));
      if (fm < n) assert(!m.has(fm));
    }
    assert(m.complete() && m.firstMissing() == n);
    assert(!m.set(n) && !m.set(n + 5));               // out of range
  }
  // window mask: bit i set = start+i missing; never beyond the image end or 8 chunks
  ChunkMap m; m.reset(20);
  for (uint16_t i : {0, 1, 3, 4, 9}) m.set(i);
  assert(m.firstMissing() == 2);
  assert(m.missingMask(2, 8) == 0b00110101 ? false : true);   // (computed below, just documenting)
  uint8_t mk = m.missingMask(2, 8);                   // chunks 2..9: 2 missing,3 have,4 have,5,6,7,8 missing,9 have
  assert(mk == ((1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6)));
  assert(m.missingMask(18, 8) == 0b11);               // only chunks 18,19 exist
  assert(m.missingMask(0, 2) == 0);
  // rebuild after a "reboot" from a simulated flash: present iff the chunk area is not all 0xFF
  std::vector<uint8_t> flash(20 * 160, 0xFF);
  for (uint16_t i : {0, 1, 3, 4, 9}) memset(&flash[i * 160], 0x5A, 160);
  ChunkMap r; r.reset(20);
  r.rebuild([&](uint16_t i) { for (int b = 0; b < 160; b++) if (flash[i * 160 + b] != 0xFF) return true; return false; });
  assert(r.have == 5 && r.has(9) && !r.has(2) && r.firstMissing() == 2);
  puts("chunkmap host test OK");
  return 0;
}
