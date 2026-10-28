#pragma once
#include <cstdint>
inline uint32_t sd_power_gpregret_clr(uint32_t, uint32_t) { return 0; }
inline uint32_t sd_power_gpregret_set(uint32_t, uint32_t) { return 0; }
#define NRF_SUCCESS 0
uint32_t sd_flash_write(uint32_t* dst, const uint32_t* src, uint32_t words);   // sim: NOR write into the node's flash
