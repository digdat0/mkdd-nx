#!/bin/bash
# Headless PC test build. Run from Git Bash; it re-enters C:\msys64's UCRT64 shell (gcc there).
#   pc/build_pc.sh [GEN dir] [jobs]
# Only recompiles sources newer than their object file.
set -e
GEN=${1:-/e/MKDD/gen}
JOBS=${2:-10}
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "$MKDD_IN_UCRT" ]; then
    exec /c/msys64/usr/bin/env MSYSTEM=UCRT64 CHERE_INVOKING=1 MKDD_IN_UCRT=1 MKDD_PC_OUT="${MKDD_PC_OUT:-build}" MKDD_GENF_EXTRA="$MKDD_GENF_EXTRA" TMP='E:/MKDD/tmp' TEMP='E:/MKDD/tmp' \
        /c/msys64/usr/bin/bash -lc "'$HERE/build_pc.sh' '$GEN' '$JOBS'"
fi
cd "$HERE"
OUT=${MKDD_PC_OUT:-build}
mkdir -p $OUT
RTF="-std=gnu11 -O2 -g -ffp-contract=off -fwrapv -fno-strict-aliasing -I../runtime -I$GEN -Wall -Wno-unused-function"
GENF="-std=gnu11 -O2 -ffp-contract=off -fwrapv -fno-strict-aliasing -I../runtime -I$GEN -w $MKDD_GENF_EXTRA"
jobs=()
for f in ../runtime/*.c main_pc.c prof_win.c ../switch/tev_gen.c; do
    o=$OUT/rt_$(basename "${f%.c}").o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find ../runtime -name '*.h' -newer "$o")" ]; then jobs+=("gcc $RTF -c $f -o $o"); fi
done
for f in "$GEN"/part_*.c "$GEN"/table.c; do
    o=$OUT/gen_$(basename "${f%.c}").o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ ../runtime/gc_core.h -nt "$o" ]; then jobs+=("gcc $GENF -c $f -o $o"); fi
done
rm -f $OUT/gen_mem.o  # the memory image is loaded from the disc's main.dol now
echo "${#jobs[@]} files to compile"
printf '%s\n' "${jobs[@]}" | xargs -P "$JOBS" -I{} sh -c '{} || exit 255'
gcc -o $OUT/mkdd_pc.exe $OUT/*.o -lm -lpthread -lwinmm -lpsapi
echo "built $OUT/mkdd_pc.exe"
