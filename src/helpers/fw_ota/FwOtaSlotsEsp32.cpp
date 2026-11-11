// (Heltec port, 2026-11-01) The FwOtaSlots.h interface on the ESP32-S3: the slot and the control record live in the "lotaslot" data
// partition (boards/lora_ota_esp32s3_16MB.csv), accessed with esp_flash_* at absolute flash addresses like the nRF52 version.
// Safety: flashErase / flashWrite refuse every address outside that partition - a bug here can never touch the running app, the other
// app partition (the installer writes it through esp_partition_*, FwOtaInstallEsp32.cpp) or the MeshCore files (identity, settings).
#if defined(ESP32)
#include "FwOtaSlots.h"
#include <string.h>
#include <Arduino.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

namespace fw_ota {

static const uint32_t kCrcNib[16] = {
  0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
  uint32_t crc = ~seed;
  const uint8_t* p = (const uint8_t*)data;
  while (len--) {
    crc ^= *p++;
    crc = (crc >> 4) ^ kCrcNib[crc & 0x0F];
    crc = (crc >> 4) ^ kCrcNib[crc & 0x0F];
  }
  return ~crc;
}

void flashRead(void* dst, uint32_t addr, uint32_t len) {
  if (esp_flash_read(NULL, dst, addr, len) != ESP_OK) memset(dst, 0xFF, len);   // reads as erased: never VALID
}

uint32_t crc32_flash(uint32_t addr, size_t len, uint32_t seed) {
  uint32_t crc = seed; uint8_t buf[256];
  while (len) {
    uint32_t n = len < sizeof(buf) ? (uint32_t)len : (uint32_t)sizeof(buf);
    flashRead(buf, addr, n); crc = crc32(buf, n, crc);
    addr += n; len -= n;
  }
  return crc;
}

uint32_t appRunAddr() {
  const esp_partition_t* p = esp_ota_get_running_partition();
  return p ? p->address : 0x10000;
}

// The slot exists only if the flash really has OUR partition table: a "lotaslot" data partition exactly at SLOT_PART_ADDR. With the stock
// table (e.g. this app installed by MeshCore's Wi-Fi updater, which does not change the table) that range is app0's tail and app1's head -
// writing it would destroy the running app. Checked once; without it every slot write is refused (LoRa-OTA then has no slot: nothing is
// pulled, the node stays a normal repeater, `fw slot` says why).
bool slotPartitionOk() {
  static int8_t ok = -1;
  if (ok < 0) {
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "lotaslot");
    ok = (p && p->address == SLOT_PART_ADDR && p->size == SLOT_PART_SIZE) ? 1 : 0;
  }
  return ok == 1;
}
// only the slot partition, page aligned for erases
static bool inSlotPart(uint32_t addr, uint32_t len) {
  return slotPartitionOk() && addr >= SLOT_PART_ADDR && len <= SLOT_PART_SIZE && addr - SLOT_PART_ADDR <= SLOT_PART_SIZE - len;
}

bool flashErase(uint32_t addr, uint32_t len) {
  uint32_t a = addr & ~(FLASH_PAGE_SIZE - 1), end = addr + len;
  uint32_t n = ((end - a) + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
  if (!inSlotPart(a, n)) return false;
  bool ok = true;
  for (uint32_t off = 0; off < n; off += FLASH_PAGE_SIZE) {   // one sector at a time: the radio task keeps running in between
    if (esp_flash_erase_region(NULL, a + off, FLASH_PAGE_SIZE) != ESP_OK) ok = false;
    if ((off & 0x3FFF) == 0) yield();
  }
  return ok;
}

bool flashWrite(uint32_t addr, const void* src, uint32_t len) {
  if (!inSlotPart(addr, len)) return false;
  return esp_flash_write(NULL, src, addr, len) == ESP_OK;
}

void flashFlush() {}

bool readSlotHeader(SlotHeader& out) {
  memset(&out, 0, sizeof(out));
  if (!slotPartitionOk()) return false;                   // another partition table: the range is not ours, never "valid"
  flashRead(&out, SLOT_ADDR, sizeof(out));
  return out.magic == SLOT_MAGIC;
}

bool slotValid() {
  SlotHeader h;
  if (!readSlotHeader(h)) return false;
  if (h.payload_len == 0 || h.payload_len > SLOT_PAYLOAD_MAX) return false;
  if (h.compression == COMP_NONE && h.raw_len != h.payload_len) return false;
  if (h.raw_len == 0 || h.raw_len > APP_MAX_SIZE) return false;
  return crc32_flash(SLOT_ADDR + SLOT_PAYLOAD_OFFSET, h.payload_len) == h.payload_crc32;
}

bool eraseSlot() { return flashErase(SLOT_ADDR, SLOT_SIZE); }
bool eraseSlotHeader() { return flashErase(SLOT_ADDR, FLASH_PAGE_SIZE); }

bool slotErasePayloadPage(uint32_t page) { return flashErase(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + page * FLASH_PAGE_SIZE, FLASH_PAGE_SIZE); }

// Program `len` bytes at payload offset `off` (target erased, never programmed before), verified by read-back.
bool slotProgramAt(uint32_t off, const void* data, uint32_t len) {
  if ((off & 3) || off + len > SLOT_PAYLOAD_MAX + 3 || len > FW_OTA_MAX_PROGRAM) return false;
  uint8_t tmp[(FW_OTA_MAX_PROGRAM + 3) & ~3u], rb[sizeof(tmp)];
  uint32_t wlen = (len + 3) & ~3u;
  memset(tmp, 0xFF, wlen); memcpy(tmp, data, len);
  const uint32_t addr = SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off;
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    flashRead(rb, addr, wlen);
    if (memcmp(rb, tmp, wlen) == 0) return true;
    if (!flashWrite(addr, tmp, wlen)) delay(1);
  }
  flashRead(rb, addr, wlen);
  return memcmp(rb, tmp, wlen) == 0;
}

bool slotPayloadPresent(uint32_t off, uint32_t len) {     // true if any byte of the area is programmed (not all 0xFF)
  uint8_t buf[64];
  for (uint32_t i = 0; i < len; i += sizeof(buf)) {
    uint32_t n = len - i < sizeof(buf) ? len - i : (uint32_t)sizeof(buf);
    flashRead(buf, SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off + i, n);
    for (uint32_t k = 0; k < n; k++) if (buf[k] != 0xFF) return true;
  }
  return false;
}

static bool writePageVerified(uint32_t page_addr, const uint8_t* data) {
  static uint8_t rb[FLASH_PAGE_SIZE];
  for (int attempt = 0; attempt < 4; attempt++) {
    if (flashErase(page_addr, FLASH_PAGE_SIZE) && flashWrite(page_addr, data, FLASH_PAGE_SIZE)) {
      flashRead(rb, page_addr, FLASH_PAGE_SIZE);
      if (memcmp(rb, data, FLASH_PAGE_SIZE) == 0) { yield(); return true; }
    }
    delay(5);
  }
  return false;
}

bool SlotWriter::begin() {
  payload_off = 0; page_fill = 0; crc = 0; failed = false;
  memset(page, 0xFF, sizeof(page));
  return eraseSlotHeader();                               // the slot never looks valid mid-write
}

bool SlotWriter::write(const void* data, uint32_t len) {
  if (failed) return false;
  const uint8_t* p = (const uint8_t*)data;
  crc = crc32(p, len, crc);
  while (len) {
    if (payload_off + page_fill + len > SLOT_PAYLOAD_MAX) { failed = true; return false; }
    uint32_t space = FLASH_PAGE_SIZE - page_fill, n = len < space ? len : space;
    memcpy(page + page_fill, p, n); page_fill += n; p += n; len -= n;
    if (page_fill == FLASH_PAGE_SIZE) {
      if (!writePageVerified(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + payload_off, page)) { failed = true; return false; }
      payload_off += FLASH_PAGE_SIZE; page_fill = 0; memset(page, 0xFF, sizeof(page));
    }
  }
  return true;
}

bool SlotWriter::finishSparse(SlotHeader hdr, uint32_t payload_len, uint32_t payload_crc) {
  failed = false; page_fill = 0; payload_off = payload_len; crc = payload_crc;
  return finish(hdr);
}

bool SlotWriter::finish(SlotHeader hdr) {
  if (failed) return false;
  if (page_fill) {
    if (!writePageVerified(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + payload_off, page)) { failed = true; return false; }
    payload_off += page_fill; page_fill = 0;
  }
  hdr.magic = SLOT_MAGIC; hdr.hdr_version = 1; hdr.payload_len = payload_off; hdr.payload_crc32 = crc;
  memset(page, 0xFF, sizeof(page)); memcpy(page, &hdr, sizeof(hdr));
  return writePageVerified(SLOT_ADDR, page);              // header written last
}

// Control record: the same 64-byte CtrlBlock as the nRF52 control page, in the slot partition's last sector. The "bootloader" that acts
// on it is FwOtaInstallEsp32.cpp at the start of the next boot.
// two copies (CTRL_ADDR, CTRL_ADDR + 4 KiB), reserved[4..7] = sequence: read the newer valid one, write over the other
static bool readCopy(int i, CtrlBlock& c) {
  flashRead(&c, CTRL_ADDR + (uint32_t)i * CTRL_SIZE, sizeof(c));
  return c.magic == CTRL_MAGIC && c.crc32 == crc32(&c, sizeof(c) - sizeof(c.crc32));
}
static uint32_t seqOf(const CtrlBlock& c) { uint32_t s; memcpy(&s, c.reserved + 4, 4); return s; }
static int newestCopy(CtrlBlock* out) {                   // -1 = none valid
  CtrlBlock a, b; const bool va = readCopy(0, a), vb = readCopy(1, b);
  const int k = (va && vb) ? ((int32_t)(seqOf(b) - seqOf(a)) > 0 ? 1 : 0) : va ? 0 : vb ? 1 : -1;
  if (out) { if (k == 0) *out = a; else if (k == 1) *out = b; }
  return k;
}
bool readCtrl(CtrlBlock& out) {
  memset(&out, 0, sizeof(out));
  if (!slotPartitionOk()) return false;
  CtrlBlock c;
  if (newestCopy(&c) < 0) return false;
  out = c; return true;
}

bool writeCtrl(const CtrlBlock& in) {
  CtrlBlock cur; const int k = newestCopy(&cur);
  CtrlBlock b = in;
  const uint32_t seq = (k < 0 ? 0 : seqOf(cur)) + 1; memcpy(b.reserved + 4, &seq, 4);
  b.magic = CTRL_MAGIC; b.ctrl_version = 5;
  b.crc32 = crc32(&b, sizeof(b) - sizeof(b.crc32));
  const uint32_t at = CTRL_ADDR + (uint32_t)(k == 0 ? 1 : 0) * CTRL_SIZE;   // never over the newest valid copy
  if (!flashErase(at, CTRL_SIZE)) return false;
  return flashWrite(at, &b, sizeof(b));
}

// (Heltec port) a FAILED / rolled-back record speaks only about the image it tried: another image in the slot (a new pull or seed) is clean
bool failedForSlot(const CtrlBlock& cb, const SlotHeader& h) {
  return cb.apply_state == APPLY_FAILED && memcmp(cb.reserved, &h.payload_crc32, 4) == 0;
}

bool ctrlInit() { CtrlBlock b; memset(&b, 0, sizeof(b)); b.apply_state = APPLY_IDLE; return writeCtrl(b); }

bool ctrlMarkPending() {
  CtrlBlock b; readCtrl(b);
  b.apply_state = APPLY_PENDING; b.boot_attempts = 0; b.fail_count = 0; b.last_result = 0;
  b.requested_bl_version = 0;                             // no bootloader version fence on ESP32 (the installer is part of the app)
  SlotHeader h; if (readSlotHeader(h)) memcpy(b.reserved, &h.payload_crc32, 4);   // which image this record is about (failedForSlot)
  return writeCtrl(b);
}

}  // namespace fw_ota
#endif  // ESP32
