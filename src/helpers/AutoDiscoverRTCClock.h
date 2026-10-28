#pragma once

#include <Mesh.h>
#include <Arduino.h>
#include <Wire.h>

class AutoDiscoverRTCClock : public mesh::RTCClock {
  mesh::RTCClock* _fallback;

  bool i2c_probe(TwoWire& wire, uint8_t addr);
public:
  AutoDiscoverRTCClock(mesh::RTCClock& fallback) : _fallback(&fallback) { }

  void begin(TwoWire& wire);
  uint32_t getCurrentTime() override;
  void setCurrentTime(uint32_t time) override;
  // (rc21, C57) LoRa-OTA time from a swarm member
  bool chipValid() const;               // an RTC chip holds a plausible time (it answers the swarm's time requests)
  void discipline(uint32_t ref);        // a better time from a trusted peer: behind -> step forward, ahead -> hold (never back)

  void tick() override {
    _fallback->tick();   // is typically VolatileRTCClock, which now needs tick()
  }
};
