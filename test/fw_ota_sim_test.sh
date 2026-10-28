#!/bin/bash
# LoRa-OTA network simulator ("cyber test", owner 2026-10-06; docs/TEST-STRATEGY-20261006.md): compiles the REAL MeshCore core and the
# REAL LoRa-OTA sources (this repo's src/helpers/fw_ota over the release tree's core) into one PC program that runs many virtual nodes on a
# simulated LoRa channel (test/sim/), then runs the scenarios in test/sim/scenarios.cpp.
# usage: test/fw_ota_sim_test.sh [-v]     env: SIM_CORE (default: this checkout), SIM_CRYPTO (rweather Crypto)
set -e -o pipefail
cd "$(dirname "$0")/.."
DEV=$(pwd)
CORE=${SIM_CORE:-$DEV}
CRYPTO=${SIM_CRYPTO:-$CORE/.pio/libdeps/RAK_3401_lora_ota/Crypto}
W=$(mktemp -d); trap 'rm -rf "${W:?}"' EXIT
mkdir -p "$W/src"
cp -r "$CORE/src/." "$W/src/"
cp "$DEV"/src/helpers/fw_ota/* "$W/src/helpers/fw_ota/"                 # the code under test
# FwOtaSlots.cpp reads the memory-mapped flash directly: point those reads at the node's simulated flash
python3 - "$W/src/helpers/fw_ota/FwOtaSlots.cpp" <<'PY'
import sys; p = sys.argv[1]; s = open(p).read()
import re
casts = re.findall(r"\(const void\*\)(\w+)", s)                 # every direct read of memory-mapped flash
m = s.count("(const uint8_t*)(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off)")
assert sorted(casts) == ["addr"] * 4 + ["page_addr"] and m == 1, f"FwOtaSlots.cpp direct flash accesses changed: {casts} + {m} slot pointer - update the simulator"
s = re.sub(r"\(const void\*\)(addr|page_addr)\b", r"(const void*)sim_flash_ptr(\1)", s)
s = s.replace("(const uint8_t*)(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off)", "sim_flash_ptr(SLOT_ADDR + SLOT_PAYLOAD_OFFSET + off)")
s = s.replace("namespace fw_ota {", "const uint8_t* sim_flash_ptr(uint32_t addr);\nnamespace fw_ota {", 1)
open(p, "w").write(s)
PY
# SIM_MUTANT=<name>: put an OLD bug back into the code under test - the scenarios must then FAIL (proof the simulator catches it)
if [ -n "$SIM_MUTANT" ]; then python3 - "$W/src/helpers/fw_ota/FwOtaTransfer.cpp" "$SIM_MUTANT" <<'PY2'
import sys; p, m = sys.argv[1], sys.argv[2]; s = open(p).read()
M = {
  # C30 (rc12): the switch time from the rate "since the download started" - it fell while the pull waited, the switch never came
  "c30": ("uint32_t FwOtaTransfer::switchAfterMs() const { return switchAfter(_stall_cph); }",
          "uint32_t FwOtaTransfer::switchAfterMs() const { uint32_t el_s = (millis() - _rate_t0_ms) / 1000, got = _cm.have - _rate_have0;\n"
          "  return switchAfter(el_s >= 60 && got >= 3 ? (uint32_t)((uint64_t)got * 3600 / el_s) : 0); }"),
  # C33 (rc11-rc13): a PACE overheard for a neighbour's pull went to the MAC check and counted as mac_fail
  "c33": ("if ((b[1] == MSG_CHUNK || b[1] == MSG_PACE) && len >= 6) {", "if (b[1] == MSG_CHUNK && len >= 6) {"),
  # C34 (rc13-rc14): the silence before a failover counted from the last wait start, which every re-offer moved (8.9 min instead of ~3-4)
  "c34": ("since(now, quiet_since) > switchAfterMs()", "now - _wait_started_ms > switchAfterMs()"),
  # rc15 R9: no shared stream (v1.17.28-rc14 behaviour: every puller requests its own chunks)
  "noshare": ("  if (!_stream_ms) return false;\n  const uint32_t t = streamT();", "  if (true) return false;\n  const uint32_t t = streamT();"),
  # rc15 R13: every server is rc14-like - session chunks only (the shared stream needs the image chunks)
  "noimg": ("  const bool img = (b[8] & 0x80) != 0;", "  const bool img = false;"),
  # rc16 text audit: F2 - a listener shows the duty placeholder of its listen offer
  "f2": ("    const bool lis = _phase == PH_DOWNLOADING && (_listening || _gap_phase || (_fill && !_sess_set));", "    const bool lis = false;"),
  # rc16 text audit: 5.3 - "not installed here" before the install check ran
  "s53": ("if (v && (!_probe_valid || probeKey(h) != _probe_key))", "if (false)"),
  # rc16 text audit: F1 - the reason appended whatever the length (cut at 160 B)
  "f1": ("if (_last_err[0] && l > 0 && l + 1 + strlen(_last_err) < 159)", "if (_last_err[0] && l > 0)"),
  # rc15 R4: no answer suppression (rc14: every holder answers every flooded search)
  "nosupp": ("if (p.heard >= ANS_SUPPRESS_K)", "if (false)"),
  # rc17 fleet fill: a fill installs by itself (owner 9.1 b: never)
  "fillinst": ('else if (_fill) strcpy(_last_err, "fleet fill done: served, not installed");', 'else if (false) strcpy(_last_err, "");'),
  # rc17: mode 1 replaces older images too (owner: empty slots only)
  "mode1rep": ("if (mode != FLEET_REPLACE || activePulls()) return false;", "if (activePulls()) return false;"),
  # rc17: a node serving a pull is overwritten
  "busyrep": ("if (mode != FLEET_REPLACE || activePulls()) return false;", "if (mode != FLEET_REPLACE) return false;"),
  # rc17: collectors do not wait for the requester (every pause turns them into requesters at the server)
  "noalive": ("  return _fann_heard_ms && since(now, _fann_heard_ms) < FANN_ALIVE_MS && _fann_have < _man.n_chunks\n", "  return false && _fann_have < _man.n_chunks\n"),
  # rc17: no local repair (gaps straight along the path)
  "nolocal": ("  if (_gap_dry >= g_tune.gap_dry_max) {\n", "  if (true) { gapEscalate(now); return; }\n  if (false) {\n"),
  # rc17: only installed images are served (rc16 rule): a fleet-filled node cannot serve the next wave
  "noserve": ("if (!force && readCtrl(cb) && cb.apply_state == APPLY_FAILED && !installedImage(h) && !bleFilled(h)) {", "if (!force && !installedImage(h) && !bleFilled(h)) {"),
  # rc17: a responder that hears a better link repair keeps sending (duplicate repairs)
  "repdup": ("  if (!_rep.sent) { stats.repairs_supp += _rep.n - _rep.i; _rep.i = _rep.n; return; }", ""),
  # rc18 (gap-fill study G): a pull by id stays pinned to its host (rc17) - MDP never finishes from a nearer holder (G1)
  "noadopt": [("  if (g_tune.req_adopt && !_img_set && !_fill && _img_ok && nm.kind == KIND_APP) {", "  if (false) {"),
              ("  if (g_tune.algo != ALGO_E || !g_tune.src_spread || !g_tune.req_adopt || _fill || m.kind != KIND_APP) return false;", "  return false;")],
  # rc18: holders gossip only 6 h (study default) - a node CRITICAL during the stream never gets the image (G2)
  "life6h": ("since(now, _tr_inc_ms) < (spread ? g_tune.gossip_life_ms : PASSIVE_QUIET_MS)", "since(now, _tr_inc_ms) < 21600000u"),
  # rc18: the source spreads only after an accepted offer - a fading tail whose answers are lost never starts (G3)
  "nosrcspread": ("  if (match && !bl && fleet) spreadStart(m.sha256);", ""),
  # rc18: every pull spreads hop by hop (not only fleet pulls) - a plain pull replaces older images everywhere (F1)
  "spreadall": [("  if (match && !bl && fleet) spreadStart(m.sha256);", "  if (match && !bl) spreadStart(m.sha256);"),
                ("    if ((_fill ? _fill_mode : _fleet_mode) == FLEET_REPLACE) memcpy(_spread_tag", "    if (true) memcpy(_spread_tag")],
  # rc18: a CRITICAL node keeps requesting (no battery rest) (G5)
  "norest": ("  if (g_tune.batt_rest && powerState() == 3 && (_phase == PH_OFFERING", "  if (false && (_phase == PH_OFFERING"),
  # rc18: the fleet flag is lost on a reboot before the first answer (G4)
  "nofleetload": ("if (mk == 0xA6 && f.read(&fm, 1) == 1 && fm == 0xE2) _fleet_mode = FLEET_REPLACE;", ""),
  # rc19 (C42): 'fw pull-abort' of a fleet pull sends no cancel (rc18) - the listeners go on filling (H1)
  "nocancel": ("  const bool fleet = (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) && !_fill", "  const bool fleet = false && (_phase == PH_DOWNLOADING || _phase == PH_OFFERING) && !_fill"),
  # rc19 (C42): a plain pull's listener escalates to a pull along the path after its dry rounds (rc18) (H2)
  "m1esc": ("    if (m1) { _gapr_at_ms = now + 60000; return; }", ""),
  # rc19 (C42): a plain pull's listener never pauses (rc18: it asks its neighbours on and on) (H2)
  "nopause": ('  if (m1 && !reqHeard(now)) { pauseFill("passive fill paused"); return; }', ""),
  # rc19 (C44): a holder's announce counts as the requester's progress (rc18: "done" - the listeners left the stream) (H3)
  "holderlive": ("    if (!hann && (!_fill || mode >= _fill_mode)) { _fann_heard_ms = millis() | 1; _fann_have = rhave;", "    _fann_heard_ms = millis() | 1; _fann_have = rhave; if (!hann && (!_fill || mode >= _fill_mode)) {"),
  # rc19 (C44): gap rounds ask for the first missing chunks, also ahead of a live stream (rc18) (H3)
  "gapahead": ("  const uint16_t lim = !live ? (uint16_t)_cm.n :", "  const uint16_t lim = true ? (uint16_t)_cm.n :"),
  # rc19 (C47): a resumed session shows "starting" at the server until its first request (H4)
  "c47": ("  if (!ds->top) ds->top = ptop;", ""),
  # rc19 (C42): a plain pull's listener missing more than 20 % when the stream stops asks its neighbours for all of it (a hidden full pull) (H7)
  "miss20": ("(miss * 5 > _cm.n || !reqHeard(now))", "(!reqHeard(now))"),
  # rc19: an announce with a LOWER mode (a neighbour's path pull, mode 1) demoted a fleet fill to passive: it paused for good (H5)
  "demote": ("if (!hann && (!_fill || mode >= _fill_mode))", "if (!hann)"),
  # rc20 (C48): a move to a "faster" holder is not checked against the measured rate (rc19) (H11)
  "noeval": ("  if (g_tune.move_eval) { _mv_cph", "  if (false) { _mv_cph"),
  # rc20 (C49): the session ABORT of 'fw pull-abort' is sent once (twice, seconds apart: rc19) (H13)
  "noabtrep": ("    _abt_n = fleet ? 3 : 1;", "    _abt_n = 0;"),
  # rc21 (C58): an apply adds a flat 30 s and writes it - also on a node whose RTC chip kept the time (rc20) (H15)
  "c58": ("  if (t == 0 || (_flood_host && _flood_host->clockSource())) return;\n  t += CLOCK_APPLY_S + millis() / 1000;",
          "  if (t == 0) return;\n  t += 30;"),
  # rc21 (C57): every node with an RTC answers a time request (no suppression) (H15)
  "tsupp": ("  if (_ta_at_ms && nonce == _ta_nonce) { _ta_at_ms = 0; _ta_supp++; }", "  if (false) { _ta_supp++; }"),
  # rc21 (C57): a peer's time that is behind is SET (the clock steps back) instead of held (H15)
  "tback": ("  if (d >= CLK_STEP_MIN_S || d <= -CLK_HOLD_MIN_S) _flood_host->clockDiscipline(ref);",
            "  if (d >= CLK_STEP_MIN_S || d <= -CLK_HOLD_MIN_S) _mesh->getRTCClock()->setCurrentTime(ref);"),
  # rc21 (C57): no bound - a peer 2 h off is taken (H15)
  "tbound": ("  if (d < -CLK_BOUND_S || (_clk_synced && d > CLK_BOUND_S)) { _clk_refused++; return; }", "  if (false) { _clk_refused++; return; }"),
}
for a, b in (M[m] if isinstance(M[m], list) else [M[m]]):
  assert s.count(a) == 1, f"mutant {m}: the fixed code is not there any more"; s = s.replace(a, b)
open(p, "w").write(s); print(f"MUTANT {m} applied")
PY2
fi
# -fno-sanitize=nonnull-attribute: MeshCore copyPath() does memcpy(dst, NULL, 0) for zero-hop sends (harmless, formally UB)
OPT=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-sanitize=nonnull-attribute); COPT=(-O1 -fsanitize=address)
[ -n "$SIM_FAST" ] && { OPT=(-O2 -g); COPT=(-O2); }             # SIM_FAST=1: no sanitizers (long measurement runs; the checks run with them)
CX=(-std=c++17 "${OPT[@]}" -w -fpermissive -DNRF52_PLATFORM -DFW_OTA -DFW_OTA_STUDY
    "-Dg_xfer=(*sim_cur_xfer)" "-Dg_wdt=(*sim_cur_wdt)"
    -I"$DEV/test/sim/shim" -I"$W/src" -I"$CORE/lib/ed25519" -I"$CRYPTO" -I"$DEV/test/sim")
SRCS=("$W/src/Mesh.cpp" "$W/src/Dispatcher.cpp" "$W/src/Packet.cpp" "$W/src/Identity.cpp" "$W/src/Utils.cpp"
      "$W/src/helpers/StaticPoolPacketManager.cpp"
      "$W/src/helpers/fw_ota/FwOtaTransfer.cpp" "$W/src/helpers/fw_ota/FwOtaSlots.cpp" "$W/src/helpers/fw_ota/FwTag.cpp" "$W/src/helpers/fw_ota/RadioDiag.cpp"
      "$DEV/test/sim/sim.cpp" "$DEV/test/sim/sim_rng.cpp" "$DEV/test/sim/audit.cpp" "$DEV/test/sim/scenarios.cpp")
for f in Crypto Hash SHA256 SHA512 AES128 AESCommon BlockCipher Ed25519 Curve25519 BigNumberUtil; do SRCS+=("$CRYPTO/$f.cpp"); done
for f in "$CORE"/lib/ed25519/*.c; do gcc -c "${COPT[@]}" -w -I"$CORE/lib/ed25519" "$f" -o "$W/$(basename "$f").o"; done
g++ "${CX[@]}" "${SRCS[@]}" "$W"/*.c.o -o "$W/sim" 2>&1 | grep -E "error|undefined" | head -40 || true
test -x "$W/sim" || { echo "FAIL: the simulator did not build"; exit 1; }
[ -n "$SIM_KEEP" ] && cp "$W/sim" "$SIM_KEEP"                                   # SIM_KEEP=<path>: keep the binary (debugging)
ASAN_OPTIONS=detect_leaks=0 "$W/sim" "$@"   # MeshCore's packet manager has no destructor (never freed on the device; the simulator reboots nodes)
