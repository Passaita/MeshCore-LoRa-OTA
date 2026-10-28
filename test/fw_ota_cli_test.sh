#!/bin/bash
# Host test of the fw CLI's argument clean-up (rc7, C15): the REAL lines of src/helpers/fw_ota/FwOtaCli.h that turn the command into the
# argument the dispatcher matches are extracted on every run and checked - a phone keyboard's autofill adds a trailing space
# ("fw pull-status "), which used to fall through to the 'fw pull <name>' parser ("ERR - unknown peer"). Also checks that every
# sub-command match in cliHandle() comes AFTER the clean-up, so no match can see the raw text.
set -e
cd "$(dirname "$0")/.."
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
python3 - "$W" <<'EOF'
import re, sys
W = sys.argv[1]
src = open("src/helpers/fw_ota/FwOtaCli.h").read()
body = src[src.index("static inline bool cliHandle("):]
a = body.index("  const char* arg = command + 2;")
b = body.index("arg = abuf;") + len("arg = abuf;")
block = body[a:b]
rest = body[b:]
first_use = min(m.start() for m in re.finditer(r'str(n)?(case)?cmp\(arg', body))
if first_use < b: sys.exit("FAIL: a sub-command is matched before the argument clean-up")
if re.search(r'\bcommand\b', rest.split("// Every reply must fit")[0]): sys.exit("FAIL: the dispatcher still reads the raw command after the clean-up")
prog = r'''#include <cstdio>
#include <cstring>
#include <cassert>
static const char* clean(const char* command, char* out) {
''' + block + r'''
  strcpy(out, arg); return out;
}
int main() {
  struct { const char* in; const char* want; } T[] = {
    {"fw pull-status ", "pull-status"}, {"fw pull-status", "pull-status"}, {"fw   pull-status  ", "pull-status"},
    {"fw serve on ", "serve on"}, {"fw serve\t", "serve"}, {"fw pull #1 duty 0.5 ", "pull #1 duty 0.5"},
    {"fw passphrase  ", "passphrase"}, {"fw pull-status\r\n", "pull-status"}, {"fw ", ""}, {"fw", ""}, {"fw erase ", "erase"},
  };
  int n = 0; char out[256];
  for (auto& t : T) { const char* r = clean(t.in, out); if (strcmp(r, t.want)) { printf("FAIL: '%s' -> '%s' (want '%s')\n", t.in, r, t.want); return 1; } n++; }
  char longc[400]; strcpy(longc, "fw "); memset(longc + 3, 'a', 350); longc[353] = ' '; longc[354] = 0;
  const char* r = clean(longc, out); assert(strlen(r) < 200); n++;   // never more than the buffer
  printf("fw CLI argument clean-up (C15) checks: %d passed\n", n); return 0;
}
'''
open(W + "/cli_test.cpp", "w").write(prog)
EOF
g++ -std=c++11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$W/cli_test" "$W/cli_test.cpp"
"$W/cli_test"
