#!/usr/bin/env python3
"""Pack the LoRa-OTA migration bundle for the Heltec V4 migrator (examples/lora_ota_migrator).

The bundle is what the user uploads on the migrator's page (step 2): a 32-byte header, the LoRa-OTA app image, the Wi-Fi seeder image.
  header: b"LOTAMIG1" | main_len u32 | seeder_len u32 | fw u32 | reserved u32 x2 | crc32 of the first 28 bytes (LE)
Checks before packing: both are ESP32-S3 app images with an appended SHA-256 (verified), the app carries the HELTECV4 LoRa-OTA tag,
the seeder does not, and the sizes fit their partitions (app 3 MiB, seeder 2 MiB).

usage: make_migrate_bundle.py <app firmware.bin> <seeder firmware.bin> <fw number> <out -migrate.bin>
"""
import hashlib
import struct
import sys
import zlib

TAG = struct.pack("<IHH", 0x4C505746, 1, 0) + b"HELTECV4"


def check_image(data, name):
    if len(data) < 24 or data[0] != 0xE9:
        sys.exit(f"{name}: not an ESP32 app image")
    segs = data[1]
    if struct.unpack_from("<H", data, 12)[0] != 9:
        sys.exit(f"{name}: not an ESP32-S3 image")
    if data[23] != 1:
        sys.exit(f"{name}: no appended SHA-256")
    pos, ck = 24, 0xEF
    for _ in range(segs):
        _, ln = struct.unpack_from("<II", data, pos)
        pos += 8
        for b in data[pos:pos + ln]:
            ck ^= b
        pos += ln
    ck_off = ((pos + 16) & ~15) - 1
    if data[ck_off] != ck:
        sys.exit(f"{name}: checksum")
    if hashlib.sha256(data[:ck_off + 1]).digest() != data[ck_off + 1:ck_off + 33]:
        sys.exit(f"{name}: SHA-256")
    if ck_off + 33 != len(data):
        sys.exit(f"{name}: {len(data) - ck_off - 33} bytes after the image")


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    app, seeder = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
    fw = int(sys.argv[3])
    check_image(app, "app")
    check_image(seeder, "seeder")
    if TAG not in app:
        sys.exit("app: no HELTECV4 LoRa-OTA tag - not a LoRa-OTA Heltec V4 build")
    if TAG in seeder:
        sys.exit("seeder: carries the app tag - wrong file?")
    if len(app) > 0x300000 or len(seeder) > 0x200000:
        sys.exit("an image is too big for its partition")
    hdr = b"LOTAMIG1" + struct.pack("<IIIII", len(app), len(seeder), fw, 0, 0)
    hdr += struct.pack("<I", zlib.crc32(hdr) & 0xFFFFFFFF)
    with open(sys.argv[4], "wb") as f:
        f.write(hdr + app + seeder)
    print(f"{sys.argv[4]}: fw-{fw}, app {len(app)} B + seeder {len(seeder)} B = {32 + len(app) + len(seeder)} B")


if __name__ == "__main__":
    main()
