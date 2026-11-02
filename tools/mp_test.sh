#!/bin/bash
# A scripted 4-pad run in the portable Ryujinx (see ryujinx_dk.sh): pads and screens as config/mp_test in main_switch.c.
#   tools/mp_test.sh <out> <seconds> <mod> "<POLL BUTTONS;POLL BUTTONS;...>"   (buttons hex, as PAD_*)
set -u
OUT="$1"; SECS="$2"; MOD="$3"; EVENTS="$4"
RYU=/e/ryujinx_deko3dauto
SD="$RYU/portable/sdcard/switch/MKDD-NX"
INI="$SD/config/settings.ini"
NRO="$(dirname "$0")/../build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro"
mkdir -p "$OUT"
cp "$NRO" "$SD/MKDD-NX.nro" || exit 1
cp "$INI" "$OUT/settings.ini.kept"
rm -f "$SD"/logs/mp_*
sed -i -E '/^(mod)=/d' "$INI"
echo "mod=$MOD" >> "$INI"
for KV in ${MP_SET:-}; do sed -i -E "/^${KV%%=*}=/d" "$INI"; echo "$KV" >> "$INI"; done # MP_SET="key=value ...": more settings for this run
echo "$EVENTS" | tr ';' '\n' > "$SD/config/mp_test"
"$RYU/Ryujinx.exe" "$(cygpath -w "$SD/MKDD-NX.nro")" > /dev/null 2>&1 &
PID=$!
for ((t = 0; t < SECS; t++)); do kill -0 $PID 2> /dev/null || break; sleep 1; done
[ "$t" -lt "$SECS" ] && echo "EMULATOR EXITED after ${t}s" || echo "ran ${t}s"
powershell.exe -NoProfile -Command "Get-Process Ryujinx -ErrorAction SilentlyContinue | Stop-Process -Force" > /dev/null 2>&1
sleep 2
cp "$SD/logs/mkdd.log" "$OUT/" 2> /dev/null
py -3 -I -c "
import sys,glob,os
from PIL import Image
for p in glob.glob(sys.argv[1]+'/logs/mp_*.ppm'):
    Image.open(p).resize((640,360)).save(sys.argv[2]+'/'+os.path.basename(p)[:-4]+'.png')
" "$(cygpath -m "$SD")" "$(cygpath -m "$OUT")"
rm -f "$SD/config/mp_test" "$SD"/logs/mp_*
cp "$OUT/settings.ini.kept" "$INI"
