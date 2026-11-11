#!/bin/bash
# Guard for the RAK3401 1W "low TX power without a GPS module" bug (MeshCore #3268 -> fixed by #3275, 2026-08; PowerSaving 17.1.4):
# WB_IO2 = PIN_3V3_EN (P0.34) switches the 3V3_S rail AND the RAK13302's 5 V boost that feeds the SKY66122 PA. The GPS probe and
# stop_gps() drove it LOW when no GPS answered, so the PA ran unpowered (a few dBm instead of 1 W). Our release build has no GPS code
# (no sensor_base: rc23 extends [rak3401] and unflags it), so it never had the bug - this test keeps it that way after base merges:
#  1. the release env does not pull in ENV_INCLUDE_GPS / sensor_base / RAK_WISBLOCK_GPS (unflagged when it extends rak3401);
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
T = os.path.expanduser(os.environ.get("RAK_TREE", "~/mc_firmware/RAK3401_upstream"))   # (rc23) the RELEASE tree (v18 base): its env, variant and
ini = open(os.path.join(T, "platformio.local.ini")).read()                                 #   example sources are what is built (test host: RAK_TREE)
rel = ini[ini.index("[env:RAK_3401_lora_ota]"):]
rel = rel[:rel.index("[env:", 10)] if "[env:" in rel[10:] else rel   # the release env only (the dev env extends it)
# (rc23, C68) the env may extend IoTThinks' [rak3401] (their board flags reach us) only when it drops sensor_base's GPS / sensor flags and
# libraries again: every ENV_INCLUDE_* flag in the env must be in build_unflags, lib_deps / src filter must not take rak3401's or sensor_base's
ext = re.search(r"^extends\s*=\s*(\S+)", rel, re.M).group(1)
need(ext in ("nrf52_base", "rak3401"), f"the release env extends {ext} (expected nrf52_base or rak3401)")
unf = rel[rel.index("build_unflags"):] if "build_unflags" in rel else ""
nx = re.search(r"^\S", unf[unf.index("\n") + 1:], re.M) if "\n" in unf else None   # the next key ends the unflags list
unf = unf[:unf.index("\n") + 1 + nx.start()] if nx else unf
if ext == "rak3401":
    need("-D ENV_INCLUDE_GPS=1" in unf, "the env extends rak3401 but does not unflag -D ENV_INCLUDE_GPS=1 (GPS probe toggles WB_IO2)")
    for f in ("ENV_INCLUDE_BME680_BSEC", "ENV_INCLUDE_BME680", "ENV_INCLUDE_VL53L0X"):
        need(f in unf, f"the env extends rak3401 but does not unflag {f}")
    need("${rak3401.lib_deps}" not in rel and "${rak3401.build_src_filter}" not in rel, "the env takes rak3401's libraries / sources (GPS, BSEC, OLED)")
for bad in ("sensor_base", "RAK_WISBLOCK_GPS", "helpers/sensors>"):
    need(bad not in rel, f"the release env pulls in {bad} (GPS probe code toggles the PA's 5 V boost on WB_IO2)")
need("ENV_INCLUDE_GPS" not in rel.replace(unf, ""), "ENV_INCLUDE_GPS set in the release env (outside build_unflags)")
low = re.compile(r"digitalWrite\(\s*(PIN_3V3_EN|WB_IO2|34)\s*,\s*LOW\s*\)")
files = ["variants/rak3401/RAK3401Board.cpp", "variants/rak3401/variant.cpp", "variants/rak3401/target.cpp",
         "examples/simple_repeater/main.cpp", "examples/simple_repeater/MyMesh.cpp"] + [f for f in glob.glob("src/helpers/fw_ota/*") if os.path.isfile(f)]
for f in files:
    s = open(os.path.join(T, f) if not f.startswith("src/helpers/fw_ota") else f).read()
    for m in low.finditer(s):
        fn = s.rfind("::", 0, m.start()); fn = s[fn:s.index("(", fn)] if fn >= 0 else ""
        need(f.endswith("RAK3401Board.cpp") and fn == "::initiateShutdown", f"{f}: {m.group(0)} outside initiateShutdown() - kills the PA supply")
b = open(os.path.join(T, "variants/rak3401/RAK3401Board.cpp")).read()
need(re.search(r"digitalWrite\(\s*PIN_3V3_EN\s*,\s*HIGH\s*\)", b[b.index("void RAK3401Board::begin"):]), "begin() must switch PIN_3V3_EN HIGH")
if len(sys.argv) > 1:
    nm = glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm"))[0]
    syms = subprocess.run([nm, "-C", sys.argv[1]], capture_output=True, text=True, check=True).stdout
    for s in ("gpsIsAwake", "rakGPSInit", "stop_gps", "start_gps", "SFE_UBLOX_GNSS"):
        need(s not in syms, f"{s} is linked into {sys.argv[1]}")
print(f"RAK3401 PA supply rail (upstream #3275) checks: {n} passed")
EOF
