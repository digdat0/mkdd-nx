#!/bin/bash
# Build the Switch .nro:  switch/build.sh <generated C dir> [extra make args]
# Needs devkitPro (devkitA64, libnx, deko3d, switch-sdl2, switch-tools, its msys2 cmake + make).
# Output: build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro (built in build-switch-dk) -> copy to
# the SD card root, put your (MKDD_DEKO3D_DEBUG=1: deko3d's validating library)
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
BUILD=build-switch-dk
DKDEBUG=OFF
[ "${MKDD_DEKO3D_DEBUG:-0}" = 1 ] && DKDEBUG=ON
"${RUN[@]}" cmake -S switch -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DKP/cmake/Switch.cmake" -DGEN_DIR="$GEN" \
    -DMKDD_DEKO3D_DEBUG="$DKDEBUG" -DMKDD_HOT_ORDER="${MKDD_HOT_ORDER:-ON}" -DMKDD_MMIO_CENSUS="${MKDD_MMIO_CENSUS:-OFF}" \
    -DMKDD_FRAME_POINTERS="${MKDD_FRAME_POINTERS:-OFF}"
# devkitPro's msys make misses header changes (its depend files name E:/... paths): drop every object older
# than a project header its .d lists, so a struct change (trick.h) can't leave a reader on the old layout.
ROOT="$(cygpath -m "$PWD" 2>/dev/null || pwd)"
for h in runtime/*.h switch/*.h; do
    find "$BUILD/CMakeFiles" -name '*.o' ! -newer "$h" 2>/dev/null | while read -r o; do
        if [ -f "$o.d" ] && grep -qF "$ROOT/$h" "$o.d"; then rm -f "$o"; echo "stale (header $h): $o"; fi
    done
done
# CMAKE_DEPENDS_USE_COMPILER=FALSE (CMakeLists.txt) still makes each object depend on a compiler_depend.ts
# that a fresh build folder never gets ("No rule to make target ... compiler_depend.ts"): create them.
for d in "$BUILD"/CMakeFiles/*.dir "$BUILD"/uam/CMakeFiles/*.dir; do
    [ -f "$d/build.make" ] && [ ! -f "$d/compiler_depend.ts" ] && : > "$d/compiler_depend.ts"
done
# main_switch.c carries the build stamp (boot.txt, the menu's BUILD HHMM): always recompile it.
touch switch/main_switch.c
"${RUN[@]}" cmake --build "$BUILD" -j"$(nproc)" -- "$@"
