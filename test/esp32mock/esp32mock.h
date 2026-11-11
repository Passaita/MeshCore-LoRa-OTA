// (Heltec port) host mocks for the ESP32 LoRa-OTA flash + install code (test/fw_ota_esp32_install_test.sh): 16 MB NOR flash, the LoRa-OTA
// partition table, ESP-IDF OTA state, and a power cut after N flash operations (throws PowerCut)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <vector>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
struct PowerCut {};
struct Restart {};
namespace mock {
extern std::vector<uint8_t> flash;
extern long ops_left;                 // flash operations until the power cut (-1 = never)
extern long ops;                      // operations done
extern bool our_table;                // false = stock partitions (no lotaslot)
extern int running, boot, last_invalid;   // partition index: 0 = app0, 1 = app1, 2 = factory; -1 = none
extern int run_state;                 // esp_ota_img_states_t of the running app
extern int boot_sets;
void tick();
}
