// LoRa-OTA migrator engine (Heltec V4): moves a node from stock MeshCore (Arduino default_16MB table) to the LoRa-OTA
// layout (boards/lora_ota_esp32s3_16MB.csv) without USB. Design: docs/lora-ota/HELTEC-V4-MIGRATOR.md.
// Plain C++ (no ESP-IDF headers): the same code runs on the board (migrator.cpp gives it the flash) and in the host test
// (test/migrator_host_test.cpp: RAM flash, an ESP-IDF bootloader model and a power cut at every erase/write).
//
// Plan (both stock cases end in the same layout; only the one 4 KiB table sector has no second copy):
//   main app  -> 0x310000 (LoRa-OTA app1 = ota_1)        seeder -> 0x810000 (LoRa-OTA factory)
//   case R1: migrator runs from stock app1 (0x650000), stock MeshCore stays in app0 (0x10000) = LoRa-OTA app0 -> stock is the rollback
//   case R0: migrator runs from stock app0 (0x10000) = LoRa-OTA app0 -> the migrator itself is the rollback (its page comes back)
//   commit:  check both images -> erase the slot's header + control sectors -> write the intent record ->
//            R1: otadata (ota_1 NEW, then the fallback ota_0 VALID below it) -> table        R0: table -> otadata (same entries)
//   The order keeps a bootable app after a power cut at any point except inside the table sector's erase + first page.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mig {

struct Flash {                                   // absolute flash addresses
  virtual bool read(uint32_t addr, void* dst, uint32_t len) = 0;
  virtual bool erase(uint32_t addr, uint32_t len) = 0;        // 4 KiB aligned
  virtual bool write(uint32_t addr, const void* src, uint32_t len) = 0;
  virtual bool writeTable(const void* src, uint32_t len) = 0; // erase the 0x8000 sector + write: the only unprotected step
  virtual ~Flash() {}
};

static const uint32_t SECTOR = 0x1000;
static const uint32_t TABLE_ADDR = 0x8000, TABLE_LEN = 0xC00;
static const uint32_t OTADATA_ADDR = 0xE000;
static const uint32_t STOCK_APP0 = 0x10000, STOCK_APP1 = 0x650000, STOCK_APP_SIZE = 0x640000;
static const uint32_t MAIN_ADDR = 0x310000, APP_SIZE = 0x300000;            // LoRa-OTA app1
static const uint32_t SEEDER_ADDR = 0x810000, SEEDER_SIZE = 0x200000;       // LoRa-OTA factory
static const uint32_t SLOT_ADDR = 0x610000, SLOT_SIZE = 0x200000;           // LoRa-OTA lotaslot
static const uint32_t CTRL_ADDR = SLOT_ADDR + SLOT_SIZE - 2 * SECTOR;       // its two control sectors
static const uint32_t MARK_ADDR = SLOT_ADDR;     // intent record in the slot header sector (the app reads it as "no slot")
static const uint32_t DATA_START = 0xC90000;     // spiffs (MeshCore files) + coredump: never written

enum OtaState : uint32_t { OTA_NEW = 0, OTA_PENDING = 1, OTA_VALID = 2, OTA_INVALID = 3, OTA_ABORTED = 4, OTA_UNDEF = 0xFFFFFFFFu };

struct Part { char name[17]; uint8_t type, subtype; uint32_t offset, size; };
enum TableKind { TABLE_STOCK, TABLE_NEW, TABLE_PARTIAL, TABLE_OTHER };   // PARTIAL = our own table write was cut off
int parseTable(const uint8_t* t, uint32_t len, Part* out, int max);   // number of entries, -1 = not a table
TableKind classifyTable(Flash& f);

struct Image { bool ok; uint32_t len; bool tag; const char* why; };
// ESP32-S3 app image at `addr`: header, segments, checksum, appended SHA-256 (required); `tag` = the HELTECV4 LoRa-OTA tag inside
Image checkImage(Flash& f, uint32_t addr, uint32_t maxLen);

struct OtaSel { uint32_t seq; uint8_t label[20]; uint32_t state; uint32_t crc; };   // esp_ota_select_entry_t, 32 bytes
uint32_t otaCrc(uint32_t seq);                    // esp_rom_crc32_le(UINT32_MAX, &seq, 4)
bool otaValid(const OtaSel& s);                   // the bootloader's view (rollback build): seq set, crc ok, not INVALID/ABORTED
int otaActive(const OtaSel s[2], int* sector);    // index (seq-1)%2 of the newest valid entry, -1 = none

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
struct Sha256 { uint32_t h[8]; uint64_t n; uint8_t buf[64]; uint32_t used; void init(); void add(const uint8_t* d, size_t len); void done(uint8_t out[32]); };
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);

struct Mark { uint32_t magic, version, run_addr, main_len, seeder_len, reserved[2], crc; };   // 32 bytes
static const uint32_t MARK_MAGIC = 0x4347494D;   // "MIGC"

struct BundleHdr { char magic[8]; uint32_t main_len, seeder_len, fw, reserved[2], crc; };  // 32 bytes, then main, then seeder
                                                                                            // crc = crc32 of the first 28 bytes
static const char BUNDLE_MAGIC[8] = {'L', 'O', 'T', 'A', 'M', 'I', 'G', '1'};

class Engine {
 public:
  Engine(Flash& f, const uint8_t* newTable, uint32_t runAddr) : f_(f), table_(newTable), run_(runAddr) {}

  // Looks at the flash: which table, which case, where the images are, what is protected. false + why() = nothing may be done.
  bool plan();
  bool caseR1() const { return run_ == STOCK_APP1; }
  uint32_t runLen() const { return runLen_; }
  uint32_t stockAddr() const { return stockAddr_; }    // the other valid app (stock MeshCore), 0 = none
  TableKind table() const { return kind_; }

  // Upload of a bundle (BundleHdr + main + seeder), streamed in order; refuses anything that would touch a protected range.
  bool uploadBegin();
  bool uploadData(uint32_t off, const uint8_t* d, uint32_t len);
  bool uploadEnd();

  bool filesReady();                // both images in place and correct (main has the HELTECV4 tag)
  bool commit();                    // the steps above; true = restart now (the main app boots on probation)
  // At boot: an interrupted commit is finished without the user. 1 = finished (restart), 0 = nothing to resume,
  // -1 = the main app was installed and failed its first start (rolled back here): do not retry by itself
  int resume();
  bool abortToStock();              // before the commit only: boot stock MeshCore again (otadata)

  const char* why() const { return why_; }
  uint32_t uploaded() const { return upOff_; }
  const BundleHdr& bundle() const { return hdr_; }

  // test hooks (host test mutants)
  static bool mutNoOtaFirstR1, mutNoFallback, mutNoMark, mutNoProtect;
  static const int AUTO_RETRIES = 2;     // a main app that fails its first start is installed again at most this often by itself

 private:
  bool fail(const char* w) { why_ = w; return false; }
  bool overlapsProtected(uint32_t a, uint32_t len) const;
  bool eraseAhead(uint32_t base, uint32_t limit, uint32_t& done, uint32_t to);
  bool writeOtadata();
  bool writeTableChecked();
  bool readOta(OtaSel s[2]);
  bool writeMark(uint32_t tries, bool fresh);
  bool readMark(Mark& m);
  bool writeSel(int sector, uint32_t seq, uint32_t state);
  bool prepareSlot();

  Flash& f_;
  const uint8_t* table_;
  uint32_t run_, runLen_ = 0, stockAddr_ = 0, stockLen_ = 0;
  TableKind kind_ = TABLE_OTHER;
  const char* why_ = "";
  BundleHdr hdr_{};
  uint8_t hdrBuf_[sizeof(BundleHdr)]{};
  uint32_t upOff_ = 0, mainErased_ = 0, seedErased_ = 0;
  bool upBad_ = false;
};

}  // namespace mig
