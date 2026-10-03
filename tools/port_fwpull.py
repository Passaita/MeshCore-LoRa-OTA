#!/usr/bin/env python3
"""Bring the LoRa firmware-pull feature (fw_ota) onto another MeshCore checkout (official, EasySkyMesh/IoTThinks PowerSaving-vNN, ...).

  python3 tools/port_fwpull.py <target-checkout> [--build ENV]

What it does (everything is checked, nothing is guessed):
  1. copies the self-contained parts: src/helpers/fw_ota/*, the two linker scripts, the stager, docs, the ChunkMap host test,
     and the reference PlatformIO environments (tools/fwpull_env.ini -> <target>/platformio.local.ini, only if the target has none);
  2. applies tools/fwpull_hooks.patch (the ~300 lines in 6 shared files: the simple_repeater hooks, the local-console clock fix,
     the start-ota fix for slim builds, the RV-3028 RTC mode) with `git apply --3way`;
  3. optionally builds ENV and checks the result: firmware tag at image offset 0x200, size within the 448 KiB app region.
Verified 2026-09-19: a clean merge onto IoTThinks/MeshCore PowerSaving-v17 (27 commits newer than our base) builds at 363,064 B.
"""
import argparse, os, re, shutil, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
COPY = ['src/helpers/fw_ota', 'boards/nrf52840_s140_v6_fwota.ld', 'boards/nrf52_common_fwota.ld',
        'tools/fw_ota_stage.py', 'docs/lora-ota', 'test/fw_ota_chunkmap_test.cpp']
APP_MAX = 0x70000
TAG = bytes.fromhex('4657504c01000000') + b'RAK3401'

def run(cmd, cwd, check=True):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'FAILED: {" ".join(cmd)}\n{r.stdout}{r.stderr}')
    return r

def upstream_id(t):
    """'<base FIRMWARE_VERSION>@<short commit>' of the target checkout, so `ver` shows what the build is based on."""
    m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', open(os.path.join(t, 'examples/simple_repeater/MyMesh.h')).read())
    return f'{m.group(1) if m else "unknown"}@{run(["git", "rev-parse", "--short=8", "HEAD"], t).stdout.strip()}'

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('target'); ap.add_argument('--build', metavar='ENV', help='PlatformIO environment to build and check afterwards')
    a = ap.parse_args()
    t = os.path.abspath(a.target)
    for need in ('src/helpers/NRF52Board.cpp', 'examples/simple_repeater/MyMesh.cpp', 'platformio.ini'):
        if not os.path.exists(os.path.join(t, need)): sys.exit(f'{t} does not look like a MeshCore checkout (missing {need})')
    if run(['git', 'status', '--porcelain', '--untracked-files=no'], t).stdout.strip():
        sys.exit('the target has uncommitted changes to tracked files - commit or stash them first')

    for rel in COPY:
        src, dst = os.path.join(SRC, rel), os.path.join(t, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isdir(src): shutil.copytree(src, dst, dirs_exist_ok=True)
        else: shutil.copy2(src, dst)
        print('copied', rel)
    env = os.path.join(t, 'platformio.local.ini')
    if os.path.exists(env): print('platformio.local.ini exists - merge tools/fwpull_env.ini into it by hand (envs RAK_3401_repeater_poc / _stocktag)')
    else:
        up = upstream_id(t)
        open(env, 'w').write(open(os.path.join(HERE, 'fwpull_env.ini')).read().replace('@UPSTREAM@', up))
        print(f'wrote platformio.local.ini (reference environments), base = {up}')

    r = run(['git', 'apply', '--3way', os.path.join(HERE, 'fwpull_hooks.patch')], t, check=False)
    if r.returncode: sys.exit('the hooks patch does not apply cleanly - resolve by hand (see docs/fw_pull_porting.md):\n' + r.stdout + r.stderr)
    print('applied fwpull_hooks.patch')

    if a.build:
        run(['pio', 'run', '-e', a.build], t)
        z = os.path.join(t, '.pio/build', a.build, 'firmware.zip')
        img = zipfile.ZipFile(z).read('firmware.bin')
        ok = img[0x200:0x200 + len(TAG)] == TAG and len(img) <= APP_MAX
        print(f'built {a.build}: {len(img)} B ({100 * len(img) / APP_MAX:.1f}% of the 448 KiB app region), firmware tag '
              f'{"present" if img[0x200:0x200 + len(TAG)] == TAG else "MISSING"}')
        sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
