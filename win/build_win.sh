#!/bin/bash
# Windows x64 build (MKDD-NX.exe). Run from Git Bash; it re-enters C:\msys64's UCRT64 shell (gcc there).
#   win/build_win.sh <generated C dir> [jobs]
# Output: build-win/MKDD-NX.exe, one file with no DLLs: put it next to the disc image.
# Only recompiles sources newer than their object file (and everything after a header change).
# A GL function new to render_gl.c / main_win.c goes into gl_funcs.h and gl_names.h by hand.
set -e
GEN=${1:?usage: win/build_win.sh <generated C dir> [jobs]}
JOBS=${2:-10}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=$ROOT/build-win
if [ -z "$MKDD_IN_UCRT" ]; then
    mkdir -p "$OUT/tmp"; T="$(cygpath -m "$OUT/tmp")"
    exec /c/msys64/usr/bin/env MSYSTEM=UCRT64 CHERE_INVOKING=1 MKDD_IN_UCRT=1 TMP="$T" TEMP="$T" \
        /c/msys64/usr/bin/bash -lc "'$HERE/build_win.sh' '$GEN' '$JOBS'"
fi
cd "$HERE"
mkdir -p "$OUT/obj" "$OUT/version"
VER=$(sed -n 's/^set(MKDD_VERSION "\(.*\)")/\1/p' ../switch/CMakeLists.txt)
LINE="#define MKDD_VERSION \"$VER\""
[ "$(cat "$OUT/version/mkdd_version.h" 2>/dev/null)" = "$LINE" ] || echo "$LINE" > "$OUT/version/mkdd_version.h"
# the battle routes built in (as switch/CMakeLists.txt): routes/<key>.txt, embedded whole, with a table of keys
RS="$OUT/routes_data.S"
{
    echo '    .section .rdata,"dr"'
    n=0; tab=""
    for f in $(ls ../routes/*.txt 2>/dev/null | sort); do
        k=$(basename "$f" .txt)
        [[ "$k" =~ ^[0-9a-f][0-9a-f]_[0-9a-f]+$ ]] || continue
        echo "route_key_$n:"; echo "    .asciz \"$k\""; echo "    .balign 4"
        echo "route_data_$n:"; echo "    .incbin \"$k.txt\""; echo "route_end_$n:"
        tab="$tab    .quad route_key_$n, route_data_$n, route_end_$n\n"; n=$((n+1))
    done
    echo '    .data'; echo '    .balign 8'; echo '    .global route_builtin'; echo 'route_builtin:'
    printf "$tab"; echo '    .quad 0, 0, 0'
} > "$RS.new"
cmp -s "$RS.new" "$RS" || mv "$RS.new" "$RS"; rm -f "$RS.new"
CF="-std=gnu11 -O2 -ffp-contract=off -fwrapv -fno-strict-aliasing -fno-math-errno -fno-trapping-math -I../runtime -I$GEN -I$OUT/version"
RTF="$CF -Wall -Wno-unused-function"
GENF="$CF -w -fomit-frame-pointer"
jobs=()
newer_h() { [ -n "$(find ../runtime ../switch . -maxdepth 1 -name '*.h' -newer "$1" 2>/dev/null | head -1)" ]; }
for f in ../runtime/*.c ../switch/tev_gen.c ../switch/menu.c ../switch/ui_font.c ../switch/pause_art.c ../switch/charpack_art.c \
         ../switch/hdtex.c main_win.c render_gl.c gl_load.c stubs_win.c launcher.c pad_sdl.c wincfg.c; do
    o=$OUT/obj/rt_$(basename "${f%.c}").o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || newer_h "$o"; then jobs+=("gcc $RTF -c $f -o $o"); fi
done
for f in "$GEN"/part_*.c "$GEN"/table.c; do
    o=$OUT/obj/gen_$(basename "${f%.c}").o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ ../runtime/gc_core.h -nt "$o" ] || [ ../runtime/native.h -nt "$o" ]; then jobs+=("gcc $GENF -c $f -o $o"); fi
done
[ mkdd.rc -nt "$OUT/obj/mkdd_rc.o" ] || [ mkdd.manifest -nt "$OUT/obj/mkdd_rc.o" ] || [ mkdd.ico -nt "$OUT/obj/mkdd_rc.o" ] && jobs+=("windres mkdd.rc -O coff -o $OUT/obj/mkdd_rc.o")
jobs+=("gcc -c data_win.S -I../switch -o $OUT/obj/data_win.o" "gcc -c $RS -I../routes -o $OUT/obj/routes_data.o")
echo "${#jobs[@]} files to compile"
printf '%s\n' "${jobs[@]}" | xargs -P "$JOBS" -I{} sh -c '{} || exit 255'
gcc -o "$OUT/MKDD-NX.exe" "$OUT"/obj/*.o -static -mwindows -l:libzstd.a -l:libSDL2.a -l:liblzma.a -lm -lpthread -lwinmm -lpsapi -lws2_32 -lgdi32 -lopengl32     -lcomctl32 -lcomdlg32 -lshell32 -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lcfgmgr32 -ldinput8 -luxtheme -ldwmapi
echo "built $OUT/MKDD-NX.exe ($VER)"
