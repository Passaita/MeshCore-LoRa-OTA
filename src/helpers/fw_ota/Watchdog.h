/*
 * nRF52 hardware watchdog for remote repeaters (v1.17.28-rc8, C23).
 *
 * A repeater that hangs (e.g. MeshCore #2283: stuck waiting for a flash operation; #2856: RAK4630/RAK19007 freezing while a solar
 * input above 5.5 V charges a full battery) stays dead until someone power-cycles it. The watchdog restarts it instead.
 *
 * Design adopted from the OffbandMesh MeshCore fork (MIT, src/helpers/NRF52Board.cpp, their #257 / #1083 / #1152: an induced hang
 * reboots the node, seven days without a false trip on a live node):
 *  - started once in setup(), fed from the MAIN LOOP only (and right before the idle sleep), so only a hung loop trips it;
 *  - it keeps counting while the CPU sleeps (CONFIG.SLEEP = Run): a hang inside a wait-for-event loop is caught too. An idle repeater
 *    is not starved: the FreeRTOS tick (1024 Hz) wakes the loop many times a second;
 *  - it pauses while a debugger halts the CPU (CONFIG.HALT = Pause): ST-Link sessions are safe.
 * Our own choice - the TIMEOUT: 10 min (owner, 2026-10-04, rc9; rc8 had 20 h; OffbandMesh uses 30 s; the counter allows ~36 h). It must outlast the LoRa-OTA bootloader's 5-minute
 * Bluetooth DFU inactivity timeout (rev 5+): a started watchdog cannot be stopped and survives a soft reset into the bootloader, which
 * reloads it in its DFU loop, so a quiet wait for the phone (`start ota`, `fw pull-ble`) is never cut short. 10 min leaves a wide margin
 * against false trips (the longest normal blocking step here takes seconds); a frozen remote node is back within 10 min.
 * Side effect: after moving a node to firmware that does not feed it (stock firmware via `start ota`), the watchdog restarts that node
 * once, up to 10 min later; the reset stops it, and the stock firmware then runs normally.
 *
 * (rc10, C27) A running watchdog survives soft resets and its timeout cannot be changed while it runs (the bootloader only feeds it). So a
 * node updated from a build with ANOTHER timeout (rc8: 20 h) kept the old one - and a remote node is rarely power-cycled. start() reads
 * the running CRV; when it is not ours the node stops feeding (it keeps working normally): the old watchdog restarts it once, at most the
 * old timeout after the last feed, and that watchdog reset clears it - the next start() sets ours. No site visit. Hang protection is
 * unchanged meanwhile (a hang restarts it within the same time). Right before steps that must not be cut short (our own restart into the
 * bootloader, the MBR bootloader copy) feedAlways() gives a full period. The planned restart is marked (/fwota.wdd) so it is not counted.
 * `fw wdt` shows it and the reason of the last reset; `fw wdt hang` (dev build only) freezes the loop on purpose to prove it.
 * A watchdog reset is counted in /fwota.wdt (written only when one happened - no flash writes in normal operation).
 */
#pragma once

#include <Arduino.h>
#include <stdio.h>
#if defined(ESP32)
// (Heltec port) the ESP32 task watchdog on the main loop task, same timeout and the same interface as the nRF52 one below. It does not
// survive a reset (the ESP-IDF bootloader resets it), so there is no other firmware's timeout to drain (C27 is nRF52 only). A trip ends in
// a panic reset; esp_reset_reason() then says TASK_WDT. A reset while a new app is on probation returns to the previous app (rollback).
#include <esp_task_wdt.h>
#include <esp_system.h>
#ifndef FW_OTA_WDT_SECS
#define FW_OTA_WDT_SECS 600                   // 10 min (owner, rc9)
#endif
namespace fw_ota {
class Watchdog {
public:
  void start() {
    _reset_reason = (uint32_t)esp_reset_reason();
    esp_task_wdt_init(FW_OTA_WDT_SECS, true);              // re-configures the one Arduino started (timeout, panic = reset)
    esp_task_wdt_add(NULL);                                // the loop task (setup() runs in it)
    _armed = true;
  }
  inline void feed() { if (_armed) esp_task_wdt_reset(); }
  inline void feedAlways() { feed(); }
  bool armed() const { return _armed; }
  bool draining() const { return false; }
  static uint32_t ourCrv() { return FW_OTA_WDT_SECS; }
  uint32_t runningSecs() const { return FW_OTA_WDT_SECS; }
  static const char* secsText(uint32_t secs, char* t) {
    if (secs >= 7200) sprintf(t, "%u h", (unsigned)(secs / 3600));
    else if (secs >= 120) sprintf(t, "%u min", (unsigned)(secs / 60));
    else sprintf(t, "%u s", (unsigned)secs);
    return t;
  }
  static const char* timeoutText() { static char t[12]; return secsText(FW_OTA_WDT_SECS, t); }
  const char* runningText() const { return timeoutText(); }
  bool lastResetWasWatchdog() const {
    return _reset_reason == ESP_RST_TASK_WDT || _reset_reason == ESP_RST_INT_WDT || _reset_reason == ESP_RST_WDT;
  }
  const char* lastResetText() const {
    switch (_reset_reason) {
      case ESP_RST_TASK_WDT: case ESP_RST_INT_WDT: case ESP_RST_WDT: return "watchdog";
      case ESP_RST_PANIC:     return "crash";
      case ESP_RST_BROWNOUT:  return "brownout";
      case ESP_RST_EXT:       return "reset pin";
      case ESP_RST_SW:        return "soft reset";
      case ESP_RST_DEEPSLEEP: return "wake from sleep";
      default:                return "power on";
    }
  }
private:
  bool     _armed = false;
  uint32_t _reset_reason = 0;
};
extern Watchdog g_wdt;
}  // namespace fw_ota
#else
#include <nrf_wdt.h>

#ifndef FW_OTA_WDT_SECS
#define FW_OTA_WDT_SECS 600                   // 10 min (owner, rc9; rc8 had 20 h)
#endif

namespace fw_ota {

class Watchdog {
public:
  void start() {
    _reset_reason = readResetReason();                    // the core's copy of RESETREAS for this boot
    if (nrf_wdt_started(NRF_WDT)) {                       // still running from before a soft reset (its settings cannot be changed now)
      _armed = true; _crv_running = NRF_WDT->CRV;
      _draining = _crv_running != ourCrv();               // (rc10, C27) another firmware's timeout: let it expire once (planned restart)
      feed(); return;
    }
    NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) | (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
    NRF_WDT->CRV    = ourCrv();                           // timeout = (CRV + 1) / 32768 s
    NRF_WDT->RREN   = WDT_RREN_RR0_Msk;
    NRF_WDT->TASKS_START = 1;                             // irreversible until the next reset
    _armed = true; _crv_running = ourCrv(); _draining = false;
  }
  inline void feed() { if (_armed && !_draining) nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0); }   // the main loop
  inline void feedAlways() { if (_armed) nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0); }          // before steps that must not be cut short
  bool armed() const { return _armed; }
  bool draining() const { return _draining; }            // (C27) another firmware's timeout runs: one planned restart pending
  static uint32_t ourCrv() { return (uint32_t)FW_OTA_WDT_SECS * 32768UL - 1UL; }
  uint32_t runningSecs() const { return (_crv_running + 1UL) / 32768UL; }
  static const char* secsText(uint32_t secs, char* t) {  // "20 h" / "10 min" / "45 s"
    if (secs >= 7200) sprintf(t, "%u h", (unsigned)(secs / 3600));
    else if (secs >= 120) sprintf(t, "%u min", (unsigned)(secs / 60));
    else sprintf(t, "%u s", (unsigned)secs);
    return t;
  }
  static const char* timeoutText() { static char t[12]; return secsText(FW_OTA_WDT_SECS, t); }   // ours (compiled in)
  const char* runningText() const { static char t[12]; return secsText(runningSecs(), t); }     // the one counting now
  bool lastResetWasWatchdog() const { return (_reset_reason & POWER_RESETREAS_DOG_Msk) != 0; }
  const char* lastResetText() const {
    if (_reset_reason & POWER_RESETREAS_DOG_Msk)    return "watchdog";
    if (_reset_reason & POWER_RESETREAS_LOCKUP_Msk) return "CPU lockup";
    if (_reset_reason & POWER_RESETREAS_RESETPIN_Msk) return "reset pin";
    if (_reset_reason & POWER_RESETREAS_SREQ_Msk)   return "soft reset";
    if (_reset_reason & POWER_RESETREAS_OFF_Msk)    return "wake from off";
    return "power on";
  }
private:
  bool     _armed = false;
  bool     _draining = false;
  uint32_t _crv_running = 0;
  uint32_t _reset_reason = 0;
};

extern Watchdog g_wdt;

}  // namespace fw_ota
#endif  // ESP32
