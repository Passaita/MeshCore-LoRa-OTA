#!/usr/bin/env python3
"""
fw_ota_stage.py - host-side stager for the RAK3401 LoRa firmware-pull (LoRa-OTA) support.

Takes a compiled firmware .bin, LZMA-compresses it to match what the
on-device bootloader's decoder expects, and writes it into the device's
single fw_ota slot over the serial console using the `fw stage` command
(FwOtaTransfer::cmdStage / stageFeed, rev13+ single-slot firmware).

Requires pyserial:  pip install pyserial

Usage:
  python3 fw_ota_stage.py <firmware.bin> <serial-port> [--baud 115200]
                           [--fw-version N] [--dict-size 8192]

Example:
  python3 fw_ota_stage.py rak3401-repeater-min2.bin /dev/ttyACM0

The dict size MUST match FW_OTA_LZMA_DIC_SIZE in the bootloader's
fw_ota_apply.c (currently 8192) - a larger dict at compress time produces
back-references the decoder's ring buffer can't resolve, corrupting the
decompressed image even though the transfer/CRC checks all pass. Do not
change --dict-size without changing the bootloader to match.
"""
import argparse
import lzma
import sys
import time
import zlib


def compress_for_slot(raw: bytes, dict_size: int):
    """Returns (props5_bytes, raw_crc32, payload_bytes).

    props5 + payload together are what the bootloader's LzmaProps_Decode +
    LzmaDec_DecodeToDic expect: a standalone LZMA1 raw stream with the
    5-byte props (1 byte lc/lp/pb + 4-byte little-endian dict size) kept
    separate rather than embedded in the stream (that's what
    SlotHeader.lzma_props[5] is for). Using FORMAT_ALONE and slicing off
    its 13-byte header is the simplest way to get exactly that from
    Python's stdlib lzma module - verified byte-for-byte against what
    LzmaProps_Decode expects (see project memory, 2026-09-16 test).
    """
    filt = [{
        "id": lzma.FILTER_LZMA1,
        "dict_size": dict_size,
        "lc": 3, "lp": 0, "pb": 2,
        "mode": lzma.MODE_NORMAL,
        "nice_len": 273,
        "mf": lzma.MF_BT4,
        "depth": 0,
    }]
    alone = lzma.compress(raw, format=lzma.FORMAT_ALONE, filters=filt)
    props5 = alone[:5]              # 1 byte props + 4 byte LE dict size
    payload = alone[13:]            # strip the 8-byte (unused) size field too
    raw_crc32 = zlib.crc32(raw) & 0xFFFFFFFF
    return props5, raw_crc32, payload


TAG_MAGIC = 0x4C505746      # bytes 'F','W','P','L' - must match the bootloader's FW_OTA_TAG_MAGIC
TAG_VERSION = 1
TAG_SCAN = 1024


def has_fw_tag(raw: bytes, board: str) -> bool:
    """True if a valid fw_ota firmware tag (magic, version, board id) sits, 4-byte aligned,
    within the first TAG_SCAN bytes - the same rule the bootloader enforces."""
    import struct
    bid = board.encode().ljust(8, b"\0")[:8]
    for off in range(0, min(len(raw), TAG_SCAN) - 15, 4):
        magic, ver, _res, b = struct.unpack_from("<IHH8s", raw, off)
        if magic == TAG_MAGIC and ver == TAG_VERSION and b == bid:
            return True
    return False


def read_reply(ser, idle_timeout=10.0):
    """Reads lines until one prefixed '  -> ' (the device's reply marker,
    see main.cpp's `Serial.print("  -> "); Serial.println(reply);`),
    returns it with the prefix stripped. Any other non-blank line (e.g. the
    `fw_ota: staged N/M B` progress lines printed during a long `fw stage`)
    is echoed to stdout as a passthrough rather than silently discarded, so
    you can see it's alive. `idle_timeout` resets every time a line arrives
    - a slow-but-progressing transfer won't time out, only a genuinely
    stuck one will."""
    deadline = time.time() + idle_timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode(errors="replace").rstrip("\r")
            deadline = time.time() + idle_timeout   # any line = still alive
            if line.startswith("  -> "):
                return line[5:]
            elif line.strip():
                print(f"   (device) {line}")
    raise TimeoutError("no activity from device within %.1fs" % idle_timeout)


def send_cmd(ser, cmd: str, idle_timeout=10.0) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    ser.flush()
    return read_reply(ser, idle_timeout=idle_timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_path", help="compiled firmware .bin to stage")
    ap.add_argument("port", help="serial port, e.g. /dev/ttyACM0 or COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--fw-version", type=int, default=None,
                     help="defaults to the current unix timestamp (monotonic enough for bench testing)")
    ap.add_argument("--dict-size", type=int, default=8192,
                     help="MUST match the bootloader's FW_OTA_LZMA_DIC_SIZE (default 8192)")
    ap.add_argument("--board", default="RAK3401", help="board id the firmware tag must carry (default RAK3401)")
    ap.add_argument("--allow-untagged", action="store_true",
                     help="stage an image WITHOUT the fw_ota firmware tag (the bootloader will still refuse to apply it)")
    args = ap.parse_args()

    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is required: pip install pyserial")

    with open(args.bin_path, "rb") as f:
        raw = f.read()

    if not has_fw_tag(raw, args.board):
        msg = (f"{args.bin_path} carries no fw_ota firmware tag for board {args.board!r} in its first "
               f"{TAG_SCAN} bytes - this looks like stock/other firmware. The bootloader would refuse it.")
        if not args.allow_untagged:
            sys.exit("REFUSED: " + msg + "\nRebuild the app with the tag (RAK_3401_repeater_poc env) or pass --allow-untagged.")
        print("WARNING: " + msg)

    fw_version = args.fw_version if args.fw_version is not None else int(time.time())

    print(f"raw image: {len(raw)} B")
    props5, raw_crc32, payload = compress_for_slot(raw, args.dict_size)
    print(f"compressed: {len(payload)} B ({100 * len(payload) / len(raw):.1f}% of raw), "
          f"props={props5.hex()}, raw_crc32={raw_crc32:08x}, fw_version={fw_version}")

    ser = serial.Serial(args.port, args.baud, timeout=1)
    time.sleep(0.3)   # let the port settle

    cmd = f"fw stage {len(payload)} {len(raw)} {raw_crc32:08x} {fw_version} {props5.hex()}"
    print(f"-> {cmd}")
    reply = send_cmd(ser, cmd, idle_timeout=10.0)
    print(f"<- {reply}")
    if not reply.startswith("OK"):
        sys.exit("device rejected the stage command, aborting")

    print(f"sending {len(payload)} B ...")
    t0 = time.time()
    # Paced, modest chunks with a short pause between each - the device is a
    # live mesh repeater servicing radio/mesh housekeeping in the same
    # loop() that drains this serial data, on top of writing+verifying
    # flash pages, so it won't always keep up with an unthrottled burst.
    # This trades some speed for headroom rather than assuming USB CDC flow
    # control alone is enough to prevent it falling behind.
    CHUNK = 256
    for off in range(0, len(payload), CHUNK):
        ser.write(payload[off:off + CHUNK])
        ser.flush()
        time.sleep(0.005)
    elapsed = time.time() - t0
    print(f"sent in {elapsed:.1f}s ({len(payload) / elapsed / 1024:.1f} KB/s), waiting for result "
          f"(device prints progress every 16KB received - if that stalls, it's genuinely stuck)...")

    # Long idle timeout is safe here since it resets on every line the device
    # prints, including the periodic progress lines - a slow-but-alive
    # transfer won't time out, only a truly stuck one will.
    result = read_reply(ser, idle_timeout=60.0)
    print(f"<- {result}")
    ser.close()

    if not result.startswith("OK"):
        sys.exit("staging failed")
    print("done - run 'fw slot' / 'fw verify' on the device to double-check, then 'fw serve' to offer it.")


if __name__ == "__main__":
    main()
