/*
 * ClockHold (rc21, C57): the time a node hands out never steps BACKWARDS - peers treat an older timestamp as a replay. A clock that runs
 * ahead of a better reference by d seconds is frozen for d seconds instead ("hold"); one that is behind is stepped forward.
 * Used by AutoDiscoverRTCClock (the RV-3028 hourly resync, and the time a node without an RTC gets from a swarm member) and by the
 * simulator's clock, so both run this one piece of logic. raw = the software clock (seconds), out = the time handed out.
 */
#pragma once
#include <stdint.h>

namespace fw_ota {

struct ClockHold {
  uint32_t hold, last_raw, last_out;
  void restart(uint32_t raw, uint32_t out) { last_raw = raw; last_out = out; hold = 0; }
  uint32_t now(uint32_t raw) {
    const uint32_t e = raw - last_raw; last_raw = raw;
    const uint32_t eat = e < hold ? e : hold;
    hold -= eat; last_out += e - eat;
    return last_out;
  }
  // ref = a better time. true: the clock is behind - the caller steps the raw clock to ref (and calls restart); false: ahead or equal,
  // the hold now absorbs the difference
  bool toward(uint32_t raw, uint32_t ref) {
    const uint32_t out = now(raw);
    if (ref > out) return true;
    hold = out - ref;
    return false;
  }
};

}  // namespace fw_ota
