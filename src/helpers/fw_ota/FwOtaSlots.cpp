#include "FwOtaSlots.h"
#include <string.h>
#include <Arduino.h>   // yield(), delay()
#include <nrf_soc.h>    // sd_flash_write()
#include <nrf_sdm.h>    // sd_softdevice_is_enabled()

// framework's SoftDevice-aware flash driver (in libInternalFileSytem.a, already
// linked because InternalFS is used). Declared here to avoid dragging in the
// Adafruit core's private common_inc.h from the library src path.
extern "C" {
  void flash_nrf5x_flush(void);
  bool flash_nrf5x_erase(uint32_t addr);
  int  flash_nrf5x_write(uint32_t dst, void const* src, uint32_t len);
  int  flash_nrf5x_read(void* dst, uint32_t src, uint32_t len);
}

// Arduino core global (cores/nRF5/wiring.c), populated by init() before
// setup() runs, from NRF_TIMER2->CC[0] - same value the bootloader sets to
// its own MK_BOOTLOADER_VERSION on every boot. Global scope (not
// fw_ota::bootloaderVersion) - this is the actual Adafruit core symbol.
extern uint32_t bootloaderVersion;

namespace fw_ota {

// ---------------------------------------------------------------------------
// CRC-32/IEEE, nibble-table (small + fast enough for a ~330 KiB image)
// ---------------------------------------------------------------------------
static const uint32_t kCrcNib[16] = {
  0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
  0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
  0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
  0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
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

uint32_t crc32_flash(uint32_t addr, size_t len, uint32_t seed) {
  uint32_t crc = ~seed;
  uint8_t buf[256];
  flash_nrf5x_flush();                     // then read the memory-mapped flash directly (chunks are programmed behind the page cache)
  while (len) {
    uint32_t n = len < sizeof(buf) ? (uint32_t)len : (uint32_t)sizeof(buf);
    memcpy(buf, (const void*)addr, n);
    for (uint32_t i = 0; i < n; i++) {
      crc ^= buf[i];
      crc = (crc >> 4) ^ kCrcNib[crc & 0x0F];
      crc = (crc >> 4) ^ kCrcNib[crc & 0x0F];
    }
    addr += n;
    len  -= n;
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Low-level flash
// ---------------------------------------------------------------------------
bool flashErase(uint32_t addr, uint32_t len) {
  uint32_t a = addr & ~(FLASH_PAGE_SIZE - 1);
  uint32_t end = addr + len;
  bool ok = true;
  uint32_t i = 0;
  for (; a < end; a += FLASH_PAGE_SIZE) {
    if (!flash_nrf5x_erase(a)) ok = false;
    if ((++i & 3) == 0) yield();   // let the SoftDevice / radio breathe every 16 KiB
  }
  flash_nrf5x_flush();
  return ok;
}

bool flashWrite(uint32_t addr, const void* src, uint32_t len) {
  int w = flash_nrf5x_write(addr, src, len);
  flash_nrf5x_flush();
  return w == (int)len || w == 0;  // impl returns bytes written or 0
}

void flashRead(void* dst, uint32_t addr, uint32_t len) {
  flash_nrf5x_flush();
  memcpy(dst, (const void*)addr, len);
}

void flashFlush() { flash_nrf5x_flush(); }

// ---------------------------------------------------------------------------
// Slot - single slot, no more A/B selector
// ---------------------------------------------------------------------------
bool readSlotHeader(SlotHeader& out) {
  memset(&out, 0, sizeof(out));
  flash_nrf5x_read(&out, SLOT_ADDR, sizeof(out));
  return out.magic == SLOT_MAGIC;
}

bool slotValid() {
  SlotHeader h;
  if (!readSlotHeader(h)) return false;
  if (h.payload_len == 0 || h.payload_len > SLOT_PAYLOAD_MAX) return false;
  if (h.compression == COMP_NONE && h.raw_len != h.payload_len) return false;
  if (h.raw_len == 0 || h.raw_len > APP_MAX_SIZE) return false;
  uint32_t c = crc32_flash(SLOT_ADDR + SLOT_PAYLOAD_OFFSET, h.payload_len);
  return c == h.payload_crc32;
}

bool eraseSlot() {
  return flashErase(SLOT_ADDR, SLOT_SIZE);
}

bool eraseSlotHeader() {
  bool ok = flash_nrf5x_erase(SLOT_ADDR);
  flash_nrf5x_flush();
  return ok;
}

// ----- sparse payload access (LoRa pull): erase the payload up front, then program each chunk where it belongs -----
bool slotErasePayloadPage(uint32_t page) {
  return flash_nrf5x_erase(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + page * FLASH_PAGE_SIZE);
}

// Program `len` bytes at payload offset `off` (4-byte aligned, target erased, never programmed before). Direct word programming:
// ~1.6 ms per 160 B chunk, instead of the framework cache's erase + rewrite of the whole 4 KiB page for every chunk.
bool slotProgramAt(uint32_t off, const void* data, uint32_t len) {
  if ((off & 3) || off + len > SLOT_PAYLOAD_MAX + 3) return false;
  uint32_t tmp[(FW_OTA_MAX_PROGRAM + 3) / 4];
  uint32_t wlen = (len + 3) & ~3u;
  if (wlen > sizeof(tmp)) return false;
  memset(tmp, 0xFF, wlen);
  memcpy(tmp, data, len);
  uint32_t addr = SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off;
  flash_nrf5x_flush();
  uint8_t sd_en = 0;
  sd_softdevice_is_enabled(&sd_en);
  if (sd_en) {                                            // async flash API while the SoftDevice runs: use the (slow) cached path
    flash_nrf5x_write(addr, tmp, wlen);
    flash_nrf5x_flush();
  } else {
    for (uint8_t attempt = 0; attempt < 20 && memcmp((const void*)addr, tmp, wlen) != 0; attempt++) {
      if (sd_flash_write((uint32_t*)addr, tmp, wlen / 4) != NRF_SUCCESS) delay(1);
    }
  }
  return memcmp((const void*)addr, tmp, wlen) == 0;
}

bool slotPayloadPresent(uint32_t off, uint32_t len) {     // true if any byte of the area is programmed (not all 0xFF)
  const uint8_t* p = (const uint8_t*)(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off);
  for (uint32_t i = 0; i < len; i++) if (p[i] != 0xFF) return true;
  return false;
}

// Program one page-aligned 4 KiB page, verify by read-back, retry a few times.
// The framework flash cache does erase+program on flush; we only add read-back
// verification, retry, and a scheduler slice so the SoftDevice can run the radio.
static bool writePageVerified(uint32_t page_addr, const uint8_t* data) {
  for (int attempt = 0; attempt < 4; attempt++) {
    flash_nrf5x_write(page_addr, data, FLASH_PAGE_SIZE);
    flash_nrf5x_flush();
    if (memcmp((const void*)page_addr, data, FLASH_PAGE_SIZE) == 0) { yield(); return true; }
    delay(5);
  }
  return false;
}

// ----- SlotWriter: page-buffered payload streaming -----
bool SlotWriter::begin() {
  payload_off = 0;
  page_fill = 0;
  crc = 0;
  failed = false;
  memset(page, 0xFF, sizeof(page));
  // wipe the header page up front so the slot never looks valid mid-write
  return eraseSlotHeader();
}

bool SlotWriter::write(const void* data, uint32_t len) {
  if (failed) return false;
  const uint8_t* p = (const uint8_t*)data;
  crc = crc32(p, len, crc);
  while (len) {
    if (payload_off + page_fill + len > SLOT_PAYLOAD_MAX) { failed = true; return false; }
    uint32_t space = FLASH_PAGE_SIZE - page_fill;
    uint32_t n = len < space ? len : space;
    memcpy(page + page_fill, p, n);
    page_fill += n; p += n; len -= n;
    if (page_fill == FLASH_PAGE_SIZE) {
      uint32_t addr = SLOT_ADDR + SLOT_PAYLOAD_OFFSET + payload_off;
      if (!writePageVerified(addr, page)) { failed = true; return false; }
      payload_off += FLASH_PAGE_SIZE;
      page_fill = 0;
      memset(page, 0xFF, sizeof(page));
    }
  }
  return true;
}

// Sparse pull: the whole payload is already in place; only the header page is written (it makes the slot VALID).
bool SlotWriter::finishSparse(SlotHeader hdr, uint32_t payload_len, uint32_t payload_crc) {
  failed = false; page_fill = 0; payload_off = payload_len; crc = payload_crc;
  return finish(hdr);
}

bool SlotWriter::finish(SlotHeader hdr) {
  if (failed) return false;
  if (page_fill) {   // flush the final partial page (tail already 0xFF)
    uint32_t addr = SLOT_ADDR + SLOT_PAYLOAD_OFFSET + payload_off;
    if (!writePageVerified(addr, page)) { failed = true; return false; }
    payload_off += page_fill;
    page_fill = 0;
  }
  hdr.magic        = SLOT_MAGIC;
  hdr.hdr_version   = 1;
  hdr.payload_len   = payload_off;
  hdr.payload_crc32 = crc;

  // reuse the (now-flushed) page buffer for the header page
  memset(page, 0xFF, sizeof(page));
  memcpy(page, &hdr, sizeof(hdr));
  return writePageVerified(SLOT_ADDR, page);   // header written last
}

// ---------------------------------------------------------------------------
// Control page
// ---------------------------------------------------------------------------
bool readCtrl(CtrlBlock& out) {
  memset(&out, 0, sizeof(out));
  flash_nrf5x_read(&out, CTRL_ADDR, sizeof(out));
  if (out.magic != CTRL_MAGIC ||
      out.crc32 != crc32(&out, sizeof(out) - sizeof(out.crc32))) {
    memset(&out, 0, sizeof(out));
    return false;
  }
  return true;
}

bool writeCtrl(const CtrlBlock& in) {
  CtrlBlock b = in;
  b.magic = CTRL_MAGIC;
  b.ctrl_version = 5;
  b.crc32 = crc32(&b, sizeof(b) - sizeof(b.crc32));
  if (!flashErase(CTRL_ADDR, CTRL_SIZE)) return false;
  return flashWrite(CTRL_ADDR, &b, sizeof(b));
}

bool ctrlInit() {
  CtrlBlock b;
  memset(&b, 0, sizeof(b));
  b.apply_state = APPLY_IDLE;
  return writeCtrl(b);
}

bool ctrlMarkPending() {
  CtrlBlock b;
  readCtrl(b);  // ok if blank; we rewrite the whole thing
  b.apply_state = APPLY_PENDING;
  b.boot_attempts = 0;
  b.fail_count = 0;
  b.requested_bl_version = ::bootloaderVersion;
  return writeCtrl(b);
}

// (the firmware tag itself lives in FwTag.cpp so a build WITHOUT fw_ota can still carry it)

}  // namespace fw_ota
