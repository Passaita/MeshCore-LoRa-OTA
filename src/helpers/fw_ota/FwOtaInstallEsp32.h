// (Heltec port) the ESP32 install + probation step (FwOtaInstallEsp32.cpp)
#pragma once
#include <stdint.h>
#include "FwOtaSlots.h"
namespace fw_ota {
static const uint32_t PROBATION_MIN_MS = 120000;           // the loop ran 2 min ...
static const uint32_t PROBATION_MAX_MS = 3UL * 3600000UL;  // ... and a packet came within 3 h, else back to the previous app
void bootApplyEsp32();                                     // at boot: act on a PENDING control record (the RAK bootloader's job)
bool runningSlotImage(const SlotHeader& h);                // the running app partition holds exactly the slot's image
bool onProbation();
void confirmApp();                                         // mark the running app valid now (back from the seeder)
void probationTick(uint32_t uptime_ms, uint32_t rx_packets);
}
