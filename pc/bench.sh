#!/bin/bash
# Throughput benchmark: race with guest time running MKDD_TIMESCALE (default 4) times faster, so
# the game runs as fast as the host CPU allows. Prints game frames per second in the race.
#   pc/bench.sh [exe] [label]
EXE=${1:-build/mkdd_pc.exe}; LABEL=${2:-run}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -n "$MKDD_DISC" ] || { echo "set MKDD_DISC to your disc image (.iso/.gcm/.ciso/.rvz)"; exit 1; }
export PATH="/c/msys64/ucrt64/bin:$PATH"
mkdir -p "$HERE/run" && cd "$HERE/run"
S="600:0,10:1000,60:0,10:1000,60:0"; for i in $(seq 1 40); do S="$S,10:100,80:0"; done
MKDD_TIMESCALE=${MKDD_TIMESCALE:-4} timeout 200 "$HERE/$EXE" "$MKDD_DISC" 110 "$S" > bench_$LABEL.txt 2>&1
# race = frames with > 30000 verts; game fps from consecutive samples there
python - "$LABEL" <<'PY'
import re,sys
rows=[(float(t),int(f),int(v)) for t,f,v in re.findall(r"\[t=\s*([\d.]+)s\] frames (\d+)\s+batches \d+\s+verts (\d+)",open(f"bench_{sys.argv[1]}.txt").read())]
r=[(a,b) for a,b in zip(rows,rows[1:]) if a[2]>30000 and b[2]>30000]
fps=[(b[1]-a[1])/(b[0]-a[0]) for a,b in r]
print(f"{sys.argv[1]}: race samples {len(fps)}, game fps " + (f"{sum(fps)/len(fps):.1f} (max {max(fps):.1f})" if fps else "n/a"))
PY
grep -E "unresolved" mkdd_trace.log | tail -1 | cut -c1-90
