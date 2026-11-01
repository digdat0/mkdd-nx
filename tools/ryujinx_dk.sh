#!/bin/bash
# Run the (deko3d) build in the portable Ryujinx (E:/ryujinx_deko3dauto, its own sdcard and config) for
# N seconds (default 90), then close it and print the end of the game's log and Ryujinx's errors.
#   tools/ryujinx_dk.sh [seconds] [nro]
set -u
RYU=/e/ryujinx_deko3dauto
SD="$RYU/portable/sdcard/switch/MKDD-NX"
NRO="${2:-$(dirname "$0")/../build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro}"
SECS="${1:-90}"
cp "$NRO" "$SD/MKDD-NX.nro" || exit 1
rm -f "$RYU"/portable/Logs/*.log
"$RYU/Ryujinx.exe" "$(cygpath -w "$SD/MKDD-NX.nro")" > /dev/null 2>&1 &
sleep "$SECS"
powershell.exe -NoProfile -Command "Get-Process Ryujinx -ErrorAction SilentlyContinue | Stop-Process -Force" > /dev/null 2>&1
sleep 2
echo "== game log"
tail -n 60 "$SD/logs/mkdd.log" 2>/dev/null
echo "== Ryujinx errors"
grep -hE "\|E\||\|W\| (Gpu|Service)|Guest|Unhandled|crash" "$RYU"/portable/Logs/*.log 2>/dev/null | grep -v "Stubbed" | tail -n 40
