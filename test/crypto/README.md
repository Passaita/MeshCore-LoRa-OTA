# test/crypto - pinned copy of rweather/Crypto for host tests

The subset of rweather/Crypto (MIT licence, see the header of every file) that MeshCore's `src/Utils.cpp` uses for channel messages
(AES-128 + SHA-256 HMAC), copied from the PlatformIO dependency the RAK3401 release build uses (`rweather/Crypto @ ^0.4.0`).
The RAK3401 build does NOT define `USE_CC310_HW_CRYPTO`, so the firmware runs exactly this code.
Used by `test/fw_ota_crypto_test.sh` (A11): LoRa-OTA channel payloads go through MeshCore's real encrypt / decrypt there.
Do not use `test/mocks/AES.h` / `SHA256.h` for that test - they are non-cryptographic stand-ins.
