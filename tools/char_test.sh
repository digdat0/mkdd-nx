#!/bin/bash
# Character select freezes in the portable Ryujinx (see fpv_test.sh): each folder of <list> (one path a line)
# hovered, picked and undone, up to 250 characters a run. A character that stops the game is written to
# <out>/freezes.txt with its logs, and the run goes on from the next one.
#   tools/char_test.sh <out> <list>
set -u
OUT="$1"; LIST="$2"
RYU=/e/ryujinx_deko3dauto
SD="$RYU/portable/sdcard/switch/MKDD-NX"
CH="$SD/mods/characters"; LOG="$SD/logs/mkdd.log"; INI="$SD/config/settings.ini"
mkdir -p "$OUT"
cp "$(dirname "$0")/../build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro" "$SD/MKDD-NX.nro" || exit 1
rm -f "$SD/config/dk_capture" "$SD/config/fpv_test"
cp "$INI" "$OUT/settings.ini.bak"
sed -i 's/^characters=.*/characters=1/; s/^mod=.*/mod=off/; s/^logging=.*/logging=1/' "$INI"
stop() { powershell.exe -NoProfile -Command "Get-Process Ryujinx -ErrorAction SilentlyContinue | Stop-Process -Force" > /dev/null 2>&1; sleep 2; }
running() { powershell.exe -NoProfile -Command "if (Get-Process Ryujinx -ErrorAction SilentlyContinue) { 'y' }" | grep -q y; }
run_batch() { # the characters are in place: until every one has had its turn
    local N=0 last t line idle F S
    while :; do
        echo "$N" > "$SD/config/char_test"
        rm -f "$LOG"
        "$RYU/Ryujinx.exe" "$(cygpath -w "$SD/MKDD-NX.nro")" > /dev/null 2>&1 &
        last=""; idle=0; t=0
        while :; do
            sleep 5; t=$((t + 5))
            line=$(grep -a "chartest:" "$LOG" 2>/dev/null | tail -n 1)
            case "$line" in *"chartest: done"*) stop; return 0;; esac
            if [ "$line" != "$last" ]; then last="$line"; idle=0; else idle=$((idle + 5)); fi
            if [ -z "$line" ] && [ "$t" -lt 420 ] && running; then continue; fi # not there yet
            if [ "$idle" -ge 60 ] || ! running; then break; fi # stopped on a character
        done
        if [ -z "$line" ]; then echo "never reached the select screen at $N" | tee -a "$OUT/freezes.txt"; stop; return 1; fi
        F=$(echo "$line" | sed -E 's/.*chartest: ([0-9]+) .*/\1/')
        echo "$line" | tee -a "$OUT/freezes.txt"
        S="$OUT/freeze_$(date +%H%M%S)_$F"; mkdir -p "$S"
        cp "$LOG" "$SD/logs/mkdd_trace.log" "$SD/logs/stderr.log" "$S/" 2>/dev/null
        stop
        grep -a "chartest:\|select screen\|no room\|OSPanic\|OSReport" "$S/mkdd.log" >> "$OUT/all.log"
        N=$((F + 1))
    done
}
mapfile -t ALL < "$LIST"
mkdir -p "$OUT/kept"; mv "$CH"/* "$OUT/kept/" 2>/dev/null
for ((i = 0; i < ${#ALL[@]}; i += 250)); do
    rm -rf "${CH:?}"/*
    for d in "${ALL[@]:i:250}"; do cp -r "${d%$'\r'}" "$CH/"; done
    echo "=== batch from $i"
    run_batch || break
    grep -a "chartest:\|select screen\|no room\|OSPanic\|OSReport" "$LOG" >> "$OUT/all.log"
done
rm -rf "${CH:?}"/*; mv "$OUT/kept/"* "$CH/" 2>/dev/null
rm -f "$SD/config/char_test"; cp "$OUT/settings.ini.bak" "$INI"
echo finished
