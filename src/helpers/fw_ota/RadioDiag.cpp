#include "RadioDiag.h"

namespace fw_ota {

RadioDiag g_radiodiag;

static const char*    RADIO_FILE   = "/fwota.radio";
static const uint32_t RADIO_MAGIC  = 0x31445252;          // "RRD1"
static const uint32_t NONRX_MS     = 8000;                // the Dispatcher's "radio not in Rx mode" limit
static const uint32_t SILENT_MS    = 10UL * 60 * 1000;    // no packet received for this long

void RadioDiag::begin(FILESYSTEM* fs, mesh::RTCClock* rtc) {
  _fs = fs; _rtc = rtc;
  memset(&_s, 0, sizeof(_s));
  if (_fs && _fs->exists(RADIO_FILE)) {
    File f = _fs->open(RADIO_FILE);
    if (f) { if (f.read((uint8_t*)&_s, sizeof(_s)) != (int)sizeof(_s) || _s.magic != RADIO_MAGIC) memset(&_s, 0, sizeof(_s)); f.close(); }
  }
  _s.magic = RADIO_MAGIC;
}

uint32_t RadioDiag::epoch() const { return _rtc ? _rtc->getCurrentTime() : 0; }

void RadioDiag::save() {
  if (!_fs) return;
  _fs->remove(RADIO_FILE);
  File f = _fs->open(RADIO_FILE, FILE_O_WRITE);
  if (f) { f.write((const uint8_t*)&_s, sizeof(_s)); f.close(); }
}

void RadioDiag::loop(bool in_rx, uint32_t n_recv, uint32_t n_sent) {
  uint32_t now = millis();
  if (!_started) { _started = true; _last_rx_ms = now; _last_recv = n_recv; _sent_at_rx = _last_sent = n_sent; }
  _last_sent = n_sent;

  // out of Rx mode for > 8 s (a TX takes well under 2 s at the bench settings)
  if (!in_rx) {
    if (!_nonrx_start) _nonrx_start = now ? now : 1;
    else if (!_nonrx_flag && now - _nonrx_start > NONRX_MS) {
      _nonrx_flag = true;
      if (_s.nonrx_n < 65535) _s.nonrx_n++;
      uint32_t e = epoch(); _s.nonrx_epoch = e > NONRX_MS / 1000 ? e - NONRX_MS / 1000 : e;
      if (_s.nonrx_max_ms < now - _nonrx_start) _s.nonrx_max_ms = now - _nonrx_start;
      save();                                          // now, not later: the node may never get out of this state
    }
  } else if (_nonrx_start) {
    if (_nonrx_flag && _s.nonrx_max_ms < now - _nonrx_start) { _s.nonrx_max_ms = now - _nonrx_start; save(); }
    _nonrx_start = 0; _nonrx_flag = false;
  }

  // nothing received for > 10 min
  if (n_recv != _last_recv) {
    if (_silent_flag) { uint32_t s = (now - _last_rx_ms) / 1000; if (_s.silent_max_s < s) { _s.silent_max_s = s; save(); } }
    _last_recv = n_recv; _last_rx_ms = now; _sent_at_rx = n_sent; _silent_flag = false;
  } else if (!_silent_flag && now - _last_rx_ms > SILENT_MS) {
    _silent_flag = true;
    if (_s.silent_n < 65535) _s.silent_n++;
    _s.silent_epoch = epoch();
    uint32_t sent = n_sent - _sent_at_rx; _s.silent_sent = sent > 65535 ? 65535 : (uint16_t)sent;
    _s.silent_inrx = in_rx ? 1 : 0;
    if (_s.silent_max_s < SILENT_MS / 1000) _s.silent_max_s = SILENT_MS / 1000;
    save();
  }
}

// "fw radio": nonRX>8s events (longest, how long ago) | silent>10min events (longest, ago, in Rx mode?, sent) | now: last rx, uptime counters
bool RadioDiag::cmd(const char* arg, char* reply) {
  while (*arg == ' ') arg++;
  if (strcmp(arg, "clear") == 0) { memset(&_s, 0, sizeof(_s)); _s.magic = RADIO_MAGIC; save(); strcpy(reply, "OK - radio record cleared"); return true; }
  uint32_t e = epoch(), now = millis();
  char a1[16] = "-", a2[16] = "-";
  if (_s.nonrx_n && _s.nonrx_epoch && e >= _s.nonrx_epoch) snprintf(a1, sizeof(a1), "%lum ago", (unsigned long)((e - _s.nonrx_epoch) / 60));
  if (_s.silent_n && _s.silent_epoch && e >= _s.silent_epoch) snprintf(a2, sizeof(a2), "%lum ago", (unsigned long)((e - _s.silent_epoch) / 60));
  snprintf(reply, 160, "radio: nonRX>8s x%u (max %lus, last %s) | silent>10m x%u (max %lum, last %s, %s, sent %u) | now: rx %lus ago%s",
           (unsigned)_s.nonrx_n, (unsigned long)(_s.nonrx_max_ms / 1000), a1,
           (unsigned)_s.silent_n, (unsigned long)(_s.silent_max_s / 60), a2, _s.silent_inrx ? "inRX" : "notRX", (unsigned)_s.silent_sent,
           (unsigned long)((now - _last_rx_ms) / 1000), _nonrx_start ? ", NOT in Rx" : "");
  return true;
}

}  // namespace fw_ota
