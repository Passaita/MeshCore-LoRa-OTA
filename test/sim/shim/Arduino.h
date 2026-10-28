#pragma once
// LoRa-OTA network simulator: Arduino / nRF52 surface for running the REAL firmware sources on a PC (one process, many virtual nodes).
// Everything a node owns (clock offset, flash, NRF registers, watchdog) is reached through the sim_cur_* pointers that the scheduler
// points at the node it runs.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <algorithm>
#include "Stream.h"
using std::isnan;

extern uint32_t g_sim_now;                       // the virtual clock (ms)
extern uint32_t g_sim_boot;                      // the running node's boot time: millis() restarts at 0 on every boot, like the device
inline uint32_t millis() { return g_sim_now - g_sim_boot; }
inline unsigned long micros() { return (unsigned long)millis() * 1000UL; }
inline void delay(uint32_t) {}                   // no busy wait in the simulator
inline void yield() {}
#ifndef F
#define F(x) x
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

struct SimSerial {
  bool quiet = true;
  void begin(unsigned long) {}
  size_t print(const char* s) { if (!quiet) fputs(s, stdout); return strlen(s); }
  size_t print(unsigned long v) { if (!quiet) printf("%lu", v); return 1; }
  size_t print(long v) { if (!quiet) printf("%ld", v); return 1; }
  size_t print(int v) { if (!quiet) printf("%d", v); return 1; }
  size_t print(unsigned v) { if (!quiet) printf("%u", v); return 1; }
  size_t println(const char* s = "") { if (!quiet) printf("%s\n", s); return strlen(s) + 1; }
  size_t println(unsigned long v) { if (!quiet) printf("%lu\n", v); return 1; }
  size_t println(long v) { if (!quiet) printf("%ld\n", v); return 1; }
  size_t println(int v) { if (!quiet) printf("%d\n", v); return 1; }
  size_t println(unsigned v) { if (!quiet) printf("%u\n", v); return 1; }
  size_t printf(const char* f, ...) { if (quiet) return 0; va_list a; va_start(a, f); int n = vprintf(f, a); va_end(a); return n; }
  void flush() {}
  int available() { return 0; }
  int read() { return -1; }
  size_t readBytes(void*, size_t) { return 0; }
  size_t write(const uint8_t*, size_t n) { return n; }
  size_t write(uint8_t) { return 1; }
  operator bool() const { return true; }
};
extern SimSerial Serial;

// ---- nRF52 registers a node owns ----
struct SimNrfPower { uint32_t GPREGRET; uint32_t RESETREAS; uint32_t USBREGSTATUS; };   // (rc19) bit 0 = VBUS (USB power) present
struct SimNrfUicr { uint32_t NRFFW[15]; };
struct WdtRegs { uint32_t CONFIG, CRV, RREN, TASKS_START, RR[8], RUNSTATUS; };
extern SimNrfPower* sim_cur_power;
extern SimNrfUicr*  sim_cur_uicr;
extern WdtRegs*     sim_cur_wdt_regs;
#define NRF_POWER sim_cur_power
#define NRF_UICR  sim_cur_uicr
#define NRF_WDT   sim_cur_wdt_regs
#define WDT_CONFIG_SLEEP_Pos 0
#define WDT_CONFIG_SLEEP_Run 1
#define WDT_CONFIG_HALT_Pos 3
#define WDT_CONFIG_HALT_Pause 0
#define WDT_RREN_RR0_Msk 1
#define POWER_RESETREAS_RESETPIN_Msk 0x1
#define POWER_RESETREAS_DOG_Msk 0x2
#define POWER_RESETREAS_SREQ_Msk 0x4
#define POWER_RESETREAS_LOCKUP_Msk 0x8
#define POWER_RESETREAS_OFF_Msk 0x10000
uint32_t readResetReason();
struct SimReset { int why; };                     // NVIC_SystemReset() never returns: the scheduler catches this and reboots the node
[[noreturn]] void NVIC_SystemReset();
