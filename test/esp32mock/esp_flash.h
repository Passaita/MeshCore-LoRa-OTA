#pragma once
#include "esp32mock.h"
typedef struct esp_flash_t esp_flash_t;
esp_err_t esp_flash_read(esp_flash_t*, void* buf, uint32_t addr, uint32_t len);
esp_err_t esp_flash_write(esp_flash_t*, const void* buf, uint32_t addr, uint32_t len);
esp_err_t esp_flash_erase_region(esp_flash_t*, uint32_t addr, uint32_t len);
