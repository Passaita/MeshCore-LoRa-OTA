#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "Arduino.h"
MockSerial Serial;
namespace mock {
std::vector<uint8_t> flash(16u << 20, 0xFF);
long ops_left = -1, ops = 0; bool our_table = true;
int running = 0, boot = 0, last_invalid = -1, run_state = ESP_OTA_IMG_VALID, boot_sets = 0;
esp_partition_t parts[] = { {ESP_PARTITION_TYPE_APP, 0x10, 0x10000, 0x300000, "app0"}, {ESP_PARTITION_TYPE_APP, 0x11, 0x310000, 0x300000, "app1"},
                            {ESP_PARTITION_TYPE_APP, 0x00, 0x810000, 0x200000, "seeder"}, {ESP_PARTITION_TYPE_DATA, 0x40, 0x610000, 0x200000, "lotaslot"} };
void tick() { ops++; if (ops_left == 0) throw PowerCut{}; if (ops_left > 0) ops_left--; }
}
using namespace mock;
static bool inRange(uint32_t a, uint32_t n) { return a + n <= flash.size(); }
esp_err_t esp_flash_read(esp_flash_t*, void* b, uint32_t a, uint32_t n) { if (!inRange(a, n)) return ESP_FAIL; memcpy(b, &flash[a], n); return ESP_OK; }
esp_err_t esp_flash_write(esp_flash_t*, const void* b, uint32_t a, uint32_t n) {   // NOR: a write only clears bits
  if (!inRange(a, n)) return ESP_FAIL; tick(); const uint8_t* s = (const uint8_t*)b; for (uint32_t i = 0; i < n; i++) flash[a + i] &= s[i]; return ESP_OK; }
esp_err_t esp_flash_erase_region(esp_flash_t*, uint32_t a, uint32_t n) { if (!inRange(a, n) || (a & 0xFFF) || (n & 0xFFF)) return ESP_FAIL; tick(); memset(&flash[a], 0xFF, n); return ESP_OK; }
const esp_partition_t* esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t st, const char* label) {
  for (auto& p : parts) {
    if (!our_table && !strcmp(p.label, "lotaslot")) continue;
    if (p.type == t && (st == ESP_PARTITION_SUBTYPE_ANY || p.subtype == (int)st) && (!label || !strcmp(label, p.label))) return &p;
  }
  return nullptr;
}
esp_err_t esp_partition_erase_range(const esp_partition_t* p, size_t off, size_t n) {
  if (off + n > p->size || ((p->address + off) & 0xFFF)) return ESP_FAIL;
  for (size_t o = 0; o < n; o += 0x1000) { tick(); memset(&flash[p->address + off + o], 0xFF, n - o < 0x1000 ? n - o : 0x1000); }
  return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t* p, size_t off, const void* b, size_t n) {
  if (off + n > p->size) return ESP_FAIL; return esp_flash_write(nullptr, b, p->address + off, n); }
const esp_partition_t* esp_ota_get_running_partition() { return &parts[running]; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &parts[running == 0 ? 1 : 0]; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p) {   // ESP-IDF checks the image: the magic byte stands in for it here
  if (flash[p->address] != 0xE9) return ESP_FAIL; tick(); boot = (int)(p - parts); boot_sets++; return ESP_OK; }
const esp_partition_t* esp_ota_get_last_invalid_partition() { return last_invalid >= 0 ? &parts[last_invalid] : nullptr; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* s) { *s = (esp_ota_img_states_t)run_state; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback() { run_state = ESP_OTA_IMG_VALID; return ESP_OK; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() { throw Restart{}; }
