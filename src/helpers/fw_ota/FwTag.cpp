// The firmware tag the fw-pull bootloader requires (see FwOtaSlots.h). Kept in its own file so that a build without the fw_ota code
// - e.g. stock firmware that only has to be FLASHABLE through this bootloader - can add just these 16 bytes.
// `used` + KEEP() in the linker script keep it although nothing references it; the section places it right after the vector table.
#include "FwOtaSlots.h"

namespace fw_ota {
#if defined(ESP32)
// (Heltec port) no LoRa-OTA bootloader and no fixed place: the installer searches the unpacked image for these 16 bytes (rodata).
// "HELTECV4" fills the 8-byte field (no NUL): spelled out, a C++ string literal would not fit.
__attribute__((used, aligned(4)))
extern const FwTag g_fwpull_tag = { TAG_MAGIC, (uint16_t)TAG_VERSION, 0, {'H', 'E', 'L', 'T', 'E', 'C', 'V', '4'} };
#else
__attribute__((section(".fwpull_tag"), used, aligned(4)))
extern const FwTag g_fwpull_tag = { TAG_MAGIC, (uint16_t)TAG_VERSION, 0, FW_OTA_TAG_BOARD };
#endif
}  // namespace fw_ota
