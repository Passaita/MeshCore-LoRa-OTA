/*
 * fw_ota CLI.
 *
 * Wired into MyMesh::handleCommand behind -D FW_OTA, for the serial console AND
 * the authenticated remote-admin session (phone app / companion). `remote` marks
 * a mesh session: `fw stage` is refused there (it takes over the serial stream).
 * Every reply must fit 160 bytes. Dev-only commands (fill, pending, stage,
 * serve force, ctl init, the 'fw pull *' form) are built only with -D FW_OTA_DEV (rc7).
 *
 * Single slot (2026-09-16, supersedes the earlier A/B commands below - no
 * more slot letter argument anywhere):
 *   fw geo                 - print the flash geometry this build was linked for
 *   fw ctl                 - dump the control page
 *   fw ctl init             - write a fresh IDLE control page (dev)
 *   fw slot                - dump the slot header + validity
 *   fw fill <kib>           - (dev) erase slot, write <kib> KiB of a test pattern,
 *                            finalise the header with the correct CRC-32
 *   fw verify               - recompute CRC over the slot payload, report match
 *   fw pending               - (dev) set control page: apply on next boot
 *   fw erase                - mark the slot empty (also in release builds: clears the slot)
 */
#pragma once

#include <Arduino.h>
#include "FwOtaSlots.h"
#include "FwOtaTransfer.h"
#include "RadioDiag.h"

namespace fw_ota {

// "fw help" (topic list) and "fw help <cmd>" (one command, argument-by-argument); every reply fits 160 bytes.
static inline bool cliHelp(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (*arg == 0) {
    strcpy(reply, "fw help <cmd>: passphrase peers list pull pull-ble blupdate pull-status pull-pause pull-resume pull-abort route serve duty "
                  "apply autoapply bl radio slot erase");
    return true;
  }
  if (!strcmp(arg, "peers")) {   // (rc17, C38, owner) 'fw peers' asks the peers; 'fw list' lists what they offer
    strcpy(reply, "fw peers - scan ~25s for peers offering firmware (own mesh + flooded discovery through relays); then 'fw list'. Far nodes: scan twice"); return true;
  }
  if (!strcmp(arg, "list")) {
    strcpy(reply, "fw list [n] - the circle's images of the last 'fw peers' scan, newest first: #n fw-X xHolders (hops). 'fw list hosts [n]' = per holder"); return true;
  }
  if (!strcmp(arg, "pull")) {
    strcpy(reply, "fw pull <#n|fw-X|id> [duty x] [fleet] [pause Nh] - #n/fw-X: best holder + failover. fleet: hearers replace older slots. pause: start later"); return true;
  }
  if (!strcmp(arg, "blupdate")) {
    strcpy(reply, "fw blupdate [force] <id|#n> - pull a peer's BOOTLOADER (your circle); id AABB/AABBCC found by a scan (clash = refused); force = same/older rev too"); return true;
  }
  if (!strcmp(arg, "pull-status")) {
    strcpy(reply, "fw pull-status - progress of the current pull: chunks, rate, ETA, or the last result (COMPLETE/FAILED/idle)"); return true;
  }
  if (!strcmp(arg, "pull-pause")) {
    strcpy(reply, "fw pull-pause [Nh] - stop sending (chunks kept, server share freed) until pull-resume, or N hours (1-168)"); return true;
  }
  if (!strcmp(arg, "pull-resume")) {
    strcpy(reply, "fw pull-resume - continue a paused pull from the first missing chunk; a paused fill ('fw slot': partial) is finished from the nearest holder"); return true;
  }
  if (!strcmp(arg, "pull-abort")) {
    strcpy(reply, "fw pull-abort - cancel a waiting/in-progress pull now and clear its saved state; a fleet pull also stops its listeners (repeated 30 min)"); return true;
  }
  if (!strcmp(arg, "route")) {
    strcpy(reply, "fw route [AA,BB..[/way back]|auto|off] - no arg: the pull's route pairs (*=in use, ok/unanswered, ms). Typed = manual, also mid-pull"); return true;
  }
  if (!strcmp(arg, "serve")) {
    strcpy(reply, "fw serve [on|off] - no arg: what is offered. on: the bootloader + the slot image if installed. off: nothing (persists)"); return true;
  }
  if (!strcmp(arg, "duty")) {
    strcpy(reply, "fw duty [0.1-1.0] - LoRa-OTA's channel share, relays included: 1.0 ~30%, default 0.3 ~9%; a pull runs at min(pull's, server's); yields to mesh"); return true;
  }
  if (!strcmp(arg, "passphrase")) {
    strcpy(reply, "fw passphrase <phrase>|off - your circle: LoRa-OTA only works between nodes with the same phrase (none = off). No arg: fingerprint"); return true;
  }
  if (!strcmp(arg, "pull-ble")) {
    strcpy(reply, "fw pull-ble - restart into Bluetooth upload INTO THE SLOT (bootloader rev 5+): phone DFU app, the -slot.zip; then served. 5 min idle = back"); return true;
  }
  if (!strcmp(arg, "apply")) {
    strcpy(reply, "fw apply - install the slot image on this node: marks it pending and restarts; the bootloader applies it (= fw pending + reboot)"); return true;
  }
  if (!strcmp(arg, "autoapply")) {
    strcpy(reply, "fw autoapply on|off - apply a completed, verified pull automatically (set pending + reboot); default on"); return true;
  }
  if (!strcmp(arg, "minbatt")) {
    strcpy(reply, "fw minbatt [mV|off] - below it this node is a LOW holder (pullers take it last); 150 mV below CRITICAL: it stops serving"); return true;
  }
  if (!strcmp(arg, "wdt")) {
    strcpy(reply, "fw wdt [clear] - hardware watchdog: restarts a node whose main loop hangs (10 min); last reset reason, watchdog resets; clear = 0"); return true;
  }
  if (!strcmp(arg, "bl")) {
    strcpy(reply, "fw bl - the running bootloader: revision, size, CRC, and whether it is currently offered to peers (no argument)"); return true;
  }
  if (!strcmp(arg, "time")) {
    strcpy(reply, "fw time [ask] - time to the second + its source; a node without an RTC takes it from a circle neighbour with one (ask = now)"); return true;
  }
  if (!strcmp(arg, "radio")) {
    strcpy(reply, "fw radio [clear] - radio receive record (kept across reboots): out of Rx mode > 8 s, nothing heard > 10 min; clear = reset"); return true;
  }
  if (!strcmp(arg, "slot")) {
    strcpy(reply, "fw slot - what the slot holds (like 'fw bl'): fw-N, size, CRC of the app, installed here or not, offered or not"); return true;
  }
  if (!strcmp(arg, "verify")) {
    strcpy(reply, "fw verify - recompute the CRC-32 over the slot payload and compare it to the header (no argument)"); return true;
  }
  if (!strcmp(arg, "ctl")) {
    strcpy(reply, "fw ctl - dump the control page (apply state, boot attempts, fail count, last result)"); return true;
  }
  if (!strcmp(arg, "geo")) {
    strcpy(reply, "fw geo - print the flash geometry this build was linked for: app region, slot address/size, max payload, control page (no argument)"); return true;
  }
#ifdef FW_OTA_DEV
  if (!strcmp(arg, "fill")) {
    strcpy(reply, "fw fill <kib> - dev build only: erase the slot and write <kib> KiB of a test pattern with a valid header (for pull/apply tests)"); return true;
  }
#endif
  if (!strcmp(arg, "erase")) {
    strcpy(reply, "fw erase - mark the slot empty (wipes its header; not during a pull). The node then serves only its bootloader"); return true;
  }
#ifdef FW_OTA_DEV
  if (!strcmp(arg, "pending")) {
    strcpy(reply, "fw pending - mark the control page 'apply on next boot' for whatever is in the slot now (no argument)"); return true;
  }
#endif
#ifdef FW_OTA_DEV
  if (!strcmp(arg, "stage")) {
    strcpy(reply, "fw stage <args> - serial console only: host script writes a real image into the slot over USB (see tools/fw_ota_stage.py)"); return true;
  }
#endif
  snprintf(reply, 160, "fw help: no such command '%s' - 'fw help' lists them", arg);
  return true;
}

// returns true if the command was a "fw ..." command (handled here)
static inline bool cliHandle(const char* command, char* reply, bool remote = false) {
  (void)remote;                                           // used only by the dev-only 'fw stage'
  if (strncmp(command, "fw", 2) != 0 || (command[2] != ' ' && command[2] != 0)) {
    return false;
  }
  const char* arg = command + 2;
  while (*arg == ' ') arg++;
  // (rc7, C15) a phone keyboard's autofill adds a trailing space ("fw pull-status "): drop trailing blanks before matching
  char abuf[200];
  size_t al = strlen(arg); if (al >= sizeof(abuf)) al = sizeof(abuf) - 1;
  while (al && (arg[al - 1] == ' ' || arg[al - 1] == '\t' || arg[al - 1] == '\r' || arg[al - 1] == '\n')) al--;
  memcpy(abuf, arg, al); abuf[al] = 0; arg = abuf;

  if (strcmp(arg, "geo") == 0) {
    snprintf(reply, 160,
             "app %06lX-%06lX(%luK) slot@%06lX %luK payload<=%luB ctrl@%06lX",
             (unsigned long)APP_REGION_START, (unsigned long)APP_REGION_END,
             (unsigned long)(APP_MAX_SIZE / 1024),
             (unsigned long)SLOT_ADDR,
             (unsigned long)(SLOT_SIZE / 1024), (unsigned long)SLOT_PAYLOAD_MAX,
             (unsigned long)CTRL_ADDR);
    return true;
  }

  if (strcmp(arg, "ctl") == 0) {
    CtrlBlock b;
    if (!readCtrl(b)) { strcpy(reply, "ctrl: blank / no magic"); return true; }
    snprintf(reply, 160, "ctrl state=%lu attempts=%lu fails=%lu last=%lu",
             (unsigned long)b.apply_state, (unsigned long)b.boot_attempts,
             (unsigned long)b.fail_count, (unsigned long)b.last_result);
    return true;
  }

#ifdef FW_OTA_DEV
  if (strncmp(arg, "ctl init", 8) == 0) {
    strcpy(reply, ctrlInit() ? "OK - ctrl initialised" : "ERR - ctrl write failed");
    return true;
  }
#endif

  if (strcmp(arg, "slot") == 0) return g_xfer.cmdSlot(reply);   // (rc17, C39) in the form of 'fw bl'


#ifdef FW_OTA_DEV
  if (strncmp(arg, "fill", 4) == 0) {
    const char* p = arg + 4;
    while (*p == ' ') p++;
    long kib = strtol(p, nullptr, 10);
    if (kib <= 0) { strcpy(reply, "usage: fw fill <kib>"); return true; }
    uint32_t len = (uint32_t)kib * 1024;
    if (len > SLOT_PAYLOAD_MAX) { snprintf(reply, 160, "ERR - max %lu KiB", (unsigned long)(SLOT_PAYLOAD_MAX / 1024)); return true; }

    uint32_t t0 = millis();
    static SlotWriter w;
    if (!w.begin()) { strcpy(reply, "ERR - header erase failed"); return true; }

    uint8_t buf[512];
    uint32_t lcg = 0x1234567u;
    uint32_t off = 0;
    bool ok = true;
    while (off < len && ok) {
      uint32_t n = (len - off) < sizeof(buf) ? (len - off) : (uint32_t)sizeof(buf);
      for (uint32_t i = 0; i < n; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(lcg >> 24);
      }
      ok = w.write(buf, n);
      off += n;
    }
    if (!ok) { snprintf(reply, 160, "ERR - write failed at %lu B", (unsigned long)off); return true; }

    SlotHeader h;
    memset(&h, 0, sizeof(h));
    h.raw_len     = len;      // COMP_NONE: raw == payload
    h.compression = COMP_NONE;
    h.fw_version  = millis();
    if (!w.finish(h)) { strcpy(reply, "ERR - finish/header failed"); return true; }

    snprintf(reply, 160, "OK - wrote %lu B in %lu ms, crc=%08lX, %s",
             (unsigned long)len, (unsigned long)(millis() - t0), (unsigned long)w.crc,
             slotValid() ? "VALID" : "INVALID(!)");
    return true;
  }
#endif

  if (strncmp(arg, "verify", 6) == 0) {
    SlotHeader h;
    if (!readSlotHeader(h)) { strcpy(reply, "slot empty"); return true; }
    uint32_t c = crc32_flash(SLOT_ADDR + SLOT_PAYLOAD_OFFSET, h.payload_len);
    snprintf(reply, 160, "hdr crc=%08lX  recomputed=%08lX  -> %s",
            (unsigned long)h.payload_crc32, (unsigned long)c,
            (c == h.payload_crc32) ? "MATCH" : "MISMATCH");
    return true;
  }

#ifdef FW_OTA_DEV
  if (strncmp(arg, "pending", 7) == 0) {
    strcpy(reply, ctrlMarkPending() ? "OK - apply pending on next boot" : "ERR - ctrl write failed");
    return true;
  }
#endif

  if (strcmp(arg, "erase") == 0) {
    // just wipe the header page -> slot reads as empty (fill/pull erase as they write). Serving stops by itself: every request re-checks the
    // slot header (imageStillServed). (rc7) Also in release builds - it clears the slot, e.g. for a swarm upload - but never under a pull.
    if (g_xfer.slotInUse()) { strcpy(reply, "ERR - a pull/stage is using the slot ('fw pull-abort' first)"); return true; }
    strcpy(reply, eraseSlotHeader() ? "OK - slot marked empty (serving only the bootloader now)" : "ERR - erase failed");
    return true;
  }

  // ---- LoRa transfer between neighbours ----

  if (strncmp(arg, "serve off", 9) == 0) return g_xfer.cmdServeOff(reply);
#ifdef FW_OTA_DEV
  if (strcmp(arg, "serve force") == 0)   return g_xfer.cmdServe(reply, true);
#endif
  if (strcmp(arg, "serve on") == 0)      return g_xfer.cmdServe(reply);         // (rc4, C9) serve what is available
  if (strcmp(arg, "serve") == 0)         return g_xfer.cmdServeStatus(reply);   // (rc4, C11) status

  if (strncmp(arg, "route", 5) == 0)     return g_xfer.cmdRoute(arg + 5, reply);
  // (rc17, C38, owner) 'fw peers' = query the peers (the scan); 'fw list [n]' / 'fw list hosts [n]' = what the circle offers
  if (strcmp(arg, "peers") == 0)         return g_xfer.cmdList(reply);
  if (strncmp(arg, "peers ", 6) == 0) { strcpy(reply, "'fw peers' scans (no argument); the images: 'fw list [n]', per holder: 'fw list hosts [n]'"); return true; }
  if (strncmp(arg, "list", 4) == 0 && (arg[4] == 0 || arg[4] == ' ')) return g_xfer.cmdPeers(arg + 4, reply);
  if (strncmp(arg, "autoapply", 9) == 0) return g_xfer.cmdAutoApply(arg + 9, reply);
  if (strncmp(arg, "wdt", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) return g_xfer.cmdWdt(arg + 3, reply);   // (rc8, C23)
  if (strncmp(arg, "minbatt", 7) == 0 && (arg[7] == 0 || arg[7] == ' ')) return g_xfer.cmdMinBatt(arg + 7, reply);   // (swarm v1)
  if (strcmp(arg, "pull-status") == 0)   return g_xfer.cmdStatus(reply);
  if (strcmp(arg, "pull-abort") == 0)    return g_xfer.cmdAbort(reply);
  if (strncmp(arg, "pull-pause", 10) == 0 && (arg[10] == 0 || arg[10] == ' ')) return g_xfer.cmdPause(arg + 10, reply);   // (v1.17.28, C8)
  if (strcmp(arg, "pull-resume") == 0)   return g_xfer.cmdResume(reply);
  if (strcmp(arg, "bl") == 0)            return g_xfer.cmdBlInfo(reply);
  if (strncmp(arg, "radio", 5) == 0 && (arg[5] == 0 || arg[5] == ' ')) return g_radiodiag.cmd(arg + 5, reply);
  if (strncmp(arg, "time", 4) == 0 && (arg[4] == 0 || arg[4] == ' ')) return g_xfer.cmdTime(arg + 4, reply);   // (rc21, C57)
  if (strncmp(arg, "duty", 4) == 0 && (arg[4] == 0 || arg[4] == ' ')) return g_xfer.cmdDuty(arg + 4, reply);
  if (strncmp(arg, "passphrase", 10) == 0 && (arg[10] == 0 || arg[10] == ' ')) return g_xfer.cmdPassphrase(arg + 10, reply);   // (v1.17.28)
  if (strcmp(arg, "pull-ble") == 0)      return g_xfer.cmdPullBle(reply);   // (v1.17.28) before "pull": BLE upload into the slot
  if (strcmp(arg, "apply") == 0)         return g_xfer.cmdApply(reply);     // (v1.17.28) install the slot image here
  // (v1.17.25) "fw pull|blupdate ... duty x": the duty for THIS pull (overrides the node's 'fw duty', the server's still caps it).
  // (rc17) options after the id, in any order: duty x | fleet (nodes that hear the stream replace older slot images) | pause Nh (C37: the
  // pull is set up and paused before its first offer). Other words are kept (an old habit: a password after the id is ignored later).
  char dbuf[160];
  if ((strncmp(arg, "pull", 4) == 0 && (arg[4] == ' ' || arg[4] == 0)) || strncmp(arg, "blupdate", 8) == 0) {   // not pull-status / pull-abort
    g_xfer.setNextPullDuty(0); g_xfer.setNextPullOpts(false, 0);
    const bool isbl = strncmp(arg, "blupdate", 8) == 0;
    char src[160]; strncpy(src, arg, sizeof(src) - 1); src[sizeof(src) - 1] = 0;
    size_t o = 0; bool fleet = false; uint8_t ph = 0;
    for (char* t = strtok(src, " "); t; t = strtok(nullptr, " ")) {
      if (!strcmp(t, "duty")) {
        char* v = strtok(nullptr, " "); uint8_t p = v ? FwOtaTransfer::parseDuty(v) : 0;
        if (!p) { strcpy(reply, "ERR - duty 0.1 .. 1.0 (e.g. 'fw pull #1 duty 0.5')"); return true; }
        g_xfer.setNextPullDuty(p); continue;
      }
      if (!isbl && !strcmp(t, "fleet")) { fleet = true; continue; }
      if (!isbl && !strcmp(t, "pause")) {
        char* v = strtok(nullptr, " "); char* e = nullptr; long h = v ? strtol(v, &e, 10) : 0;
        if (!v || e == v || (*e != 'h' && *e != 'H') || e[1] || h < 1 || h > 168) { strcpy(reply, "ERR - pause Nh, N = 1..168 hours (e.g. 'fw pull #1 pause 4h')"); return true; }
        ph = (uint8_t)h; continue;
      }
      o += (size_t)snprintf(dbuf + o, sizeof(dbuf) - o, "%s%s", o ? " " : "", t);
      if (o >= sizeof(dbuf)) o = sizeof(dbuf) - 1;
    }
    dbuf[o] = 0; arg = dbuf;
    g_xfer.setNextPullOpts(fleet, ph);
  }
  if (strncmp(arg, "blupdate", 8) == 0)  return g_xfer.cmdPullPeer(arg + 8, reply, true);   // [force] <id|#n>: the offered BOOTLOADER
  if (strncmp(arg, "pull", 4) == 0) {
    // fw pull <id|#n>
    const char* p = arg + 4;
    while (*p == ' ') p++;
    char name[32] = {0};
    uint32_t i = 0;
    while (*p && *p != ' ' && i < sizeof(name) - 1) name[i++] = *p++;
    if (name[0] == 0) { strcpy(reply, "usage: fw pull <id|#n> [duty x] [fleet] [pause Nh]   (see 'fw list')"); return true; }
#ifdef FW_OTA_DEV
    // dev form: '*' only. (rc16) 'fw-N' is the swarm version form here too - the dev form sent it UNTARGETED, and a neighbour serving
    // another image answered NAK and failed the pull (bench 2026-10-07: board 3 vs the twin serving fw-9300)
    if (name[0] == '*') return g_xfer.cmdPull(name, reply);
#endif
    return g_xfer.cmdPullPeer(p - strlen(name), reply, false);
  }

  // ---- local staging over serial (host script writes a real image) ----
#ifdef FW_OTA_DEV
  if (strncmp(arg, "stage", 5) == 0) {
    if (remote) { strcpy(reply, "ERR - fw stage works on the serial console only"); return true; }
    return g_xfer.cmdStage(arg + 5, reply);
  }
#endif

  if (strncmp(arg, "help", 4) == 0) return cliHelp(arg + 4, reply);

  // Every reply must fit the 160-byte reply buffers (serial: char reply[160]; remote: 161 bytes in temp[166]). This usage line had grown to
  // 194 bytes (v1.17.15-19): an unknown or mistyped 'fw' command (e.g. "fw status") overran the buffer and froze the node - also remotely.
  static const char USAGE[] = "fw: passphrase|peers|list|pull|pull-ble|blupdate|pull-status|pull-pause|pull-resume|pull-abort|route|serve|duty|apply|"
                              "autoapply|bl|slot - 'fw help'";
  static_assert(sizeof(USAGE) <= 160, "fw usage reply must fit the 160-byte reply buffer");
  strcpy(reply, USAGE);
  return true;
}

}  // namespace fw_ota
