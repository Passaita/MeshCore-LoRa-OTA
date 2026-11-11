#!/bin/bash
# LoRa-OTA network simulator ("cyber test", owner 2026-10-06; docs/protocol/TEST-STRATEGY-20261006.md): compiles the REAL MeshCore core and the
# REAL LoRa-OTA sources (this repo's src/helpers/fw_ota over the release tree's core) into one PC program that runs many virtual nodes on a
# simulated LoRa channel (test/sim/), then runs the scenarios in test/sim/scenarios.cpp.
# usage: test/fw_ota_sim_test.sh [-v]     env: SIM_CORE (default ~/mc_firmware/RAK3401_upstream), SIM_CRYPTO (rweather Crypto)
set -e -o pipefail
cd "$(dirname "$0")/.."
DEV=$(pwd)
# (rc25, C99 stage 2) SIM_DEV=<tree>: compile THAT tree's fw_ota + simulator (the previous release); SIM_MAIN=<file.cpp>: instead of
# scenarios.cpp (test/sim/c99_upgrade.cpp, copied into the work dir so that its "sim.h" is the compiled tree's). See fw_ota_upgrade_test.sh
[ -n "$SIM_DEV" ] && DEV=$(cd "$SIM_DEV" && pwd)
CORE=${SIM_CORE:-$HOME/mc_firmware/RAK3401_upstream}
CRYPTO=${SIM_CRYPTO:-$CORE/.pio/libdeps/RAK_3401_lora_ota/Crypto}
W=$(mktemp -d); trap 'rm -rf "${W:?}"' EXIT
mkdir -p "$W/src"
cp -r "$CORE/src/." "$W/src/"
cp -r "$DEV"/src/helpers/fw_ota/. "$W/src/helpers/fw_ota/"   # (Heltec port) -r: lzma/                 # the code under test
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
  "noserve": ("if (!force && readCtrl(cb) && failedForSlot(cb, h) && !installedImage(h)) {", "if (!force && !installedImage(h) && !bleFilled(h)) {"),
  # C83 (rc22.1 RAK): any FAILED record blocked every later image until the next 'fw apply' (H21 (2))
  "c83any": ("if (!force && readCtrl(cb) && failedForSlot(cb, h) && !installedImage(h)) {", "if (!force && readCtrl(cb) && cb.apply_state == APPLY_FAILED && !installedImage(h)) {"),
  # C83 (rc22.1 RAK): a Bluetooth-filled image the bootloader refused was still served (H21 (3))
  "c83ble": ("if (!force && readCtrl(cb) && failedForSlot(cb, h) && !installedImage(h)) {", "if (!force && readCtrl(cb) && failedForSlot(cb, h) && !installedImage(h) && !bleFilled(h)) {"),
  # rc17: a responder that hears a better link repair keeps sending (duplicate repairs)
  "repdup": ("  if (!_rep.sent) { stats.repairs_supp += _rep.n - _rep.i; _rep.i = _rep.n; return; }", ""),
  # rc18 (gap-fill study G): a pull by id stays pinned to its host (rc17) - MDP never finishes from a nearer holder (G1)
  "noadopt": [("  if (g_tune.req_adopt && !_img_set && !_fill && _img_ok && (nm.kind == KIND_APP || (nm.kind == KIND_BOOTLOADER && g_tune.bl_adopt))) {", "  if (false) {"),
              ("  if (g_tune.algo != ALGO_E || !g_tune.src_spread || !g_tune.req_adopt || _fill || m.kind != KIND_APP) return false;", "  return false;")],
  # rc18: holders gossip only 6 h (study default) - a node CRITICAL during the stream never gets the image (G2). RETIRED from release.sh in
  # rc25: a fleet pull is a campaign, and a node whose battery recovers pulls the image itself (island pull) - G2 passes even with this
  # mutant (checked 2026-11-08). That second path has its own mutants (islandnear, nostuck, P4). Kept here for study runs.
  "life6h": ("(camp || since(now, _tr_inc_ms) < (spread ? g_tune.gossip_life_ms : PASSIVE_QUIET_MS))", "(since(now, _tr_inc_ms) < 21600000u)"),
  # rc18: the source spreads only after an accepted offer - a fading tail whose answers are lost never starts (G3 by chance; H26 since rc23)
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
  "holderlive": ("    if (!hann && (!_fill || mode >= _fill_mode)) { _fill_h = false; _fann_heard_ms = millis() | 1; _fann_have = rhave;", "    _fann_heard_ms = millis() | 1; _fann_have = rhave; if (!hann && (!_fill || mode >= _fill_mode)) { _fill_h = false;"),
  # rc19 (C44): gap rounds ask for the first missing chunks, also ahead of a live stream (rc18) (H3)
  "gapahead": ("  const uint16_t lim = !live ? (uint16_t)_cm.n :", "  const uint16_t lim = true ? (uint16_t)_cm.n :"),
  # rc19 (C47): a resumed session shows "starting" at the server until its first request (H4)
  "c47": ("  if (!ds->top) ds->top = ptop;", ""),
  # rc19 (C42): a plain pull's listener missing more than 20 % when the stream stops asks its neighbours for all of it (a hidden full pull) (H7)
  "miss20": ("(miss * 5 > _cm.n || !reqHeard(now))", "(!reqHeard(now))"),
  # rc19: an announce with a LOWER mode (a neighbour's path pull, mode 1) demoted a fleet fill to passive: it paused for good (H5 by chance, H17)
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
  "tback": ("  if (d >= CLK_STEP_MIN_S || d <= -CLK_HOLD_MIN_S || _flood_host->clockChipFound()) _flood_host->clockDiscipline(ref);",
            "  if (d >= CLK_STEP_MIN_S || d <= -CLK_HOLD_MIN_S || _flood_host->clockChipFound()) _mesh->getRTCClock()->setCurrentTime(ref);"),
  # rc21 (C57): no bound - a peer 2 h off is taken (H15)
  "tbound": ("  if (d < -CLK_BOUND_S || (_clk_synced && d > CLK_BOUND_S)) { _clk_refused++; return; }", "  if (false) { _clk_refused++; return; }"),
  # rc22 (C74): the slot header (VALID) written after the CRC-32, BEFORE the SHA-256 (rc21) (H16)
  "c74": [("  // 2) SHA-256 over the payload in flash must match the manifest\n",
           "  if (!bl) { SlotHeader h0; memset(&h0, 0, sizeof(h0)); h0.raw_len = _man.raw_len; h0.raw_crc32 = _man.raw_crc32; h0.compression = _man.compression;\n"
           "    h0.fw_version = _man.fw_version; memcpy(h0.lzma_props, _man.lzma_props, sizeof(h0.lzma_props)); _writer.finishSparse(h0, _man.payload_len, pcrc); }\n"),
          ('  if (memcmp(out, _man.sha256, 32) != 0) { retryOrFail("SHA-256 mismatch"); return; }',
           '  if (memcmp(out, _man.sha256, 32) != 0) { if (!bl) eraseSlotHeader(); retryOrFail("SHA-256 mismatch"); return; }')],
  # rc22 (C74): 'fw apply' accepted while the image is still being checked (rc21) (H16)
  "applyverify": ('  if (_phase == PH_VERIFYING) { strcpy(reply, "ERR - still checking', '  if (false) { strcpy(reply, "ERR - still checking'),
  # rc22 (C65): auto-apply on for every node (rc21) (H16)
  "aadefault": ("  if (_fs && !_noapply && !_fs->exists(APPLY_FILE) && !_fs->exists(CIRCLE_FILE)) {", "  if (false) {"),
  # rc22 (C71): answers to another node's scan are dropped (rc21) (H16)
  "nooverheard": ("|| !g_tune.keep_overheard) return;", "|| true) return;"),
  # rc22 (C71): an overheard flooded answer keeps the ASKER's route to the holder (the route inside the answer) (H16)
  "overroute": ("    if (ret) { rtReverse(rev, ret, ret_enc); rt = rev; rl = ret_enc; }\n    else if", "    if (ret) { }\n    else if"),
  # rc22 (C72): 'fw pull-resume duty x' on a running pull does not ask the server again (H16)
  "c72": ("      _reagree_at_ms = millis() | 1; _reagree_why = true;\n      int w = snprintf(reply, 160, \"OK - duty %u.%02u at", "      int w = snprintf(reply, 160, \"OK - duty %u.%02u at"),
  # rc22 (C73): 'fw pull-status' shows only the own pull (rc21) (H16)
  "c73": ("  if (_resolving || _phase != PH_IDLE) appendServing(reply);", ""),
  # rc22 (C70): the first 'fw list' only says "no scan yet" (rc21) (C37)
  "noautoscan": ("  if (!n && !_scanned) return scanCmd(reply, true);", ""),
  # rc22 bench (C77): a late copy of our own previous window's chunk started a "shared stream" - the only puller stalled (H18)
  "mixfleet": ("  if (len != FANN_LEN || memcmp(&b[2], &b[32], 4) != 0 || memcmp(&b[69], g_fwpull_tag.board, 8) != 0) return;",
               "  if (len != FANN_LEN || memcmp(&b[2], &b[32], 4) != 0) return;"),
  "mixlist": ("    if (memcmp(&b[37], g_fwpull_tag.board, 8) != 0) {           // a server of another board",
              "    if (false) {           // a server of another board"),
  "mixtext": ("        fail(why);\n      }\n      return;\n    }", "      }\n      return;\n    }"),
  "c75": ("    else if ((*e == 'm' || *e == 'M') && !mins) { mins = true; m += v; }", "    else if (false) { }"),
  # Heltec bench (H20): awake 60 s after ANY LoRa-OTA packet - a neighbour that only overheard a transfer stayed awake all along
  "awakeany": [("bool FwOtaTransfer::needsAwake() const {", "static uint32_t s_aw;\nbool FwOtaTransfer::needsAwake() const {"),
               ("  return _rep.i < _rep.n;  ", "  return _rep.i < _rep.n || (s_aw && (int32_t)(s_aw - millis()) > 0);  "),
               ("  if (len < 2 + XFER_MAC_LEN || b[0] != XFER_MAGIC || !_circle_set) return;   // (v1.17.28) no circle = hidden: nothing is answered\n",
                "  if (len < 2 + XFER_MAC_LEN || b[0] != XFER_MAGIC || !_circle_set) return;   // (v1.17.28) no circle = hidden: nothing is answered\n  s_aw = (millis() + 60000) | 1;\n")],
  # (H20) never awake for LoRa-OTA: the pull waits out the sleeps / the server's paced window is not kept awake
  "noawake": ("bool FwOtaTransfer::needsAwake() const {", "bool FwOtaTransfer::needsAwake() const { return false;"),
  "noqueue": ("  for (uint8_t k = 0; k < DUTY_SESS_N; k++) if (_dsess[k].sess && _dsess[k].q_i < _dsess[k].q_n) return true;", ""),
  # T9 (rc22.1): a new pull sent its first request at once - the CLI reply (600 ms later) met the server's answer (H22)
  "c55p": ("say what runs, not an error\n      statusLine(reply);", "say what runs, not an error\n      strcpy(reply, \"ERR - that is the server this pull already uses\");"),
  "t9": ("  _last_req_ms = millis(); _retry_timeout_ms = PULL_START_MS;", "  _last_req_ms = 0; _retry_timeout_ms = 0;"),
  # C76 (rc22.1): a found but unset chip said "no RTC" (H23)
  "c76": ('_flood_host && _flood_host->clockChipFound() ? "RTC chip found, not set" : "no RTC"', '"no RTC"'),
  # C76 (rc22.1): an unset chip was written only when the software clock was off by a step - one in step stayed unset (H23 U2)
  "c76w": (" || _flood_host->clockChipFound()) _flood_host->clockDiscipline(ref);", ") _flood_host->clockDiscipline(ref);"),
  # (owner 2026-11-04) mid-pull 'fw pull fw-302' (another image) answered ERR instead of the running pull's status (H24)
  "busyver": [("    if (pullVer() && v != pullVer()) { statusLine(reply); return true; }", "    if (false) { statusLine(reply); return true; }"),
              ("    if (cur_ver && p.fw_version != cur_ver) { statusLine(reply); return true; }", "    if (cur_ver && p.fw_version != cur_ver) { strcpy(reply, \"ERR - peer has another image\"); return true; }")],
  # (2026-11-04) a refused/answered 'fw pull' replaced the running pull's route with the other holder's before the checks (H24)
  "busyroute": ("  if (in_pull) {                                          // source switch pre-flight",
                "  if (!_route_manual && strcmp(p.name, \"(not listed)\") != 0) { if (rtBytes(p.route_len)) memcpy(_route, p.route, rtBytes(p.route_len)); _route_len = p.route_len; }\n  if (in_pull) {                                          // source switch pre-flight"),
  # (owner 2026-11-04) the pull-status line did not name the image version (H24)
  "nover": ("    else if (const uint32_t v = pullVer()) snprintf(what", "    else if (const uint32_t v = 0) snprintf(what"),
  # (rc23, C91) the requester counted dead after 3 lost announces (its only path to the listeners: the server's one echo) (H25 seed 7)
  "c91alive": ("static const uint32_t FANN_ALIVE_MS = 5 * FANN_IVL_MS + 60000;", "static const uint32_t FANN_ALIVE_MS = 3 * FANN_IVL_MS + 60000;"),
  # (rc23, C91) a stream that still moves on is no sign of its requester's life (H25 seed 53)
  "c91strm": ("      || (since(now, _fann_heard_ms) < PASSIVE_QUIET_MS && _strm_top_ms", "      || (false && _strm_top_ms"),
  # (rc23, C91) a fill begun from a holder's announce takes the holder's "all chunks" as the stream's requester being done
  "c91hold": [("    if (hann) _fc.have = 0;", "    if (false) _fc.have = 0;"), ("  if (!rep && _fill_h && _fill && !_sess_set) {", "  if (false) {")],
  "c77": ("  if (!own && !late && !rep_for_req && !(rep && _fill_paused)) {", "  if (!own && !rep_for_req && !(rep && _fill_paused)) {"),
  # (rc25, C95) fw push / the campaign (scenarios P1-P10)
  "joinnewer": ("bool FwOtaTransfer::campNewerHere(const XferManifest& m) const {", "bool FwOtaTransfer::campNewerHere(const XferManifest& m) const {\n  if (m.fw_version) return false;"),
  "joinboard": ("  if (memcmp(&b[69], g_fwpull_tag.board, 8) != 0) return; // another board's campaign (a query names the asker's board)", ""),
  "campapply": ("  else if (_camp.island && campImg(_man.sha256)) {", "  else if (false) {"),
  "islandnear": ("  _camp_island_ms = (now + (uint32_t)(8 - h) * g_tune.island_step_ms", "  _camp_island_ms = (now + (uint32_t)(1 + h) * g_tune.island_step_ms"),
  "nocampsave": ("  if (!_camp.set) return;\n  File f = _fs->open(CAMP_FILE, FILE_O_WRITE);", "  return;\n  File f = _fs->open(CAMP_FILE, FILE_O_WRITE);"),
  "nostuck": ("void FwOtaTransfer::campResult(uint8_t st) {\n  if (!_camp.set || _camp.mine) return;", "void FwOtaTransfer::campResult(uint8_t st) {\n  if (st == CR_STUCK || !_camp.set || _camp.mine) return;"),
  "noquery": ("campFlood(CK_QUERY, _camp_q_nonce, 100); stats.camp_queries++;", "stats.camp_queries++;"),
  "noremsupp": ("if (!(_camp_rem_heard_ms && since(now, _camp_rem_heard_ms) < g_tune.remind_ms) && powerState() != 3)", "if (powerState() != 3)"),
  "campcancel": ("    if (same) { if (!_camp.cancelled) { _camp.cancelled = 1; campSave(); campStopLocal(); } return; }", "    if (same) return;"),
  # (rc25, C99 stage 2, fw_ota_upgrade_test.sh LOAD side only) the state file format changed without reading the old one
  "c99state": ("static const uint32_t STATE_MAGIC      = 0x38585746;", "static const uint32_t STATE_MAGIC      = 0x39585746;"),
  "nofleetcamp": ("    if (_fleet_mode == FLEET_REPLACE && _man.kind == KIND_APP && _circle_set) campStart(_man);", "    if (false) campStart(_man);"),
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
    "-Dg_xfer=(*sim_cur_xfer)" "-Dg_wdt=(*sim_cur_wdt)" "-Dg_fwpull_tag=(*sim_cur_tag)"   # (Heltec) a board id per node
    -I"$DEV/test/sim/shim" -I"$W/src" -I"$CORE/lib/ed25519" -I"$CRYPTO" -I"$DEV/test/sim")
SRCS=("$W/src/Mesh.cpp" "$W/src/Dispatcher.cpp" "$W/src/Packet.cpp" "$W/src/Identity.cpp" "$W/src/Utils.cpp"
      "$W/src/helpers/StaticPoolPacketManager.cpp"
      "$W/src/helpers/fw_ota/FwOtaTransfer.cpp" "$W/src/helpers/fw_ota/FwOtaSlots.cpp" "$W/src/helpers/fw_ota/RadioDiag.cpp"
      "$DEV/test/sim/sim.cpp" "$DEV/test/sim/sim_rng.cpp" "$DEV/test/sim/audit.cpp")
if [ -n "$SIM_MAIN" ]; then cp "$SIM_MAIN" "$W/sim_main.cpp"; SRCS+=("$W/sim_main.cpp"); else SRCS+=("$DEV/test/sim/scenarios.cpp"); fi
for f in Crypto Hash SHA256 SHA512 AES128 AESCommon BlockCipher Ed25519 Curve25519 BigNumberUtil; do SRCS+=("$CRYPTO/$f.cpp"); done
for f in "$CORE"/lib/ed25519/*.c; do gcc -c "${COPT[@]}" -w -I"$CORE/lib/ed25519" "$f" -o "$W/$(basename "$f").o"; done
g++ "${CX[@]}" "${SRCS[@]}" "$W"/*.c.o -o "$W/sim" 2>&1 | grep -E "error|undefined" | head -40 || true
test -x "$W/sim" || { echo "FAIL: the simulator did not build"; exit 1; }
[ -n "$SIM_KEEP" ] && cp "$W/sim" "$SIM_KEEP"                                   # SIM_KEEP=<path>: keep the binary (debugging)
# (T8, owner 2026-11-02) a full run (no scenario list) runs every scenario as its own process, SIM_JOBS (default: cores - 2) at a time, then sums the parts:
# the same checks and the same last line ("simulator: N checks passed, M failed"); SIM_JOBS=1 = the old single process
J=${SIM_JOBS:-$(( $(nproc) > 3 ? $(nproc) - 2 : 1 ))}   # default: all cores but two (test host: 16 -> 14)
if [ $# -eq 0 ] && [ "$J" -gt 1 ]; then
  SC=$(grep -oE 'if \(want\("[A-Z0-9]+"\)\)' "$DEV/test/sim/scenarios.cpp" | grep -oE '"[A-Z0-9]+"' | tr -d '"')
  [ "$(echo "$SC" | wc -l)" -ge 40 ] || { echo "FAIL: scenario list from scenarios.cpp looks wrong: $(echo $SC)"; exit 1; }
  mkdir -p "$W/parts"
  echo "$SC" | ASAN_OPTIONS=detect_leaks=0 SIM_PART=1 xargs -P "$J" -I{} sh -c '"$1" {} > "$2/{}.out" 2>&1; echo $? > "$2/{}.rc"' _ "$W/sim" "$W/parts"
  P=0; F=0; AF=0; AT=0; BAD=""
  for s in $SC; do
    cat "$W/parts/$s.out"
    l=$(grep -E "^simulator: [0-9]+ checks passed, [0-9]+ failed" "$W/parts/$s.out" | tail -1)
    a=$(grep -E "^SIM_PART audit " "$W/parts/$s.out" | tail -1)
    if [ -z "$l" ] || [ -z "$a" ]; then BAD="$BAD $s"; F=$((F+1)); continue; fi   # crashed / sanitizer stop = one failed check
    set -- $l; P=$((P+$2)); F=$((F+$5)); pf=$5; set -- $a; AF=$((AF+$3)); AT=$((AT+$4))
    [ "$(cat "$W/parts/$s.rc")" = 0 ] || [ "$pf" != 0 ] || { BAD="$BAD $s"; F=$((F+1)); }   # exit code != 0 with 0 failed checks
  done
  [ -z "$BAD" ] || echo "  FAIL: scenario process(es) ended without a result:$BAD"
  if [ "$AF" -eq 0 ]; then P=$((P+1)); else F=$((F+1)); echo "  FAIL text audit: $AF of $AT status texts contradict the node's state (see AUDIT FAIL)"; fi
  echo "  text audit: $AT status texts read and compared with the node state"
  echo "simulator: $P checks passed, $F failed"
  [ "$F" -eq 0 ]; exit $?
fi
ASAN_OPTIONS=detect_leaks=0 "$W/sim" "$@"   # MeshCore's packet manager has no destructor (never freed on the device; the simulator reboots nodes)
