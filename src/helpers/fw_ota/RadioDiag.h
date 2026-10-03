/*
 * RadioDiag - MEASURING ONLY (v1.17.23): records when the radio stops receiving, so a "deaf radio" can be diagnosed afterwards.
 *
 * 2026-09-27: after a remote `advert` (tx 18) both bench boards stopped receiving: CPU and USB alive, rx counters frozen, Dispatcher error
 * flag 4 (radio not back in Rx mode for > 8 s); only a reboot recovered them - and the reboot erased the evidence. This keeps it, in a
 * small file that survives the reboot. It changes nothing in the radio's behaviour (lesson of Pager 4.3: measure first, then fix).
 *
 * Two events:
 *  - "nonRX": the radio is out of Rx mode for more than 8 s (the Dispatcher's STARTRX_TIMEOUT condition);
 *  - "silent": no packet received for more than 10 min - noted with whether the radio claimed to be in Rx mode at that moment and how
 *    many packets were sent during the silence (relaying stops too when nothing is heard; own adverts do not).
 * `fw radio` shows them (also remotely); `fw radio clear` resets the saved record.
 */
#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM / File

namespace fw_ota {

class RadioDiag {
public:
  void begin(FILESYSTEM* fs, mesh::RTCClock* rtc);
  void loop(bool in_rx, uint32_t n_recv, uint32_t n_sent);   // every main loop: cheap
  bool cmd(const char* arg, char* reply);                    // "fw radio [clear]" (reply <= 160 B)

private:
  struct Saved {                        // persisted (/fwota.radio); counts accumulate across reboots until 'fw radio clear'
    uint32_t magic;
    uint16_t nonrx_n, silent_n;
    uint32_t nonrx_max_ms;              // longest non-Rx stretch seen (of those > 8 s)
    uint32_t nonrx_epoch;               // when the last one started (RTC epoch; 0 = unknown)
    uint32_t silent_max_s;              // longest silence seen (of those > 10 min)
    uint32_t silent_epoch;              // when the last silence crossed 10 min
    uint16_t silent_sent;               // packets sent during that silence (until it crossed 10 min)
    uint8_t  silent_inrx;               // the radio said "Rx mode" when it crossed 10 min (1) or not (0)
    uint8_t  pad;
  } _s = {};
  FILESYSTEM*     _fs = nullptr;
  mesh::RTCClock* _rtc = nullptr;
  uint32_t _nonrx_start = 0; bool _nonrx_flag = false;
  uint32_t _last_rx_ms = 0, _last_recv = 0, _sent_at_rx = 0, _last_sent = 0; bool _silent_flag = false;
  bool     _started = false;
  void save();
  uint32_t epoch() const;
};

extern RadioDiag g_radiodiag;

}  // namespace fw_ota
