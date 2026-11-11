// LoRa-OTA seeder for ESP32 boards (Heltec V4, owner 2026-11-01: "Having the necessity to connect one board to laptop USB isn't
// convenient"). It lives in the "factory" app partition (boards/lora_ota_esp32s3_16MB.csv), written once by the USB install and never
// sent over LoRa, so the repeater image stays small (airtime). The repeater's `fw pull-wifi` restarts into it:
//   - an OPEN Wi-Fi access point "LoRa-OTA-<id>" (owner 2026-11-01: only a logged-in admin can start it, 5 min at most, and every upload
//     is checked by the app like any slot image - board, CRC-32, SHA-256);
//   - http://192.168.4.1 takes the release's .lota and writes it into the slot partition exactly where a RAK -slot.zip lands
//     (the .lota header ends at the slot's first page, the payload sits at the slot payload offset);
//   - then it returns to the app partition it came from; the app's slotFromLota() checks the image (board, CRC-32, SHA-256) and serves it.
// Never written here: the app partitions, the MeshCore files (identity, settings), the slot's control sector. 5 min idle = back to the app.
// Without /fwota.seed (the bootloader started it because no app is valid) it only reports that on the serial console and returns.
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static const uint32_t SLOT_PART_SIZE = 0x200000, CTRL_SIZE = 0x1000, PAYLOAD_OFF = 0x1000, LOTA_HDR = 160;
static const uint32_t LOTA_AT = PAYLOAD_OFF - LOTA_HDR;                 // partition offset of the .lota header
static const uint32_t SEED_MAGIC = 0x44454553;                          // "SEED"
static const uint32_t IDLE_MS = 5UL * 60000UL;

static const esp_partition_t* g_slot = nullptr;
static uint32_t g_ret = 0;
static volatile uint32_t g_last_ms = 0, g_back_at = 0;
static uint32_t g_off = 0, g_expect = 0, g_erased_to = 0;
static bool g_bad = false;
static uint8_t g_hdr[LOTA_HDR];
static char g_msg[120] = "no upload yet";

static void backToApp(const char* why) {
  Serial.printf("seeder: %s - back to the app\n", why); Serial.flush();
  const esp_partition_t* p = nullptr;                               // the app partition `fw pull-wifi` came from
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (; it; it = esp_partition_next(it)) { const esp_partition_t* q = esp_partition_get(it); if (q->address == g_ret) p = q; }
  if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (p) esp_ota_set_boot_partition(p);
  delay(200); esp_restart();
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// erase the sectors up to `end` (partition offset) before they are written; sector 0 first, so an old slot header is gone at once
static bool eraseTo(uint32_t end) {
  while (g_erased_to < end) {
    if (esp_partition_erase_range(g_slot, g_erased_to, 0x1000) != ESP_OK) return false;
    g_erased_to += 0x1000;
  }
  return true;
}

static void onUpload(AsyncWebServerRequest* req, const String& fn, size_t index, uint8_t* data, size_t len, bool final) {
  g_last_ms = millis();
  if (index == 0) { g_off = 0; g_expect = 0; g_bad = false; g_erased_to = 0; strcpy(g_msg, "receiving"); }
  if (g_bad) return;
  for (size_t i = 0; i < len && g_off + i < LOTA_HDR; i++) g_hdr[g_off + i] = data[i];
  if (!g_expect && g_off + len >= LOTA_HDR) {                       // the header is in: check it before anything is kept
    const char* why = nullptr;
    const uint32_t plen = le32(g_hdr + 20);
    if (memcmp(g_hdr, "LOTA", 4) != 0) why = "not a .lota file";
    else if (g_hdr[6] != 0) why = "not an app image";
    else if (memcmp(g_hdr + 8, "HELTECV4", 8) != 0) why = "for another board";
    else if (!plen || LOTA_AT + LOTA_HDR + plen > SLOT_PART_SIZE - 2 * CTRL_SIZE) why = "too big for the slot";
    if (why) { g_bad = true; snprintf(g_msg, sizeof(g_msg), "refused: %s", why); return; }
    g_expect = LOTA_HDR + plen;
  }
  if (g_expect && g_off + len > g_expect) { g_bad = true; strcpy(g_msg, "refused: longer than its header says"); return; }
  const uint32_t at = LOTA_AT + g_off;
  if (!eraseTo((at + len + 0xFFF) & ~0xFFFu) || esp_partition_write(g_slot, at, data, len) != ESP_OK) { g_bad = true; strcpy(g_msg, "flash write failed"); return; }
  g_off += len;
  if (final) {
    if (g_expect && g_off == g_expect) {
      snprintf(g_msg, sizeof(g_msg), "OK - fw-%lu written (%lu B); the node restarts, checks it and serves it", (unsigned long)le32(g_hdr + 16), (unsigned long)g_off);
      g_back_at = millis() + 3000;
    } else { g_bad = true; snprintf(g_msg, sizeof(g_msg), "refused: incomplete (%lu of %lu B)", (unsigned long)g_off, (unsigned long)g_expect); }
  }
}

static AsyncWebServer server(80);
static const char PAGE[] =
  "<!doctype html><meta name=viewport content='width=device-width'><title>LoRa-OTA seeder</title>"
  "<body style='font-family:sans-serif;max-width:30em;margin:2em auto;padding:0 1em'><h2>LoRa-OTA seeder</h2>"
  "<p>Choose the release's <b>.lota</b> file for this board (HELTECV4). It goes into the slot only; the running app is not changed.</p>"
  "<form method=POST action=/upload enctype=multipart/form-data><input type=file name=f accept=.lota> <input type=submit value=Upload></form>"
  "<p><a href=/back>Cancel - back to the repeater</a></p>";

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("seeder: LoRa-OTA seeder (factory partition)");
  g_slot = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "lotaslot");
  bool seed = false;
  if (SPIFFS.begin(false)) {                                          // /fwota.seed: written by `fw pull-wifi` (magic + the app to return to)
    File f = SPIFFS.open("/fwota.seed");
    uint8_t b[8] = {0};
    if (f && f.read(b, sizeof(b)) == sizeof(b) && le32(b) == SEED_MAGIC) { g_ret = le32(b + 4); seed = true; }
    if (f) f.close();
  }
  if (!seed || !g_slot) {                           // started by the bootloader (no valid app) or no slot: nothing safe to do here
    Serial.println(seed ? "seeder: no slot partition" : "seeder: not started by 'fw pull-wifi' (no valid app?) - reinstall by USB");
    delay(2000); backToApp("nothing to do");
  }
  uint8_t mac[6]; WiFi.mode(WIFI_AP); WiFi.macAddress(mac);
  char ssid[24]; snprintf(ssid, sizeof(ssid), "LoRa-OTA-%02X%02X%02X", mac[3], mac[4], mac[5]);
  WiFi.softAP(ssid);                                                  // open (see above)
  Serial.printf("seeder: Wi-Fi '%s' up, http://%s\n", ssid, WiFi.softAPIP().toString().c_str());
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { g_last_ms = millis(); r->send(200, "text/html", PAGE); });
  server.on("/back", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "back to the repeater"); g_back_at = millis() + 1000; });
  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest* r) { r->send(g_bad ? 400 : 200, "text/plain", g_msg); }, onUpload);
  server.begin();
  g_last_ms = millis();
}

void loop() {
  if (g_back_at && (int32_t)(millis() - g_back_at) >= 0) backToApp(g_msg);
  if (millis() - g_last_ms > IDLE_MS) backToApp("5 min without an upload");
  delay(50);
}
