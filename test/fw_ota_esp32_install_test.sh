#!/bin/bash
# (Heltec port) host test of the ESP32 install path (see fw_ota_esp32_install_test.cpp). usage: fw_ota_esp32_install_test.sh <HELTECV4 .lota>
# ESP32_MUTANT=<name>: put a bug in - the test must then FAIL (noguard, notag, nocrc, noattempt)
set -e -o pipefail
cd "$(dirname "$0")/.."
W=$(mktemp -d); trap 'rm -rf "${W:?}"' EXIT
mkdir -p "$W/src/helpers/fw_ota"; cp -r src/helpers/fw_ota/. "$W/src/helpers/fw_ota/"
if [ -n "$ESP32_MUTANT" ]; then python3 - "$W/src/helpers/fw_ota" "$ESP32_MUTANT" <<'PY'
import sys
d, m = sys.argv[1], sys.argv[2]
M = {"noguard": ("FwOtaSlotsEsp32.cpp", "  return slotPartitionOk() && addr >= SLOT_PART_ADDR", "  return true || addr >= SLOT_PART_ADDR"),
     "notag": ("FwOtaInstallEsp32.cpp", "  if (!sink.tag) return 11;", ""),
     "nocrc": ("FwOtaInstallEsp32.cpp", "  if (sink.crc != h.raw_crc32) return 5;", ""),
     "noattempt": ("FwOtaInstallEsp32.cpp", "  cb.boot_attempts++; writeCtrl(cb);", "")}
f, a, b = M[m]; p = f"{d}/{f}"; s = open(p).read(); assert s.count(a) == 1, (m, a); open(p, "w").write(s.replace(a, b)); print(f"ESP32 MUTANT {m} applied")
PY
fi
I=(-I test/esp32mock -I "$W/src")
gcc -c -O1 -g -fsanitize=address,undefined -w -DESP32 "${I[@]}" "$W/src/helpers/fw_ota/lzma/LzmaDec.c" -o "$W/lzma.o"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -w -DESP32 -DHELTEC_LORA_V4 "${I[@]}" \
  test/fw_ota_esp32_install_test.cpp test/esp32mock/esp32mock.cpp "$W/src/helpers/fw_ota/FwOtaSlotsEsp32.cpp" "$W/src/helpers/fw_ota/FwOtaInstallEsp32.cpp" \
  "$W/src/helpers/fw_ota/FwTag.cpp" "$W/lzma.o" -o "$W/t"
ASAN_OPTIONS=detect_leaks=0 "$W/t" "$1"   # a simulated power cut abandons the decoder mid-install (hardware: RAM is gone too)
