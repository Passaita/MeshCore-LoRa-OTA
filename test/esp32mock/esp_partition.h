#pragma once
#include "esp32mock.h"
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_APP_FACTORY = 0, ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10, ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
               ESP_PARTITION_SUBTYPE_ANY = 0xff } esp_partition_subtype_t;
typedef struct { esp_partition_type_t type; int subtype; uint32_t address; uint32_t size; char label[17]; } esp_partition_t;
typedef struct esp_partition_iterator_opaque* esp_partition_iterator_t;
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char* label);
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t off, size_t len);
esp_err_t esp_partition_write(const esp_partition_t*, size_t off, const void* src, size_t len);
namespace mock { extern esp_partition_t parts[]; }
