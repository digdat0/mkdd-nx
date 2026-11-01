#!/bin/bash
# Single player battles in the portable Ryujinx (see ryujinx_dk.sh), A pressed all the way through. Each run
# leaves <out>/<name>/ with its log and screens (one each 15 s).
#   tools/battle_test.sh <out> <seconds> "<kind:karts:starts> ..."   (kind 0 balloons, 1 Shine, 2 Bob-omb; starts 0: the arena's)
set -u
OUT="$1"; SECS="$2"; RUNS="$3"
RYU=/e/ryujinx_deko3dauto
SD="$RYU/portable/sdcard/switch/MKDD-NX"
INI="$SD/config/settings.ini"
NRO="$(dirname "$0")/../build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro"
mkdir -p "$OUT"
cp "$NRO" "$SD/MKDD-NX.nro" || exit 1
cp "$INI" "$OUT/settings.ini.kept"
stop() { powershell.exe -NoProfile -Command "Get-Process Ryujinx -ErrorAction SilentlyContinue | Stop-Process -Force" > /dev/null 2>&1; sleep 2; }
for R in $RUNS; do
    IFS=: read -r KIND KARTS STARTS <<< "$R"
    D="$OUT/k${KIND}_${KARTS}karts_s${STARTS}"; mkdir -p "$D"
    rm -f "$SD"/logs/bt_*
    sed -i -E '/^(bomb_hits|battle_karts|logging|single_battle|mod)=/d' "$INI"
    printf 'bomb_hits=%s\nbattle_karts=%s\nlogging=%s\nsingle_battle=1\nmod=off\n' "${BT_HITS:-4}" "$KARTS" "${BT_LOG:-4}" >> "$INI"
    echo "$KIND $STARTS" > "$SD/config/battle_test"
    "$RYU/Ryujinx.exe" "$(cygpath -w "$SD/MKDD-NX.nro")" > /dev/null 2>&1 &
    PID=$!
    for ((t = 0; t < SECS; t++)); do kill -0 $PID 2> /dev/null || break; sleep 1; done
    [ "$t" -lt "$SECS" ] && echo "$R: EMULATOR EXITED after ${t}s" || echo "$R: ran ${t}s"
    stop
    cp "$SD/logs/mkdd.log" "$D/" 2> /dev/null
    grep -E "1-player battle:|start .* added|computer team|chartest|unmapped|panic" "$SD/logs/mkdd.log" | cut -c1-120 | head -24
    py -3 -I -c "
import sys,glob,os
from PIL import Image
for p in glob.glob(sys.argv[1]+'/logs/bt_*.ppm'):
    Image.open(p).resize((640,360)).save(sys.argv[2]+'/'+os.path.basename(p)[:-4]+'.png')
" "$(cygpath -m "$SD")" "$(cygpath -m "$D")"
done
rm -f "$SD/config/battle_test" "$SD"/logs/bt_*
cp "$OUT/settings.ini.kept" "$INI"
