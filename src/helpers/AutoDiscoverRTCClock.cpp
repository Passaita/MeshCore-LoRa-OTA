#include "AutoDiscoverRTCClock.h"
#include "RTClib.h"

#ifdef NO_EXTERNAL_RTC
// RAK WisMesh 1W Booster has no RTC chip - always use the software fallback clock.
// Drops the Melopero RV3028 / RX8130 / DS3231 / PCF8563 drivers + I2C probing.
void AutoDiscoverRTCClock::begin(TwoWire& wire) { (void)wire; }
uint32_t AutoDiscoverRTCClock::getCurrentTime() { return _fallback->getCurrentTime(); }
void AutoDiscoverRTCClock::setCurrentTime(uint32_t time) { _fallback->setCurrentTime(time); }
bool AutoDiscoverRTCClock::i2c_probe(TwoWire& wire, uint8_t addr) { (void)wire; (void)addr; return false; }
#elif defined(RTC_RV3028_ONLY)
// RV-3028 only (RAK12002 WisBlock RTC, I2C 0x52) - keeps the DS3231 / PCF8563 / RX8130CE drivers out of small builds.
// The chip is NOT read on every getCurrentTime(): the software clock stays the single time source, and the chip only
//  - sets it once at boot (if the chip holds a plausible time, i.e. it did not lose power),
//  - re-disciplines it once an hour (the MCU clock drifts by seconds per day),
//  - is written whenever the time is set ('time', 'clock sync').
// Without the module nothing changes: same binary, software clock.
#include <Melopero_RV3028.h>
#include "RTClib.h"

static Melopero_RV3028 rtc_rv3028;
static bool rv3028_success = false;
static uint32_t rv3028_last_sync_ms = 0;
static const uint32_t RV3028_MIN_VALID = 1735689600UL;   // 2025-01-01: an unset/power-lost chip reads year 2000
static const uint32_t RV3028_RESYNC_MS = 3600UL * 1000UL;

static uint32_t rv3028_read() {
  return DateTime(rtc_rv3028.getYear(), rtc_rv3028.getMonth(), rtc_rv3028.getDate(),
                  rtc_rv3028.getHour(), rtc_rv3028.getMinute(), rtc_rv3028.getSecond()).unixtime();
}

// The time handed out never steps BACKWARDS on a resync (peers treat older timestamps as replays): if the software clock has
// run ahead of the chip by d seconds it is frozen for d seconds instead ("hold"); if it is behind it is stepped forward.
// Only an explicit set ('time' on the local console) can move it back.
static uint32_t rv_hold = 0, rv_last_raw = 0, rv_last_out = 0;

static void rv_restart(mesh::RTCClock* fb, uint32_t out) { rv_last_raw = fb->getCurrentTime(); rv_last_out = out; rv_hold = 0; }

static uint32_t rv_now(mesh::RTCClock* fb) {
  uint32_t raw = fb->getCurrentTime();
  uint32_t elapsed = raw - rv_last_raw;
  rv_last_raw = raw;
  uint32_t eat = elapsed < rv_hold ? elapsed : rv_hold;
  rv_hold -= eat;
  rv_last_out += elapsed - eat;
  return rv_last_out;
}

static void rv3028_sync(mesh::RTCClock* fb) {
  rv3028_last_sync_ms = millis();
  uint32_t c = rv3028_read();
  if (c < RV3028_MIN_VALID) return;                 // chip lost power / never set: keep the software clock
  uint32_t out = rv_now(fb);
  if (c > out) { fb->setCurrentTime(c); rv_restart(fb, c); }
  else rv_hold = out - c;
}

bool AutoDiscoverRTCClock::i2c_probe(TwoWire& wire, uint8_t addr) {
  wire.beginTransmission(addr);
  return wire.endTransmission() == 0;
}

void AutoDiscoverRTCClock::begin(TwoWire& wire) {
  if (!i2c_probe(wire, 0x52)) return;
  rtc_rv3028.initI2C(wire);
  rtc_rv3028.writeToRegister(0x35, 0x00);
  rtc_rv3028.writeToRegister(0x37, 0xB4);   // direct switching mode + trickle charge of the RAK12002 supercap
  rtc_rv3028.set24HourMode();
  rv3028_success = true;
  rv_restart(_fallback, _fallback->getCurrentTime());
  rv3028_sync(_fallback);
}

uint32_t AutoDiscoverRTCClock::getCurrentTime() {
  if (!rv3028_success) return _fallback->getCurrentTime();
  if (millis() - rv3028_last_sync_ms > RV3028_RESYNC_MS) rv3028_sync(_fallback);
  return rv_now(_fallback);
}

void AutoDiscoverRTCClock::setCurrentTime(uint32_t time) {
  _fallback->setCurrentTime(time);
  if (!rv3028_success) return;
  rv_restart(_fallback, time);
  DateTime dt(time);
  uint8_t weekday = (dt.day() + (uint16_t)((2.6 * dt.month()) - 0.2) - (2 * (dt.year() / 100)) + dt.year() + (uint16_t)(dt.year() / 4) + (uint16_t)(dt.year() / 400)) % 7;
  rtc_rv3028.setTime(dt.year(), dt.month(), weekday, dt.day(), dt.hour(), dt.minute(), dt.second());
  rv3028_last_sync_ms = millis();
}
#else

#include <Melopero_RV3028.h>
#include "RTC_RX8130CE.h"

static RTC_DS3231 rtc_3231;
static bool ds3231_success = false;

static Melopero_RV3028 rtc_rv3028;
static bool rv3028_success = false;

static RTC_PCF8563 rtc_8563;
static bool rtc_8563_success = false;

static RTC_RX8130CE rtc_8130;
static bool rtc_8130_success = false;

#define DS3231_ADDRESS   0x68
#define RV3028_ADDRESS   0x52
#define PCF8563_ADDRESS  0x51
#define RX8130CE_ADDRESS 0x32

bool AutoDiscoverRTCClock::i2c_probe(TwoWire& wire, uint8_t addr) {
  wire.beginTransmission(addr);
  uint8_t error = wire.endTransmission();
  return (error == 0);
}

void AutoDiscoverRTCClock::begin(TwoWire& wire) {
  #if !defined(DISABLE_DS3231_PROBE)
  if (i2c_probe(wire, DS3231_ADDRESS)) {
    ds3231_success = rtc_3231.begin(&wire);
  }
  #endif

  if (i2c_probe(wire, RV3028_ADDRESS)) {
    rtc_rv3028.initI2C(wire);
    rtc_rv3028.writeToRegister(0x35, 0x00);
    rtc_rv3028.writeToRegister(0x37, 0xB4); // Direct Switching Mode (DSM): when VDD < VBACKUP, switchover occurs from VDD to VBACKUP
    rtc_rv3028.set24HourMode(); // Set the device to use the 24hour format (default) instead of the 12 hour format
    rv3028_success = true;
  }

  if (i2c_probe(wire, PCF8563_ADDRESS)) {
    MESH_DEBUG_PRINTLN("PCF8563: Found");
    rtc_8563_success = rtc_8563.begin(&wire);
  }

  if (i2c_probe(wire, RX8130CE_ADDRESS)) {
    MESH_DEBUG_PRINTLN("RX8130CE: Found");
    rtc_8130.begin(&wire);
    rtc_8130_success = true;
    MESH_DEBUG_PRINTLN("RX8130CE: Initialized");
  }
}

uint32_t AutoDiscoverRTCClock::getCurrentTime() {
  if (ds3231_success) {
    return rtc_3231.now().unixtime();
  }

  if (rv3028_success) {
    return DateTime(
        rtc_rv3028.getYear(),
        rtc_rv3028.getMonth(),
        rtc_rv3028.getDate(),
        rtc_rv3028.getHour(),
        rtc_rv3028.getMinute(),
        rtc_rv3028.getSecond()
    ).unixtime();
  }

  if (rtc_8563_success) {
    return rtc_8563.now().unixtime();
  }

  if (rtc_8130_success) {
    MESH_DEBUG_PRINTLN("RX8130CE: Reading time");
    return rtc_8130.now().unixtime();
  }

  return _fallback->getCurrentTime();
}

void AutoDiscoverRTCClock::setCurrentTime(uint32_t time) { 
  if (ds3231_success) {
    rtc_3231.adjust(DateTime(time));
  } else if (rv3028_success) {
    auto dt = DateTime(time);
	  uint8_t weekday = (dt.day() + (uint16_t)((2.6 * dt.month()) - 0.2) - (2 * (dt.year() / 100)) + dt.year() + (uint16_t)(dt.year() / 4) + (uint16_t)(dt.year() / 400)) % 7;
    rtc_rv3028.setTime(dt.year(), dt.month(), weekday, dt.day(), dt.hour(), dt.minute(), dt.second());
  } else if (rtc_8563_success) {
    rtc_8563.adjust(DateTime(time));
  } else if (rtc_8130_success) {
    MESH_DEBUG_PRINTLN("RX8130CE: Setting time");
    rtc_8130.adjust(DateTime(time));
  } else {
    _fallback->setCurrentTime(time);
  }
}

#endif // NO_EXTERNAL_RTC
