// rweather Crypto's global RNG (Ed25519/Curve25519 reference it; node keys come from MeshCore's RNG). Own file: RNG.h clashes with
// MeshCore.h's SEED_SIZE macro.
#include <RNG.h>                                  // rweather Crypto's global RNG (Ed25519/Curve25519 reference it; keys come from MeshCore's RNG)
RNGClass::RNGClass() {}
RNGClass::~RNGClass() {}
void RNGClass::rand(uint8_t* d, size_t n) { static uint32_t s = 0xC0FFEE; for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; d[i] = (uint8_t)(s >> 16); } }
RNGClass RNG;
