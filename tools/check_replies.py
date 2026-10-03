#!/usr/bin/env python3
"""Fail if any fixed reply text in the fw_ota code is longer than the 160-byte reply buffer.
usage: tools/check_replies.py [src/helpers/fw_ota]   (run before every release build)
Serial replies go into char reply[160] (main.cpp), remote ones into 161 bytes of temp[166] (MyMesh.cpp): strcpy past that corrupts the stack.
Formatted replies must use snprintf(reply, 160, ...), which is checked by grep here too."""
import glob, os, re, sys
d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "src", "helpers", "fw_ota")
bad = 0
for f in sorted(glob.glob(os.path.join(d, "*.h")) + glob.glob(os.path.join(d, "*.cpp"))):
    src = open(f).read()
    for m in re.finditer(r'strcpy\(\s*reply\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)', src):
        s = bytes("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))), "utf-8").decode("unicode_escape")
        if len(s) + 1 > 160:
            bad += 1; print(f"{os.path.basename(f)}:{src[:m.start()].count(chr(10)) + 1}: {len(s) + 1} bytes > 160: {s[:50]}...")
    for m in re.finditer(r'\b(sprintf|strcat)\s*\(\s*reply', src):
        bad += 1; print(f"{os.path.basename(f)}:{src[:m.start()].count(chr(10)) + 1}: unbounded {m.group(1)}(reply, ...) - use snprintf(reply, 160, ...)")
print("replies: OK" if not bad else f"replies: {bad} problem(s)"); sys.exit(1 if bad else 0)
