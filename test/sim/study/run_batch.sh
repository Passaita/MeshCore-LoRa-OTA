#!/bin/bash
# run_batch.sh OUTDIR SEEDS : the gap-fill study comparison (docs/GAPFILL-ALGO-STUDY-20261008.md) - 6 variants x T1..T4 x SEEDS seeds.
# Builds the fast simulator (no sanitizers) once, runs the batch in parallel, prints the summary table (summ.py).
# The rc18 defaults ARE variant G; "A0" = rc17 (algo A, no adopt/newq/spread).
set -e -o pipefail
O=${1:?outdir}; N=${2:-16}; D=$(cd "$(dirname "$0")/../../.." && pwd); mkdir -p "$O"
SIM_FAST=1 SIM_KEEP="$O/sim" bash "$D/test/fw_ota_sim_test.sh" STUDY net=T1 chunks=40 hours=1 > /dev/null
R0="adopt=0 newq=0 spread=0 srcspread=0 life=6 k=1"; X="adopt=1 newq=15 spread=0 srcspread=0 life=6 k=1"; E="algo=E adopt=1 newq=15 spread=1 srcspread=0 life=48"
cd "$O"; for net in T1 T2 T3 T4; do for s in $(seq 1 "$N"); do
  echo "A0_${net}_$s net=$net seed=$s algo=A $R0"; echo "C_${net}_$s net=$net seed=$s algo=C $X"; echo "D_${net}_$s net=$net seed=$s algo=D $X"
  echo "E2_${net}_$s net=$net seed=$s $E k=1"; echo "E2k2_${net}_$s net=$net seed=$s $E k=2"; echo "G_${net}_$s net=$net seed=$s algo=E"
done; done | xargs -P "$(nproc)" -L 1 sh -c './sim STUDY "$@" > out_$0.txt 2>&1'
python3 "$D/test/sim/study/summ.py" .
