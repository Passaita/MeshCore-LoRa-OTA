#pragma once
#include <Arduino.h>
enum nrf_wdt_rr_register_t { NRF_WDT_RR0 = 0 };
inline bool nrf_wdt_started(WdtRegs* w) { return w->RUNSTATUS != 0; }
void sim_wdt_feed(WdtRegs* w);                    // records the feed time (the scheduler checks expiry)
inline void nrf_wdt_reload_request_set(WdtRegs* w, nrf_wdt_rr_register_t r) { w->RR[r] = 0x6E524635UL; sim_wdt_feed(w); }
