#!/bin/bash
# Regenerate the C, rebuild the PC harness and benchmark it; appends a line to run/results.txt.
#   pc/pipeline.sh <label>
LABEL=${1:-run}
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.." && python tools/gen_c.py /e/MKDD/sheets /e/MKDD/gen2 > /dev/null 2>&1 || { echo "$LABEL: gen failed" >> "$HERE/run/results.txt"; exit 1; }
cd "$HERE" && ./build_pc.sh /e/MKDD/gen2 ${MKDD_JOBS:-12} > run/build_$LABEL.log 2>&1
grep -q "built" run/build_$LABEL.log || { echo "$LABEL: build failed" >> run/results.txt; exit 1; }
MKDD_PROF=60 ./bench.sh build/mkdd_pc.exe "$LABEL" | tee -a run/results.txt
grep "cpu: \(game\|mkdd-gpu\)" run/bench_$LABEL.txt | tee -a run/results.txt
