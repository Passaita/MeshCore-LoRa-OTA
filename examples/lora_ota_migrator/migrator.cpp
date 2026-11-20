// LoRa-OTA migrator for the Heltec V4: moves a node from stock MeshCore to LoRa-OTA over Wi-Fi, from a phone, without USB (a roof node
// stays on the roof). Design, safety and test results: docs/lora-ota/HELTEC-V4-MIGRATOR.md.
//
// How a user runs it, next to the node with a phone, no USB:
//   1. stock MeshCore: `start ota` -> its Wi-Fi update page -> upload this migrator (.bin). Stock MeshCore restarts into it.
//   2. Wi-Fi "LoRa-OTA-MIGRATE-<id>" (open), http://192.168.4.1: upload the release's migration bundle (-migrate.bin = LoRa-OTA app +
//      seeder), the page checks both images.
//   3. tick "I'm next to the node and its power is stable" -> Migrate. The node rewrites its partition table (the one step with no
//      second copy, well under a second) and restarts into LoRa-OTA on probation (2 min + a packet heard, else it rolls back).
// Identity, settings and contacts (spiffs 0xC90000) and nvs are never written. Before step 3 "Cancel" (or 15 min idle) = stock again.
// A power cut during the migration resumes it at the next start (intent record); only a cut inside the table sector needs USB.
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <esp_ota_ops.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_system.h>
#include "MigrateEngine.h"
#include "new_table.h"                  // NEW_TABLE_USED: boards/lora_ota_esp32s3_16MB.csv through gen_esp32part.py

static const uint32_t IDLE_MS = 15UL * 60000UL;

// ---------------------------------------------------------------------------------------------------------------------------------
// Flash for the engine. ESP-IDF's own guard (region_protected) refuses the bootloader + table AND the whole running partition as the
// table describes it - stock app1 is 0x650000-0xC90000, so the seeder at 0x810000 (R1) or the main app at 0x310000 (R0, app0
// 0x10000-0x650000) would abort() (bench 2026-11-19, bench Heltec: abort in esp_flash_erase_region at the first seeder sector). All erases
// and writes therefore go through a copy of the default chip without that check, and this driver + the engine do the guarding
// instead: nothing below 0x10000 but the two otadata sectors (the table only via writeTable), nothing from DATA_START up, and the
// engine refuses its own image and the kept stock image (overlapsProtected).
static esp_flash_t g_chip;
static esp_flash_os_functions_t g_fn;
static esp_flash_t* chip() {
  static bool init = false;
  if (!init) { g_chip = *esp_flash_default_chip; g_fn = *g_chip.os_func; g_fn.region_protected = NULL; g_chip.os_func = &g_fn; init = true; }
  return &g_chip;
}
static bool inOtadata(uint32_t a, uint32_t l) { return a >= mig::OTADATA_ADDR && a + l <= mig::OTADATA_ADDR + 2 * mig::SECTOR; }

struct EspFlash : mig::Flash {
  bool read(uint32_t a, void* d, uint32_t l) override { return esp_flash_read(NULL, d, a, l) == ESP_OK; }
  bool erase(uint32_t a, uint32_t l) override {
    if ((a & (mig::SECTOR - 1)) || (l & (mig::SECTOR - 1)) || l == 0) return false;
    if (a < 0x10000 && !inOtadata(a, l)) return false;
    if (a + l > mig::DATA_START) return false;
    for (uint32_t o = 0; o < l; o += mig::SECTOR) { if (esp_flash_erase_region(chip(), a + o, mig::SECTOR) != ESP_OK) return false; if (!(o & 0xFFFF)) yield(); }
    return true;
  }
  bool write(uint32_t a, const void* s, uint32_t l) override {
    if (a < 0x10000 && !inOtadata(a, l)) return false;
    if (a + l > mig::DATA_START) return false;
    return esp_flash_write(chip(), s, a, l) == ESP_OK;
  }
  bool writeTable(const void* s, uint32_t l) override {
    if (l == 0 || l > mig::TABLE_LEN || (l & 31)) return false;   // the entries + the MD5 entry (the rest stays 0xFF)
    // erase + the first 256-byte page (all 8 entries) is the window; the MD5 entry is in the second page (a table without it boots)
    if (esp_flash_erase_region(chip(), mig::TABLE_ADDR, mig::SECTOR) != ESP_OK) return false;
    return esp_flash_write(chip(), s, mig::TABLE_ADDR, l) == ESP_OK;
  }
};

static EspFlash g_flash;
static mig::Engine* g_eng = nullptr;
static volatile uint32_t g_last_ms = 0;
static volatile bool g_commit_req = false, g_abort_req = false;
static char g_msg[160] = "";
static bool g_ready = false, g_failed = false;

static void say(const char* m) { snprintf(g_msg, sizeof(g_msg), "%s", m); Serial.printf("migrator: %s\n", m); }

static String page() {
  String s = F("<!doctype html><meta name=viewport content='width=device-width'><title>LoRa-OTA migrator</title>"
               "<body style='font-family:sans-serif;max-width:32em;margin:2em auto;padding:0 1em'><h2>LoRa-OTA migrator</h2>");
  char b[200];
  snprintf(b, sizeof(b), "<p>Board: Heltec V4. Running from 0x%06lx (%s). Stock MeshCore kept: %s.</p>", (unsigned long)esp_ota_get_running_partition()->address,
           g_eng->table() == mig::TABLE_STOCK ? "stock layout" : "LoRa-OTA layout", g_eng->stockAddr() ? "yes" : "no");
  s += b;
  s += "<p><b>Status:</b> "; s += g_msg; s += "</p>";
  if (g_failed) s += F("<p>The LoRa-OTA app did not start correctly and the node came back here. Upload the bundle again or retry.</p>");
  s += F("<h3>1. Upload</h3><p>The release's <b>-migrate.bin</b> (LoRa-OTA app + Wi-Fi seeder for HELTECV4).</p>"
         "<form method=POST action=/upload enctype=multipart/form-data><input type=file name=f accept=.bin> <input type=submit value=Upload></form>");
  if (g_ready)
    s += F("<h3>2. Migrate</h3><form method=POST action=/commit><label><input type=checkbox name=ok value=1> I'm next to the node and its "
           "power is stable (the partition table is rewritten - under a second without a second copy)</label><br><br>"
           "<input type=submit value=Migrate></form>");
  if (g_eng->stockAddr()) s += F("<p><a href=/cancel>Cancel - back to stock MeshCore</a></p>");
  return s;
}

static AsyncWebServer server(80);

static void onUpload(AsyncWebServerRequest*, const String&, size_t index, uint8_t* data, size_t len, bool final) {
  g_last_ms = millis();
  if (index == 0) { g_ready = false; if (!g_eng->uploadBegin()) { say(g_eng->why()); return; } say("receiving"); }
  if (!g_eng->uploadData(index, data, len)) { say(g_eng->why()); return; }
  if (final) {
    if (g_eng->uploadEnd()) { g_ready = true; snprintf(g_msg, sizeof(g_msg), "bundle checked: fw-%lu app + seeder in place - ready to migrate", (unsigned long)g_eng->bundle().fw); }
    else say(g_eng->why());
  }
}

void setup() {
  Serial.begin(115200); delay(300);
  esp_ota_mark_app_valid_cancel_rollback();               // the stock updater started us on trial: we run, so stay
  const esp_partition_t* run = esp_ota_get_running_partition();
  static uint8_t table[mig::TABLE_LEN];
  memset(table, 0xFF, sizeof(table)); memcpy(table, NEW_TABLE_USED, NEW_TABLE_USED_LEN);
  static mig::Engine eng(g_flash, table, run->address);
  g_eng = &eng;
  if (!eng.plan()) { say(eng.why()); }
  else {
    const int r = eng.resume();                            // an interrupted migration is finished without the user
    if (r == 1) { say("migration finished after a restart - starting LoRa-OTA"); delay(500); esp_restart(); }
    if (r == -1) { g_failed = true; say("LoRa-OTA app failed its first start"); }
    else if (eng.filesReady()) { g_ready = true; say("files from an earlier upload are in place - ready to migrate"); }
    else say("waiting for the bundle");
  }
  uint8_t mac[6]; WiFi.mode(WIFI_AP); WiFi.macAddress(mac);
  char ssid[32]; snprintf(ssid, sizeof(ssid), "LoRa-OTA-MIGRATE-%02X%02X%02X", mac[3], mac[4], mac[5]);
  WiFi.softAP(ssid);
  Serial.printf("migrator: Wi-Fi '%s', http://%s\n", ssid, WiFi.softAPIP().toString().c_str());
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { g_last_ms = millis(); r->send(200, "text/html", page()); });
  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest* r) { r->redirect("/"); }, onUpload);
  server.on("/commit", HTTP_POST, [](AsyncWebServerRequest* r) {
    g_last_ms = millis();
    if (!r->hasParam("ok", true)) { say("tick the box first"); r->redirect("/"); return; }
    g_commit_req = true;
    r->send(200, "text/plain", "Migrating. The node restarts into LoRa-OTA in a few seconds; this Wi-Fi goes away. "
                               "After ~2 min it confirms itself (it must hear a packet); check it with 'ver' and 'fw slot'.");
  });
  server.on("/cancel", HTTP_GET, [](AsyncWebServerRequest* r) { g_abort_req = true; r->send(200, "text/plain", "back to stock MeshCore"); });
  server.begin();
  g_last_ms = millis();
}

void loop() {
  if (g_commit_req) {
    g_commit_req = false;
    delay(1500);                                          // the reply leaves first
    if (g_eng->commit()) { say("migrated - restarting"); delay(300); esp_restart(); }
    say(g_eng->why());
  }
  if (g_abort_req || (millis() - g_last_ms > IDLE_MS && !g_failed)) {
    g_abort_req = false;
    if (g_eng->stockAddr() && g_eng->abortToStock()) { say("back to stock MeshCore"); delay(1500); esp_restart(); }
    g_last_ms = millis();                                 // nothing to go back to: stay here
  }
  delay(50);
}
