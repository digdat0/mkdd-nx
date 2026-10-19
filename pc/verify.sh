#!/bin/bash
# Native verifier run: a race with every native checked against the recompiled original
# (build from gen_c.py --native-verify C). Reports go to run/verify_<label>.txt.
#   pc/verify.sh <exe> <label> [seconds]
EXE=$1; LABEL=${2:-verify}; SECS=${3:-500}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -n "$MKDD_DISC" ] || { echo "set MKDD_DISC to your disc image (.iso/.gcm/.ciso/.rvz)"; exit 1; }
export PATH="/c/msys64/ucrt64/bin:$PATH"
mkdir -p "$HERE/run" && cd "$HERE/run"
S="600:0,10:1000,60:0,10:1000,60:0"; for i in $(seq 1 40); do S="$S,10:100,80:0"; done
MKDD_NATIVE_VERIFY=1 MKDD_NATIVE_VERIFY_MS=${MKDD_NATIVE_VERIFY_MS:-500} MKDD_TIMESCALE=4 timeout "$SECS" "$HERE/$EXE" "$MKDD_DISC" 110 "$S" > verify_$LABEL.txt 2>&1
grep -E "native .*checked|native .*(wrong|differ|mismatch)|crash|fatal" verify_$LABEL.txt | tail -n 120
