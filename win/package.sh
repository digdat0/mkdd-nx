#!/bin/bash
# The Windows release zip: dist/MKDD-NX-Windows-<version>.zip, made from a fresh build.
#   win/package.sh <generated C dir> [jobs]
# Only MKDD-NX.exe is taken from build-win/; nothing else there (config, saves, logs, mods, the
# disc image) can reach the zip. The zip holds one folder, MKDD-NX/: the exe, README.txt,
# LICENSE.txt, THIRD-PARTY-NOTICES.txt (the licences of what the exe links statically, from
# MSYS2's own copies) and empty mods/ folders with a note each.
set -e
GEN=${1:?usage: win/package.sh <generated C dir> [jobs]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LIC=/c/msys64/ucrt64/share/licenses
VER=$(sed -n 's/^set(MKDD_VERSION "\(.*\)")/\1/p' "$ROOT/switch/CMakeLists.txt")
[ -n "$VER" ] || { echo "no MKDD_VERSION in switch/CMakeLists.txt"; exit 1; }

"$HERE/build_win.sh" "$GEN" ${2:+"$2"}
EXE=$ROOT/build-win/MKDD-NX.exe
[ -s "$EXE" ] || { echo "no $EXE"; exit 1; }

STAGE=$ROOT/dist/win-stage
PKG=$STAGE/MKDD-NX
rm -rf "$STAGE"
mkdir -p "$PKG"
cp "$EXE" "$PKG/"
strip_cr() { sed 's/\r$//' "$1" | sed 's/$/\r/'; } # Windows line ends for Notepad
strip_cr "$HERE/release/README.txt" > "$PKG/README.txt"
strip_cr "$ROOT/LICENSE" > "$PKG/LICENSE.txt"

# what the exe links statically: SDL2 (zlib), zstd (BSD), liblzma (0BSD), the MinGW-w64 runtime
# and winpthreads, GCC's runtime library (GCC Runtime Library Exception)
{
    echo "MKDD-NX.exe includes the following third-party software, statically linked."
    echo "Each is used under the licence below."
    for part in "SDL2|Simple DirectMedia Layer 2 (zlib licence)|$LIC/SDL2/LICENSE.txt" \
                "zstd|Zstandard (BSD licence; zstd is dual BSD / GPLv2, used here under BSD)|$LIC/zstd/LICENSE" \
                "xz|liblzma from XZ Utils (0BSD)|$LIC/xz/COPYING.0BSD" \
                "crt|MinGW-w64 runtime|$LIC/crt/COPYING.MinGW-w64-runtime.txt" \
                "winpthreads|MinGW-w64 winpthreads|$LIC/winpthreads/COPYING" \
                "gcc|GCC runtime library (libgcc), under the GCC Runtime Library Exception|$LIC/gcc-libs/COPYING.RUNTIME"; do
        IFS='|' read -r key title file <<< "$part"
        [ -s "$file" ] || { echo "missing licence file $file" >&2; exit 1; }
        printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$title"
        cat "$file"
    done
} > "$STAGE/notices.txt"
strip_cr "$STAGE/notices.txt" > "$PKG/THIRD-PARTY-NOTICES.txt"
rm "$STAGE/notices.txt"

note() { mkdir -p "$PKG/mods/$1"; printf '%s\r\n' "$2" > "$PKG/mods/$1/PUT FILES HERE.txt"; }
note hd "Put the HD texture pack here: mods\\hd\\hd-textures.bin"
note characters "Put extra characters here, one folder each (see README.txt)."
note karts "Put extra karts here, one folder each (see README.txt)."
note audio "Put MKDD Plus's stereo music here (its AudioRes\\Stream files)."

ZIP=$ROOT/dist/MKDD-NX-Windows-$VER.zip
rm -f "$ZIP"
py -3 -I - "$(cygpath -w "$STAGE")" "$(cygpath -w "$ZIP")" <<'EOF'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, dirs, files in os.walk(stage):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, stage).replace(os.sep, "/"))
EOF
rm -rf "$STAGE"
echo "packaged $ZIP ($(du -h "$ZIP" | cut -f1))"
