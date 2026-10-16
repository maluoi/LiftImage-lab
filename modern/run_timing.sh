#!/bin/sh
# Timing pass: run alone on an idle machine. Pins to CPU 3, times every lossy and lossless setting single-threaded,
# then prints the tables against the saved metrics pass.
# usage: run_timing.sh [metrics.csv]
set -e
M=$(cd "$(dirname "$0")" && pwd)
METRICS=${1:-$M/runs/metrics/metrics.csv}
OUT=$M/runs/timing_$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
IMAGES=$(ls "$M"/../corpus/examples/*.png "$M"/../corpus/kodak/*.png | sort)
taskset -c 3 "$M/build/compare_modern" $IMAGES > "$OUT/timing.csv"
taskset -c 3 "$M/build/compare_modern" -l $IMAGES > "$OUT/lossless_timing.csv"
python3 "$M/compare_modern.py" --timing "$OUT/timing.csv" --metrics "$METRICS" --metric ssimulacra2 | tee "$OUT/lossy_table.md"
python3 "$M/compare_modern.py" --timing "$OUT/timing.csv" --metrics "$METRICS" --metric psnr | tee "$OUT/lossy_table_psnr.md"
python3 "$M/compare_modern.py" --lossless "$OUT/lossless_timing.csv" | tee "$OUT/lossless_table.md"
echo "saved to $OUT"
