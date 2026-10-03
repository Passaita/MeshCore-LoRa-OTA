/*
 * fw_ota - LoRa firmware-pull staging support for RAK3401 (nRF52840).
 *
 * This header defines the on-flash layout shared between the application and
 * the (forked OTAFIX) bootloader:
 *   - ONE staging slot that holds an LZMA-compressed firmware image
 *   - a control page the app uses to ask the bootloader to apply the slot,
 *     and the bootloader uses to report the result
 *
 * Single slot: no A/B rollback on a failed post-boot self-test - the safety net
 * is strict pre-flash verification (the bootloader and the app both refuse to
 * apply / mark valid a slot whose SHA-256 does not match) plus OTAFIX's BLE DFU
 * kept as a manual recovery path. Geometry MUST match the bootloader's
 * src/fw_ota/fw_ota.h and boards/nrf52840_s140_v6_fwota.ld.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace fw_ota {

// ---------------------------------------------------------------------------
// Flash geometry  (keep in sync with the linker script)
// ---------------------------------------------------------------------------
static const uint32_t FLASH_PAGE_SIZE  = 4096;

static const uint32_t APP_REGION_START = 0x26000;
static const uint32_t APP_REGION_END   = 0x96000;   // linker FLASH end == slot start (448 KiB app)

static const uint32_t CTRL_ADDR        = 0xEC000;
static const uint32_t CTRL_SIZE        = FLASH_PAGE_SIZE;
static const uint32_t SLOT_ADDR        = 0x96000;
static const uint32_t SLOT_SIZE        = CTRL_ADDR - SLOT_ADDR;   // 344 KiB, single slot

static const uint32_t APP_MAX_SIZE     = APP_REGION_END - APP_REGION_START;  // 0x70000

// ---------------------------------------------------------------------------
// Slot header - first bytes of a slot, followed immediately by the payload
// ---------------------------------------------------------------------------
static const uint32_t SLOT_MAGIC = 0x4F544C46;   // "FLTO"

enum Compression : uint16_t {
  COMP_NONE = 0,
  // 1 was COMP_HEATSHRINK (did not compress enough). Retired; do not reuse this value.
  COMP_LZMA = 2,   // raw LZMA1 stream; lzma_props[5] below carries lc/lp/pb + dict size.
                    // Value matches the bootloader's FW_OTA_COMP_LZMA exactly.
};

struct SlotHeader {
  uint32_t magic;         // SLOT_MAGIC when a complete, valid image is present
  uint32_t payload_len;   // bytes of payload following this header
  uint32_t raw_len;       // uncompressed image size (== payload_len if COMP_NONE)
  uint32_t payload_crc32; // CRC-32/IEEE over payload_len payload bytes
  uint16_t compression;   // Compression
  uint16_t hdr_version;   // = 1
  uint32_t fw_version;    // monotonic image version (anti-rollback / display)
  uint32_t raw_crc32;     // CRC-32/IEEE over the decompressed raw_len bytes
  uint8_t  lzma_props[5]; // LZMA raw-stream props (lc/lp/pb + dict size)
  uint8_t  reserved[3];
};  // 32 bytes

// The header lives alone in the slot's first flash page; the payload starts on
// the next page boundary so every payload write is a full, page-aligned program.
static const uint32_t SLOT_PAYLOAD_OFFSET = FLASH_PAGE_SIZE;   // 0x1000
static const uint32_t SLOT_PAYLOAD_MAX    = SLOT_SIZE - FLASH_PAGE_SIZE;

// ---------------------------------------------------------------------------
// Firmware tag - every image the fw_ota bootloader accepts must carry this
// within its first KiB (placed by boards/nrf52_common_fwota.ld right after the
// vector table). Keeps stock / other-board firmware from being flashed by
// accident; the bootloader checks it before writing (raw DFU) or erasing
// (apply from slot). Keep in sync with the bootloader's fw_ota_tag_t.
// ---------------------------------------------------------------------------
static const uint32_t TAG_MAGIC   = 0x4C505746;   // bytes 'F','W','P','L'
static const uint32_t TAG_VERSION = 1;
#define FW_OTA_TAG_BOARD "RAK3401"

struct FwTag {
  uint32_t magic;       // TAG_MAGIC
  uint16_t version;     // TAG_VERSION
  uint16_t reserved;
  char     board[8];    // NUL-padded board id
};  // 16 bytes
static_assert(sizeof(FwTag) == 16, "FwTag must match the bootloader's fw_ota_tag_t");
extern const FwTag g_fwpull_tag;   // this firmware's own tag (FwTag.cpp): its board id is also the board id of the bootloader it needs

// ---------------------------------------------------------------------------
// Control page
// ---------------------------------------------------------------------------
static const uint32_t CTRL_MAGIC = 0x4F544343;   // "CCTO"

enum ApplyState : uint32_t {
  APPLY_IDLE    = 0,   // nothing to do
  APPLY_PENDING = 1,   // app -> bootloader: apply the slot on next boot
  APPLY_DONE    = 2,   // bootloader -> app: slot applied, now running it
  APPLY_FAILED  = 3,   // bootloader -> app: could not apply; app left untouched
};

// Single-slot: no more active_slot/pending_slot A-vs-B (matches the
// bootloader's fw_ota_ctrl_t exactly - keep both in sync).
struct CtrlBlock {
  uint32_t magic;         // CTRL_MAGIC
  uint32_t ctrl_version;  // = 5 (v5 adds crc32; v4 fail_count; v3 requested_bl_version)
  uint32_t apply_state;   // ApplyState
  uint32_t boot_attempts; // apply attempts STARTED (bootloader bumps before each); zeroed on success / new request
  uint32_t last_result;   // bootloader detail/errno for the app to log
  // Stamped by ctrlMarkPending() with the Adafruit core's `bootloaderVersion`
  // global (== NRF_TIMER2->CC[0], set by the bootloader on every boot before
  // jumping to the app). The bootloader refuses to act on a PENDING request
  // stamped with any version other than its own currently-running one -
  // see fw_ota_apply.c's version fence. This exists because a bootloader-
  // only DFU flash never touches this control page, so a stale PENDING left
  // over from testing under a previous bootloader build must not be acted
  // on automatically by a newly-flashed one (2026-09-16 bricked-unit
  // incident's suspected root cause).
  uint32_t requested_bl_version;
  // Apply attempts that ended in a DETECTED failure (bad slot, decode error,
  // CRC mismatch). interrupted attempts (power loss / hang) = boot_attempts -
  // fail_count. Matches the bootloader's fw_ota_ctrl_t - keep in sync.
  uint32_t fail_count;
  uint8_t  reserved[32];
  // CRC-32/IEEE over the first 60 bytes (set by writeCtrl, checked by readCtrl
  // and by the bootloader). A record torn by a power cut is treated as blank
  // instead of possibly aliasing a different valid state. Keep in sync with the
  // bootloader's fw_ota_ctrl_t.
  uint32_t crc32;
};  // 64 bytes
static_assert(sizeof(CtrlBlock) == 64, "CtrlBlock must match the bootloader's fw_ota_ctrl_t");

// ---------------------------------------------------------------------------
// CRC-32/IEEE (poly 0xEDB88320) - matches zlib / PlatformIO's crc32
// ---------------------------------------------------------------------------
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
uint32_t crc32_flash(uint32_t addr, size_t len, uint32_t seed = 0);

// ---------------------------------------------------------------------------
// Low-level flash (SoftDevice-aware, via the framework's flash_nrf5x)
// ---------------------------------------------------------------------------
bool flashErase(uint32_t addr, uint32_t len);   // page-aligned erase of [addr, addr+len)
bool flashWrite(uint32_t addr, const void* src, uint32_t len);

// Sparse payload access for the LoRa pull (chunks arrive in any order, each is programmed once where it belongs)
#define FW_OTA_MAX_PROGRAM 164
bool slotErasePayloadPage(uint32_t page);
bool slotProgramAt(uint32_t off, const void* data, uint32_t len);
bool slotPayloadPresent(uint32_t off, uint32_t len);
void flashRead(void* dst, uint32_t addr, uint32_t len);
void flashFlush();

// ---------------------------------------------------------------------------
// Slot helpers - single slot, no more A/B selector
// ---------------------------------------------------------------------------
bool     readSlotHeader(SlotHeader& out);
bool     slotValid();                 // magic + length sane + CRC matches
bool     eraseSlot();

bool     eraseSlotHeader();            // just the header page (marks slot empty)

// Stream a payload into the slot, page-buffered. Each full 4 KiB page is written
// once, verified, and retried on failure; the SoftDevice/radio is given a slice
// between pages. Call begin() -> write() in order -> finish(hdr).
struct SlotWriter {
  uint32_t payload_off;           // next payload byte offset to write
  uint32_t page_fill;             // bytes buffered in `page`
  uint32_t crc;                   // running CRC-32 over payload
  bool     failed;
  uint8_t  page[FLASH_PAGE_SIZE];

  bool begin();
  bool write(const void* data, uint32_t len);
  bool finish(SlotHeader hdr);    // hdr.payload_len/crc32 filled in from the stream
  bool finishSparse(SlotHeader hdr, uint32_t payload_len, uint32_t payload_crc);   // payload already in place (LoRa pull)
};

// ---------------------------------------------------------------------------
// Control page
// ---------------------------------------------------------------------------
bool readCtrl(CtrlBlock& out);       // false + zeroed struct if blank/corrupt
bool writeCtrl(const CtrlBlock& in); // erase + rewrite the whole page
bool ctrlInit();                     // fresh IDLE control block
bool ctrlMarkPending();

}  // namespace fw_ota
