// (Heltec port) host test of the ESP32 install path: the REAL FwOtaInstallEsp32.cpp + FwOtaSlotsEsp32.cpp + LzmaDec against a mocked
// 16 MB flash (test/esp32mock). The rule (priority 2, never brick): at every moment the partition the bootloader would start holds a
// complete, valid image (old or new); a power cut at ANY flash operation of an install is followed by a correct finish.
#include "../src/helpers/fw_ota/FwOtaSlots.h"
#include "../src/helpers/fw_ota/FwOtaInstallEsp32.h"
#include "../src/helpers/fw_ota/Watchdog.h"
#include "esp_ota_ops.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
namespace fw_ota { Watchdog g_wdt; }
using namespace fw_ota;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static std::vector<uint8_t> g_raw_new, g_raw_old;
static std::vector<uint8_t> loadFile(const char* p) { FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); } std::vector<uint8_t> v; int c; while ((c = fgetc(f)) != EOF) v.push_back((uint8_t)c); fclose(f); return v; }

// the slot as slotFromLota() leaves it: header page + payload; COMP_NONE images (refusal cases) carry the raw image as their payload
static void seedSlot(const std::vector<uint8_t>& payload, const SlotHeader& h0) {
  memset(&mock::flash[SLOT_ADDR], 0xFF, SLOT_SIZE);
  SlotHeader h = h0; h.magic = SLOT_MAGIC; h.hdr_version = 1; h.payload_len = (uint32_t)payload.size(); h.payload_crc32 = crc32(payload.data(), payload.size());
  memcpy(&mock::flash[SLOT_ADDR], &h, sizeof h); memcpy(&mock::flash[SLOT_ADDR + SLOT_PAYLOAD_OFFSET], payload.data(), payload.size());
}
static void seedLota(const std::vector<uint8_t>& lota) {
  SlotHeader h{}; h.raw_len = le32(&lota[24]); h.raw_crc32 = le32(&lota[32]); h.compression = COMP_LZMA; h.fw_version = le32(&lota[16]);
  memcpy(h.lzma_props, &lota[44], 5);
  seedSlot(std::vector<uint8_t>(lota.begin() + 160, lota.end()), h);
}
static void seedRaw(const std::vector<uint8_t>& raw, uint32_t fw) {
  SlotHeader h{}; h.raw_len = (uint32_t)raw.size(); h.raw_crc32 = crc32(raw.data(), raw.size()); h.compression = COMP_NONE; h.fw_version = fw; seedSlot(raw, h);
}
static void fresh() {                     // app0 runs the old image, app1 holds garbage, blank control record
  std::fill(mock::flash.begin(), mock::flash.end(), 0xFF);
  memcpy(&mock::flash[0x10000], g_raw_old.data(), g_raw_old.size());
  for (uint32_t i = 0; i < 0x300000; i += 7) mock::flash[0x310000 + i] = (uint8_t)(i * 31);
  mock::running = mock::boot = 0; mock::last_invalid = -1; mock::run_state = ESP_OTA_IMG_VALID; mock::ops_left = -1; mock::ops = 0; mock::our_table = true;
  mock::boot_sets = 0;
}
// one boot: the bootloader starts `boot`; a newly selected image runs on probation. Returns 'R' restart, 'C' power cut, 'N' normal start
static char bootOnce() {
  const bool changed = mock::running != mock::boot; mock::running = mock::boot;
  if (changed) mock::run_state = ESP_OTA_IMG_PENDING_VERIFY;
  try { bootApplyEsp32(); return 'N'; } catch (Restart&) { return 'R'; } catch (PowerCut&) { return 'C'; }
}
static bool holds(int part, const std::vector<uint8_t>& img) { return !memcmp(&mock::flash[mock::parts[part].address], img.data(), img.size()); }
static CtrlBlock ctrl() { CtrlBlock c; readCtrl(c); return c; }

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: test <real HELTECV4 .lota>\n"); return 2; }
  std::vector<uint8_t> lota = loadFile(argv[1]);
  // the new image = what the .lota decodes to: let the installer produce it once, keep the result as the reference
  g_raw_old.assign(300000, 0); for (size_t i = 0; i < g_raw_old.size(); i++) g_raw_old[i] = (uint8_t)(i * 7 + 3);
  g_raw_old[0] = 0xE9; g_raw_old[12] = 9; g_raw_old[13] = 0; memcpy(&g_raw_old[1000], &g_fwpull_tag, sizeof(FwTag));
  // 1) a real install: PENDING -> unpack into app1 -> switch -> restart; the new app then starts, nothing more to do
  fresh(); seedLota(lota); CHECK(slotValid(), "the seeded slot is VALID"); ctrlMarkPending();
  char r = bootOnce();
  CHECK(r == 'R' && mock::boot == 1 && ctrl().apply_state == APPLY_DONE, "install: restart into app1, DONE (r=%c boot=%d state=%u last=%u)", r, mock::boot, ctrl().apply_state, ctrl().last_result);
  SlotHeader sh; readSlotHeader(sh);
  g_raw_new.assign(&mock::flash[0x310000], &mock::flash[0x310000] + sh.raw_len);
  CHECK(crc32(g_raw_new.data(), g_raw_new.size()) == sh.raw_crc32, "app1 holds exactly the slot's raw image (CRC %08X)", sh.raw_crc32);
  CHECK(holds(0, g_raw_old), "the old app is untouched");
  r = bootOnce();
  CHECK(r == 'N' && mock::running == 1 && onProbation(), "the new app starts on probation, no second install (r=%c)", r);
  probationTick(130000, 0); CHECK(onProbation(), "no packet yet: still on probation");
  probationTick(130000, 3); CHECK(!onProbation() && mock::run_state == ESP_OTA_IMG_VALID, "2 min + a packet: confirmed");
  // 2) a power cut at EVERY flash operation of the install: the started partition always holds a whole image; the next boots finish it
  fresh(); seedLota(lota); ctrlMarkPending(); bootOnce(); const long total = mock::ops;
  int cuts = 0;
  for (long n = 0; n <= total; n++) {
    fresh(); seedLota(lota); ctrlMarkPending(); mock::ops_left = n;
    r = bootOnce(); mock::ops_left = -1;
    if (r == 'C') cuts++;
    const bool ok_after_cut = holds(mock::boot, mock::boot == 0 ? g_raw_old : g_raw_new);
    CHECK(ok_after_cut, "cut at op %ld: the partition the bootloader starts (%d) holds a whole image", n, mock::boot);
    for (int k = 0; k < 4 && r != 'N'; k++) r = bootOnce();
    CHECK(mock::running == 1 && holds(1, g_raw_new) && ctrl().apply_state == APPLY_DONE, "cut at op %ld: finished on the next boots (running %d, state %u last %u)",
          n, mock::running, ctrl().apply_state, ctrl().last_result);
    if (fails > 5) break;
  }
  printf("  power cut at each of %ld flash operations (%d interrupted installs): all finished\n", total + 1, cuts);
  // 3) an install that is cut every time gives up after 8 tries (the old app keeps running)
  fresh(); seedLota(lota); ctrlMarkPending();
  for (int k = 0; k < 9; k++) { mock::ops_left = 40; r = bootOnce(); }
  mock::ops_left = -1; r = bootOnce();
  CHECK(ctrl().apply_state == APPLY_FAILED && ctrl().last_result == 12 && mock::boot == 0 && holds(0, g_raw_old), "8 interrupted tries: FAILED 12, the old app stays (state %u last %u)", ctrl().apply_state, ctrl().last_result);
  // 4) refused images: nothing switched, the old app keeps running
  struct Case { const char* what; uint32_t code; } ;
  auto refuse = [&](const char* what, std::vector<uint8_t> img, uint32_t want) {
    fresh(); seedRaw(img, 400); ctrlMarkPending(); char rr = bootOnce();
    CHECK(rr == 'N' && mock::boot == 0 && mock::boot_sets == 0 && ctrl().apply_state == APPLY_FAILED && ctrl().last_result == want,
          "%s: refused with %u, no switch (r=%c boot=%d state=%u last=%u)", what, want, rr, mock::boot, ctrl().apply_state, ctrl().last_result);
  };
  std::vector<uint8_t> img = g_raw_old; img[1000 + 8 + 7] = '5';   refuse("another board's tag (HELTECV5)", img, 11);
  img = g_raw_old; img[0] = 0x00;                                    refuse("not an ESP32 image", img, 8);
  img = g_raw_old; img[12] = 2;                                      refuse("an image for another ESP32 chip", img, 8);
  { fresh(); SlotHeader h{}; h.raw_len = (uint32_t)g_raw_old.size(); h.raw_crc32 = crc32(g_raw_old.data(), g_raw_old.size()) ^ 1;   // an intact
    h.compression = COMP_NONE; h.fw_version = 402; seedSlot(g_raw_old, h); ctrlMarkPending(); bootOnce();                            //   slot whose
    CHECK(mock::boot == 0 && mock::boot_sets == 0 && ctrl().last_result == 5, "the unpacked image is not the one the header names: refused 5 (last %u)", ctrl().last_result); }   // result differs
  fresh(); seedLota(lota); mock::flash[SLOT_ADDR + SLOT_PAYLOAD_OFFSET + 5000] ^= 1; ctrlMarkPending(); r = bootOnce();
  CHECK(mock::boot == 0 && ctrl().apply_state == APPLY_FAILED && ctrl().last_result == 1, "a damaged slot payload: not VALID, refused (last %u)", ctrl().last_result);
  // 5) rollback seen by the old app: DONE + an invalid partition + the slot image is not what runs -> FAILED 13; a NEW image is not blocked
  fresh(); seedLota(lota); ctrlMarkPending(); bootOnce(); mock::boot = 0; mock::last_invalid = 1; r = bootOnce();
  CHECK(r == 'N' && ctrl().apply_state == APPLY_FAILED && ctrl().last_result == 13, "rollback detected: FAILED 13 (state %u last %u)", ctrl().apply_state, ctrl().last_result);
  { CtrlBlock c = ctrl(); SlotHeader h; readSlotHeader(h); CHECK(failedForSlot(c, h), "the record names this image");
    img = g_raw_old; img[2000] ^= 0x55; seedRaw(img, 401); readSlotHeader(h); CHECK(!failedForSlot(c, h), "another image in the slot is not blocked by it"); }
  // 6) the slot partition guard
  fresh();
  CHECK(!flashErase(0x10000, 0x1000) && !flashWrite(0x310000, "x", 1) && !flashErase(0xC90000, 0x1000), "never an erase/write outside the slot partition");
  CHECK(flashErase(SLOT_ADDR, 0x1000), "the slot partition itself is writable");
  CHECK(!flashErase(SLOT_ADDR + SLOT_PART_SIZE - 0x1000, 0x2000), "not past its end");
  printf("esp32 install: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
