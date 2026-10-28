#!/bin/bash
# Host test of the hardware watchdog (rc8, C23): the REAL src/helpers/fw_ota/Watchdog.h is compiled against simulated nRF52 WDT/POWER
# registers and checked (configuration, the "already running after a soft reset" path, feeding, reset reasons); then the source is
# checked for where it is started and fed (setup before the mesh starts; once per loop pass before the idle sleep; before our own
# restarts), that the deliberate hang exists only in the dev build, and that the timeout outlasts the bootloader's 5-min BLE DFU window.
set -e
cd "$(dirname "$0")/.."
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/inc"
cat > "$W/inc/Arduino.h" <<'EOF'
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
struct WdtRegs { uint32_t CONFIG, CRV, RREN, TASKS_START, RR[8], RUNSTATUS; };
static WdtRegs g_wdt_regs;
#define NRF_WDT (&g_wdt_regs)
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
static uint32_t g_resetreas;
static inline uint32_t readResetReason() { return g_resetreas; }
EOF
cat > "$W/inc/nrf_wdt.h" <<'EOF'
#pragma once
enum nrf_wdt_rr_register_t { NRF_WDT_RR0 = 0 };
static inline bool nrf_wdt_started(WdtRegs* w) { return w->RUNSTATUS != 0; }
static inline void nrf_wdt_reload_request_set(WdtRegs* w, nrf_wdt_rr_register_t r) { w->RR[r] = 0x6E524635UL; }
EOF
cat > "$W/t.cpp" <<'EOF'
#include <cstdio>
#include <cassert>
#include "src/helpers/fw_ota/Watchdog.h"
namespace fw_ota { Watchdog g_wdt; }
using fw_ota::Watchdog;
int main() {
  int n = 0;
  { memset(&g_wdt_regs, 0, sizeof g_wdt_regs); g_resetreas = 0; Watchdog w; assert(!w.armed()); w.feed(); assert(g_wdt_regs.RR[0] == 0); n++;   // not armed: feed is a no-op
    w.start(); assert(w.armed() && g_wdt_regs.TASKS_START == 1 && g_wdt_regs.RREN == WDT_RREN_RR0_Msk); n++;
    assert((g_wdt_regs.CONFIG & 1) == 1 && (g_wdt_regs.CONFIG & 8) == 0); n++;                 // counts during CPU sleep, pauses for a debugger
    assert(g_wdt_regs.CRV + 1 == (uint32_t)FW_OTA_WDT_SECS * 32768u); n++;                       // timeout exactly FW_OTA_WDT_SECS
    w.feed(); assert(g_wdt_regs.RR[0] == 0x6E524635UL); n++;
    assert(!w.lastResetWasWatchdog() && !strcmp(w.lastResetText(), "power on")); n++; }
  { memset(&g_wdt_regs, 0, sizeof g_wdt_regs); g_wdt_regs.RUNSTATUS = 1; g_wdt_regs.CRV = Watchdog::ourCrv(); g_wdt_regs.CONFIG = 9;   // ours, still running after a soft reset
    g_resetreas = POWER_RESETREAS_SREQ_Msk; Watchdog w; w.start();
    assert(w.armed() && !w.draining() && g_wdt_regs.CONFIG == 9 && g_wdt_regs.TASKS_START == 0); n++;   // not reconfigured (impossible anyway)
    assert(g_wdt_regs.RR[0] == 0x6E524635UL); n++;                                                // ... but fed at once
    assert(!strcmp(w.lastResetText(), "soft reset") && w.runningSecs() == FW_OTA_WDT_SECS); n++; }
  { memset(&g_wdt_regs, 0, sizeof g_wdt_regs); g_wdt_regs.RUNSTATUS = 1; g_wdt_regs.CRV = 72000u * 32768u - 1u; g_wdt_regs.CONFIG = 9;   // (C27) rc8's 20 h runs
    g_resetreas = POWER_RESETREAS_SREQ_Msk; Watchdog w; w.start();
    assert(w.armed() && w.draining() && g_wdt_regs.TASKS_START == 0 && g_wdt_regs.CRV == 72000u * 32768u - 1u); n++;
    assert(g_wdt_regs.RR[0] == 0); w.feed(); assert(g_wdt_regs.RR[0] == 0); n++;               // the main loop no longer feeds: it runs out once
    w.feedAlways(); assert(g_wdt_regs.RR[0] == 0x6E524635UL); n++;                                // critical steps still get a full period
    char t[12]; assert(w.runningSecs() == 72000 && !strcmp(w.runningText(), "20 h") && !strcmp(Watchdog::secsText(360, t), "6 min")); n++; }
  { memset(&g_wdt_regs, 0, sizeof g_wdt_regs); g_resetreas = POWER_RESETREAS_DOG_Msk; Watchdog w; w.start();   // after that watchdog reset: ours
    assert(!w.draining() && g_wdt_regs.TASKS_START == 1 && g_wdt_regs.CRV == Watchdog::ourCrv() && w.lastResetWasWatchdog()); n++; }
  { struct { uint32_t r; const char* t; bool dog; } R[] = { {POWER_RESETREAS_DOG_Msk, "watchdog", true}, {POWER_RESETREAS_LOCKUP_Msk, "CPU lockup", false},
      {POWER_RESETREAS_RESETPIN_Msk, "reset pin", false}, {POWER_RESETREAS_OFF_Msk, "wake from off", false},
      {POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_SREQ_Msk, "watchdog", true} };
    for (auto& x : R) { g_resetreas = x.r; Watchdog w; w.start(); assert(!strcmp(w.lastResetText(), x.t) && w.lastResetWasWatchdog() == x.dog); n++; } }
  static_assert(FW_OTA_WDT_SECS > 300, "the watchdog must outlast the bootloader's 5-minute BLE DFU inactivity window");
  assert(FW_OTA_WDT_SECS == 600 && !strcmp(Watchdog::timeoutText(), "10 min")); n++;            // 10 min (owner, rc9)
  static_assert((uint64_t)FW_OTA_WDT_SECS * 32768u - 1u <= 0xFFFFFFFFu, "CRV is 32 bits");
  n += 2;
  printf("%d", n); return 0;
}
EOF
g++ -std=c++11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$W/inc" -I . -o "$W/t" "$W/t.cpp"
N=$("$W/t")
python3 - "$N" <<'EOF'
import re, sys
n = int(sys.argv[1])
m = open("examples/simple_repeater/main.cpp").read()
setup = m[m.index("void setup()"):m.index("void loop()")]
loop = m[m.index("void loop()"):]
def need(cond, msg):
    global n
    if not cond: sys.exit("FAIL: " + msg)
    n += 1
need("g_wdt.start()" in setup, "the watchdog is not started in setup()")
need(setup.index("g_wdt.start()") < setup.index("the_mesh.begin("), "the watchdog must start before the mesh (and LoRa-OTA) start")
need(setup.index("g_wdt.start()") > setup.index("board.begin()"), "start it after board.begin()")
need(loop.count("g_wdt.feed()") == 1, "feed exactly once per loop pass (from the main loop only)")
need(loop.index("g_wdt.feed()") < loop.index("board.sleep("), "feed before the idle sleep")
need("g_wdt.feed()" not in setup, "setup() must not feed it (only the loop proves the node is alive)")
x = open("src/helpers/fw_ota/FwOtaTransfer.cpp").read()
r = x[x.index("if (_reset_at_ms && (int32_t)(now - _reset_at_ms) >= 0)"):]
need(r.index("g_wdt.feedAlways()") < r.index("NVIC_SystemReset()"), "feed (always) right before our own restart into the bootloader")
b = x[x.index("void FwOtaTransfer::blApplyNow()"):]
need(b.index("g_wdt.feedAlways()") < b.index("sd_mbr_command(&c)"), "a full period before the MBR bootloader copy (C27: also while draining)")
w = x[x.index("void FwOtaTransfer::begin("):]
need("WDD_FILE" in w[:w.index("_serve_disabled")] and "_wdt_planned" in w[:w.index("_serve_disabled")], "the planned restart of a timeout change must be marked and not counted")
need(x.count("g_wdt.feed()") == 0, "FwOtaTransfer must use feedAlways() (its feeds are for steps that must not be cut short)")
hang = x.index("for (;;) { }")
need(x.rfind("#ifdef FW_OTA_DEV", 0, hang) > x.rfind("#endif", 0, hang), "the deliberate hang must be in the dev build only")
c = open("src/helpers/fw_ota/FwOtaCli.h").read()
need('"wdt"' in c and "cmdWdt" in c, "fw wdt is not wired into the CLI")
print(f"hardware watchdog (C23) checks: {n} passed")
EOF
