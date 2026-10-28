#!/usr/bin/env python3
# summarize study runs: python3 summ.py <dir> [<dir> ...]
import sys, glob, re, statistics as st, collections
rows = collections.defaultdict(list)
for d in sys.argv[1:]:
    for f in glob.glob(d + "/out_*.txt"):
        cfg = f.split("out_")[1].rsplit("_", 2)[0]
        for line in open(f, errors="replace"):
            if line.startswith("RESULT"):
                kv = dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
                rows[(kv["net"], cfg)].append(kv)
def mean(v): return st.mean(v) if v else float("nan")
print(f"{'net':3} {'cfg':4} {'n':>2} {'all%':>5} {'nodes%':>6} {'time_h(med,done)':>16} {'air_s':>6} {'maxtx_s':>7} {'coll':>5} {'srcTx/ch':>8} {'deaths':>6} {'dead_h':>6} {'after2h':>7} {'esc':>4} {'claimF':>6} {'summ':>5} {'erase':>5}")
for (net, cfg) in sorted(rows):
    r = rows[(net, cfg)]
    dn = [int(x["done"].split("/")[0]) / int(x["done"].split("/")[1]) for x in r]
    full = [x for x in r if x["done"].split("/")[0] == x["done"].split("/")[1]]
    tm = [float(x["time_min"]) / 60 for x in full]
    f = lambda k: mean([float(x[k]) for x in r])
    print(f"{net:3} {cfg:4} {len(r):2} {100*len(full)/len(r):5.0f} {100*mean(dn):6.1f} {(st.median(tm) if tm else float('nan')):16.1f} {f('air_s'):6.0f} {f('maxtx_s'):7.0f} {f('coll'):5.0f} {f('src_tx_chunk'):8.2f} {f('deaths'):6.2f} {f('dead_h'):6.1f} {f('after2h_s'):7.1f} {f('esc'):4.0f} {f('claim_floods'):6.1f} {f('summs'):5.0f} {f('erases'):5.0f}")
