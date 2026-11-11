#!/bin/bash
# (rc25, C99 stage 2, owner 2026-11-06) VERSION UPGRADE in the simulator: the LoRa-OTA state (circle, duty, auto-apply, minbatt, a pull
# at 40 % with its state file + chunks in the slot) is written by the PREVIOUS release's fw_ota and booted by this tree's.
# usage: test/fw_ota_upgrade_test.sh <previous release tree>     env: SIM_CORE, SIM_CRYPTO (as fw_ota_sim_test.sh)
# Last line: "C99 upgrade: N checks passed, M failed". Stage 1 (prefs.json through MeshCore's real loader) is otafix tools/c99/.
set -e -o pipefail
cd "$(dirname "$0")/.."
NEW=$(pwd); OLD=$(cd "${1:?usage: fw_ota_upgrade_test.sh <previous release tree>}" && pwd)
D=$(mktemp -d); trap 'rm -rf "${D:?}"' EXIT
SIM_DEV="$OLD" SIM_MAIN="$NEW/test/sim/c99_upgrade.cpp" bash "$NEW/test/fw_ota_sim_test.sh" SAVE "$D" | grep -E "C99|FAIL"
# C99_MUTANT=c99state: the LOAD side only (this tree) - the run must then FAIL (release.sh)
SIM_MUTANT="${C99_MUTANT:-}" SIM_MAIN="$NEW/test/sim/c99_upgrade.cpp" bash "$NEW/test/fw_ota_sim_test.sh" LOAD "$D" | grep -E "C99|FAIL|  B had"
