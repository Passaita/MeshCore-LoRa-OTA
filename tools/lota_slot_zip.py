#!/usr/bin/env python3
"""Make a "slot zip" for `fw pull-ble` (LoRa-OTA v1.17.28+, bootloader rev 5+): a DFU package for the phone's DFU app (nRF Device Firmware
Update / nRF Connect) whose image is NOT an app but a .lota file placed so the bootloader's slot-fill mode puts it into the staging slot:

    image = 0xFF x (0x1000 - 160)  +  .lota (160-byte header + payload)  +  0xFF padding to a multiple of 4

The bootloader (GPREGRET 0xA9) writes the image from the slot start (0x96000): the .lota header ends at the slot's first page, the payload
starts exactly at the slot payload offset (0x1000). At the next boot the app checks the header (board, kind, sizes, CRC-32, SHA-256) and
turns it into a VALID slot, which it serves. Only after `fw pull-ble`: sent after a plain `start ota` this image would replace the app
(it is not an app) - the node then waits in BLE DFU until the normal app zip is uploaded.

usage: lota_slot_zip.py in.lota out-slot.zip
"""
import os, struct, subprocess, sys, tempfile, zlib

SLOT_SIZE = 0xEC000 - 0x96000        # the whole slot (the control page follows it)
PAD = 0x1000 - 160
NRFUTIL = os.path.join(os.path.dirname(sys.executable), "adafruit-nrfutil")

def main():
    if len(sys.argv) != 3: sys.exit(__doc__)
    lota = open(sys.argv[1], "rb").read()
    if lota[:4] != b"LOTA" or struct.unpack_from("<H", lota, 4)[0] != 1: sys.exit("not a .lota v1 file")
    if zlib.crc32(lota[:156]) & 0xFFFFFFFF != struct.unpack_from("<I", lota, 156)[0]: sys.exit(".lota header CRC wrong")
    if lota[6] != 0: sys.exit("not an app image (fw pull-ble takes apps only)")
    plen = struct.unpack_from("<I", lota, 20)[0]
    if 160 + plen != len(lota): sys.exit(".lota length does not match its header")
    img = b"\xff" * PAD + lota
    img += b"\xff" * (-len(img) % 4)
    if len(img) > SLOT_SIZE: sys.exit(f"image {len(img)} B does not fit the slot ({SLOT_SIZE} B)")
    with tempfile.TemporaryDirectory() as d:
        b = os.path.join(d, "slot.bin"); open(b, "wb").write(img)
        r = subprocess.run([NRFUTIL, "dfu", "genpkg", "--dev-type", "0x0052", "--dev-revision", "52840", "--sd-req", "0x00B6",
                            "--application", b, sys.argv[2]], capture_output=True, text=True)
        if r.returncode: sys.exit(r.stdout + r.stderr)
    board = lota[8:16].rstrip(b"\0").decode(); ver = struct.unpack_from("<I", lota, 16)[0]
    print(f"{sys.argv[2]}: slot zip for {board} fw-{ver}, image {len(img)} B (.lota {len(lota)} B at +0x{PAD:X})")

if __name__ == "__main__":
    main()
