#!/bin/bash
# Guard for the RAK3401 1W "low TX power without a GPS module" bug (MeshCore #3268 -> fixed by #3275, 2026-08; PowerSaving 17.1.4):
# WB_IO2 = PIN_3V3_EN (P0.34) switches the 3V3_S rail AND the RAK13302's 5 V boost that feeds the SKY66122 PA. The GPS probe and
# stop_gps() drove it LOW when no GPS answered, so the PA ran unpowered (a few dBm instead of 1 W). Our release build has no GPS code
# (it does not extend sensor_base), so it never had the bug - this test keeps it that way after base merges:
#  1. the release env does not pull in ENV_INCLUDE_GPS / sensor_base / RAK_WISBLOCK_GPS;
#  2. in the code compiled into it, PIN_3V3_EN / WB_IO2 is written LOW only in RAK3401Board::initiateShutdown();
#  3. optional (argument = a built firmware.elf): no GPS probe symbols are linked.
set -e
cd "$(dirname "$0")/.."
python3 - "$@" <<'EOF'
import re, sys, subprocess, glob, os
n = 0
def need(c, m):
    global n
    if not c: sys.exit("FAIL: " + m)
    n += 1
ini = open("platformio.local.ini").read()
if "[env:RAK_3401_lora_ota]" not in ini:              # the dev checkout keeps old envs; the release env lives in the release tree
    sys.exit("FAIL: platformio.local.ini has no [env:RAK_3401_lora_ota]")
rel = ini[ini.index("[env:RAK_3401_lora_ota]"):]
rel = rel[:rel.index("[env:", 10)] if "[env:" in rel[10:] else rel   # the release env only (the dev env extends it)
need("extends = nrf52_base" in rel, "the release env must extend nrf52_base (not rak3401 / sensor_base)")
for bad in ("sensor_base", "ENV_INCLUDE_GPS", "RAK_WISBLOCK_GPS", "helpers/sensors>"):
    need(bad not in rel, f"the release env pulls in {bad} (GPS probe code toggles the PA's 5 V boost on WB_IO2)")
low = re.compile(r"digitalWrite\(\s*(PIN_3V3_EN|WB_IO2|34)\s*,\s*LOW\s*\)")
files = ["variants/rak3401/RAK3401Board.cpp", "variants/rak3401/variant.cpp", "variants/rak3401/target.cpp",
         "examples/simple_repeater/main.cpp", "examples/simple_repeater/MyMesh.cpp"] + glob.glob("src/helpers/fw_ota/*")
for f in files:
    s = open(f).read()
    for m in low.finditer(s):
        fn = s.rfind("::", 0, m.start()); fn = s[fn:s.index("(", fn)] if fn >= 0 else ""
        need(f.endswith("RAK3401Board.cpp") and fn == "::initiateShutdown", f"{f}: {m.group(0)} outside initiateShutdown() - kills the PA supply")
b = open("variants/rak3401/RAK3401Board.cpp").read()
need(re.search(r"digitalWrite\(\s*PIN_3V3_EN\s*,\s*HIGH\s*\)", b[b.index("void RAK3401Board::begin"):]), "begin() must switch PIN_3V3_EN HIGH")
if len(sys.argv) > 1:
    nm = glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm"))[0]
    syms = subprocess.run([nm, "-C", sys.argv[1]], capture_output=True, text=True, check=True).stdout
    for s in ("gpsIsAwake", "rakGPSInit", "stop_gps", "start_gps", "SFE_UBLOX_GNSS"):
        need(s not in syms, f"{s} is linked into {sys.argv[1]}")
print(f"RAK3401 PA supply rail (upstream #3275) checks: {n} passed")
EOF
