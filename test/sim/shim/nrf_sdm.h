#pragma once
#include <cstdint>
inline uint32_t sd_softdevice_is_enabled(uint8_t* e) { *e = 0; return 0; }
inline uint32_t sd_softdevice_disable() { return 0; }
