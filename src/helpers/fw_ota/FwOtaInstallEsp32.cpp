// (Heltec port, 2026-11-01) What the RAK3401's LoRa-OTA bootloader does, done by the app on the ESP32-S3 (no custom bootloader):
//   bootApplyEsp32()  - at the start of a boot, before the mesh runs: the control record says PENDING -> unpack the verified slot (LZMA)
//                       into the OTHER app partition, check it (ESP32-S3 image header, CRC-32 of the result, the HELTECV4 tag inside it,
//                       the image's own hash via esp_ota_set_boot_partition), switch, restart. The running app is never touched: a
//                       failure or a power cut leaves it booting. Same counters as the bootloader: 8 interrupted tries -> FAILED.
//   probationTick()   - the new app starts "pending verify" (ESP-IDF rollback, enabled in the prebuilt bootloader). It confirms
//                       itself after 2 min of running loop AND one received packet; any reset before that returns to the previous
//                       app; no packet within 3 h -> it restarts into the previous app (a deaf radio is the failure that strands a
//                       remote node).
// Result codes (CtrlBlock.last_result, `fw ctl`): 0 ok, 1 no valid slot, 2 compression, 3 size, 4 decode, 5 CRC, 6 no partition,
// 7 write, 8 not an ESP32-S3 image, 9 the image failed the ESP-IDF check, 11 no HELTECV4 tag, 12 gave up (interrupted 8x), 13 rolled back.
#if defined(ESP32)
#include "FwOtaSlots.h"
#include "FwOtaInstallEsp32.h"
#include "Watchdog.h"
#include "lzma/LzmaDec.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <string.h>
#include <stdlib.h>

extern "C" bool verifyRollbackLater() { return true; }   // Arduino: do not confirm a new app at start - probationTick() does

namespace fw_ota {

static void* szAlloc(ISzAllocPtr, size_t s) { return malloc(s); }
static void szFree(ISzAllocPtr, void* a) { free(a); }
static const ISzAlloc kAlloc = { szAlloc, szFree };

struct InstallSink {
  const esp_partition_t* part; uint32_t off = 0, crc = 0; bool hdr_ok = false, tag = false, werr = false;
  uint8_t carry[sizeof(FwTag) - 1]; uint8_t nc = 0;
  void put(const uint8_t* d, uint32_t n) {
    if (off == 0 && n >= 16) hdr_ok = d[0] == 0xE9 && (d[12] | d[13] << 8) == 0x0009;   // esp_image_header_t: magic, chip_id ESP32-S3
    if (esp_partition_write(part, off, d, n) != ESP_OK) werr = true;
    crc = crc32(d, n, crc);
    if (!tag) {                                                // the 16-byte tag may straddle two blocks
      static uint8_t w[sizeof(carry) + 4096];
      for (uint32_t done = 0; done < n && !tag;) {
        uint32_t k = n - done < 4096 ? n - done : 4096;
        memcpy(w, carry, nc); memcpy(w + nc, d + done, k);
        const uint32_t wl = nc + k;
        for (uint32_t i = 0; i + sizeof(FwTag) <= wl; i++) if (memcmp(w + i, &g_fwpull_tag, sizeof(FwTag)) == 0) { tag = true; break; }
        nc = (uint8_t)(wl < sizeof(carry) ? wl : sizeof(carry)); memcpy(carry, w + wl - nc, nc);
        done += k;
      }
    }
    off += n;
  }
};

static int installSlot(const SlotHeader& h) {
  if (!slotValid()) return 1;
  if (h.compression != COMP_LZMA && h.compression != COMP_NONE) return 2;
  const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
  if (!part) return 6;
  if (h.raw_len == 0 || h.raw_len > part->size) return 3;
  const uint32_t er = (h.raw_len + 0xFFF) & ~0xFFFu;
  for (uint32_t o = 0; o < er; o += 0x10000) {                // the other partition only (the running app is never written)
    g_wdt.feedAlways();
    if (esp_partition_erase_range(part, o, er - o < 0x10000 ? er - o : 0x10000) != ESP_OK) return 7;
  }
  InstallSink sink; sink.part = part;
  static uint8_t in[1024], out[4096];
  const uint32_t src = SLOT_ADDR + SLOT_PAYLOAD_OFFSET;
  if (h.compression == COMP_NONE) {
    for (uint32_t o = 0; o < h.raw_len; o += sizeof(out)) {
      const uint32_t n = h.raw_len - o < sizeof(out) ? h.raw_len - o : (uint32_t)sizeof(out);
      flashRead(out, src + o, n); sink.put(out, n); g_wdt.feedAlways();
    }
  } else {
    CLzmaDec st; LzmaDec_Construct(&st);
    if (LzmaDec_Allocate(&st, h.lzma_props, LZMA_PROPS_SIZE, &kAlloc) != SZ_OK) return 4;   // the dictionary (64 KB for HELTECV4 images)
    LzmaDec_Init(&st);
    uint32_t in_off = 0; SizeT in_pos = 0, in_len = 0; int r = 0; bool end = false;
    while (!end && !r) {
      if (in_pos == in_len && in_off < h.payload_len) {
        in_len = h.payload_len - in_off < sizeof(in) ? h.payload_len - in_off : sizeof(in);
        flashRead(in, src + in_off, (uint32_t)in_len); in_off += (uint32_t)in_len; in_pos = 0;
      }
      SizeT il = in_len - in_pos, ol = sizeof(out); ELzmaStatus status;
      if (LzmaDec_DecodeToBuf(&st, out, &ol, in + in_pos, &il, LZMA_FINISH_ANY, &status) != SZ_OK) { r = 4; break; }
      in_pos += il;
      if (ol) { if (sink.off + ol > h.raw_len) { r = 4; break; } sink.put(out, (uint32_t)ol); }
      if (status == LZMA_STATUS_FINISHED_WITH_MARK) end = true;
      else if (!il && !ol && in_off >= h.payload_len && in_pos == in_len) r = 4;   // input used up without the end marker
      if ((sink.off & 0xFFFF) < ol) g_wdt.feedAlways();
    }
    LzmaDec_Free(&st, &kAlloc);
    if (r) return r;
  }
  if (sink.werr) return 7;
  if (sink.off != h.raw_len) return 4;
  if (!sink.hdr_ok) return 8;
  if (sink.crc != h.raw_crc32) return 5;
  if (!sink.tag) return 11;
  if (esp_ota_set_boot_partition(part) != ESP_OK) return 9;    // ESP-IDF verifies the image (segments + its appended SHA-256)
  return 0;
}

bool runningSlotImage(const SlotHeader& h) {
  return h.raw_len > 0 && h.raw_len <= APP_MAX_SIZE && crc32_flash(appRunAddr(), h.raw_len) == h.raw_crc32;
}

void bootApplyEsp32() {
  CtrlBlock cb; if (!readCtrl(cb)) return;
  SlotHeader h; const bool hv = readSlotHeader(h);
  if (cb.apply_state == APPLY_DONE) {                          // installed last time: did the new app confirm itself, or roll back?
    if (hv && esp_ota_get_last_invalid_partition() && !runningSlotImage(h)) { cb.apply_state = APPLY_FAILED; cb.last_result = 13; writeCtrl(cb); }
    return;
  }
  if (cb.apply_state != APPLY_PENDING) return;
  if (hv && runningSlotImage(h)) { cb.apply_state = APPLY_DONE; cb.last_result = 0; writeCtrl(cb); return; }   // already running it
  if (cb.boot_attempts >= 8) { cb.apply_state = APPLY_FAILED; cb.last_result = 12; writeCtrl(cb); return; }
  cb.boot_attempts++; writeCtrl(cb);                           // counted before: a power cut during the unpack is an interrupted try
  Serial.println("fw_ota: installing the slot image into the other app partition"); Serial.flush();
  const int r = hv ? installSlot(h) : 1;
  if (r) {                                                     // the old app keeps running; nothing to retry for a refused image
    cb.fail_count++; cb.apply_state = APPLY_FAILED; cb.last_result = (uint32_t)r; writeCtrl(cb);
    Serial.printf("fw_ota: install failed (%d), this app keeps running\n", r);
    return;
  }
  cb.apply_state = APPLY_DONE; cb.last_result = 0; writeCtrl(cb);
  Serial.println("fw_ota: installed - restarting into the new app (on probation)"); Serial.flush(); delay(100);
  esp_restart();
}

static int8_t s_probation = -1;                                // -1 not checked yet, 1 on probation, 0 confirmed / not an update
bool onProbation() {
  if (s_probation < 0) {
    esp_ota_img_states_t st;
    s_probation = (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) ? 1 : 0;
  }
  return s_probation == 1;
}
void confirmApp() { if (onProbation()) { esp_ota_mark_app_valid_cancel_rollback(); s_probation = 0; } }
void probationTick(uint32_t uptime_ms, uint32_t rx_packets) {
  if (!onProbation()) return;
#ifdef FW_OTA_TEST_NEVER_CONFIRM
  rx_packets = 0; if (uptime_ms >= 180000) uptime_ms = PROBATION_MAX_MS;   // (bench test build only) a deaf app: back after 3 min
#endif
  if (uptime_ms >= PROBATION_MIN_MS && rx_packets > 0) {
    esp_ota_mark_app_valid_cancel_rollback(); s_probation = 0;
    Serial.println("fw_ota: new app confirmed (loop ran 2 min, the radio received)");
  } else if (uptime_ms >= PROBATION_MAX_MS) {
    Serial.println("fw_ota: no packet received in 3 h on probation - back to the previous app"); Serial.flush(); delay(100);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

}  // namespace fw_ota
#endif  // ESP32
