#include "helpers/PowerSaving.h"

#if defined(ESP32_PLATFORM)
// PowerSaving for companions
void PowerSaving::applyCompanionPowerSaving(bool powersaving_enabled) {
  if (powersaving_enabled) {
    enableCompanionPowerSaving();
  } else {
    disableCompanionPowerSaving();
  }
}

void PowerSaving::enableCompanionPowerSaving() {
#if defined(BLE_PIN_CODE) && !CONFIG_IDF_TARGET_ESP32C6
  // Enable BLE sleep
  esp_bt_sleep_enable();
#endif

  // Configure Power Management
#if defined(BLE_PIN_CODE)
  configurePowerManagement(80, 10, false);
#endif
}

void PowerSaving::disableCompanionPowerSaving() {
#if defined(BLE_PIN_CODE) && !CONFIG_IDF_TARGET_ESP32C6
  // Disable BLE sleep
  esp_bt_sleep_disable();
#endif

  // Configure Power Management
#if defined(ESP32_CPU_FREQ)
  configurePowerManagement(ESP32_CPU_FREQ, ESP32_CPU_FREQ, false);
#else
  configurePowerManagement(80, 80, false);
#endif
}

esp_err_t PowerSaving::configurePowerManagement(int max_freq_mhz, int min_freq_mhz, bool light_sleep_enable) {
  // Configure Power Management
#if CONFIG_IDF_TARGET_ESP32C3
  esp_pm_config_esp32c3_t pm_config;
#elif CONFIG_IDF_TARGET_ESP32S3
  esp_pm_config_esp32s3_t pm_config;
#elif CONFIG_IDF_TARGET_ESP32
  esp_pm_config_esp32_t pm_config;
#elif CONFIG_IDF_TARGET_ESP32C6
  esp_pm_config_t pm_config;
#endif

  // Set PM Config
  pm_config = { .max_freq_mhz = max_freq_mhz, .min_freq_mhz = min_freq_mhz, .light_sleep_enable = light_sleep_enable };
  esp_err_t errPM = esp_pm_configure(&pm_config);
  if (errPM == ESP_OK) {
    POWERSAVING_DEBUG_PRINTLN("Power Management configured successfully");
  } else {
    POWERSAVING_DEBUG_PRINTLN("Power Management failed to configure: %d\r\n", errPM);
  }

  return errPM;
}
#endif