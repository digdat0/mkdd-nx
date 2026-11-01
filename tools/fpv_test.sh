#!/bin/bash
# KART FPV screenshots in the portable Ryujinx (see ryujinx_dk.sh): for each kart, a race with that
# kart and a saved screen for each eye. Writes <out>/fpv_<kart>_<eye>.png.
#   tools/fpv_test.sh <out> "<karts>" ["UP FWD TILT;UP FWD TILT;..."]   (no eyes: the table's, kart_eye.h)
set -u
OUT="$1"; KARTS="$2"; EYES="${3:-}"
RYU=/e/ryujinx_deko3dauto
SD="$RYU/portable/sdcard/switch/MKDD-NX"
NRO="$(dirname "$0")/../build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro"
mkdir -p "$OUT"
cp "$NRO" "$SD/MKDD-NX.nro" || exit 1
rm -f "$SD/config/dk_capture"
stop() { powershell.exe -NoProfile -Command "Get-Process Ryujinx -ErrorAction SilentlyContinue | Stop-Process -Force" > /dev/null 2>&1; sleep 2; }
for K in $KARTS; do
    rm -f "$SD"/logs/fpv_*
    { echo "kart $K"; IFS=';'; for E in $EYES; do echo "eye $E"; done; unset IFS; } > "$SD/config/fpv_test"
    "$RYU/Ryujinx.exe" "$(cygpath -w "$SD/MKDD-NX.nro")" > /dev/null 2>&1 &
    for ((t = 0; t < 240; t++)); do [ -e "$SD/logs/fpv_done" ] && break; sleep 1; done
    sleep 1
    stop
    [ -e "$SD/logs/fpv_done" ] && echo "kart $K: done in ${t}s" || echo "kart $K: TIMED OUT"
    grep -hE "fpv test|kart view" "$SD/logs/mkdd.log" | tail -n 2
    py -3 -I -c "
import sys,glob,os
from PIL import Image
for p in glob.glob(sys.argv[1]+'/logs/fpv_*.ppm'):
    Image.open(p).save(sys.argv[2]+'/'+os.path.basename(p)[:-4]+'.png')
" "$(cygpath -m "$SD")" "$(cygpath -m "$OUT")"
done
rm -f "$SD/config/fpv_test"
