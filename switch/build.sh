#!/bin/bash
# Build the Switch .nro:  switch/build.sh <generated C dir> [extra make args]
# Needs devkitPro (devkitA64, libnx, switch-sdl2, switch-mesa, switch-tools, its msys2 cmake + make).
# Output: build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro -> copy to the SD card root, put your
# GM4E01 disc image (.iso/.gcm/.ciso/.rvz) next to it.
set -e
cd "$(dirname "$0")/.."
GEN="${1:?usage: switch/build.sh <generated C dir> (from tools/gen_c.py or tools/build_from_iso.py)}"
[ $# -gt 0 ] && shift

DKP="${DEVKITPRO:-/opt/devkitpro}"
RUN=()
if [ -d /c/devkitPro ]; then
    # Windows: devkitPro's cmake must run from its own msys2, variables don't cross from Git Bash
    # into it (passed with its env.exe), and gcc needs a Windows temp dir.
    mkdir -p build-switch/tmp
    T="$(cygpath -m "$PWD/build-switch/tmp")"
    RUN=(/c/devkitPro/msys2/usr/bin/env.exe
         "PATH=/opt/devkitpro/msys2/usr/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/bin"
         DEVKITPRO=/opt/devkitpro TMPDIR="$T" TMP="$T" TEMP="$T")
    DKP=/opt/devkitpro
    GEN="$(cygpath -u "$GEN")"  # its cmake globs only POSIX paths (/e/...), not E:/...
else
    export DEVKITPRO="$DKP"
fi

# Configure every time: GEN_DIR is a cache variable, so an existing build directory would keep
# compiling the generated C it was first configured with.
"${RUN[@]}" cmake -S switch -B build-switch -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DKP/cmake/Switch.cmake" -DGEN_DIR="$GEN"
# main_switch.c carries the build stamp (boot.txt, the menu's BUILD HHMM): always recompile it.
touch switch/main_switch.c
"${RUN[@]}" cmake --build build-switch -j"$(nproc)" -- "$@"
