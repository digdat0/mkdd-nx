#!/usr/bin/env bash
# Publish a build for the in-game updater (switch/update.c), and open the update window.
#
#   tools/release.sh publish ["notes"]   upload build-switch/MKDD-NX.nro (+ update.txt) as release v<version>
#                                        (version from switch/CMakeLists.txt; an existing release of
#                                        that version gets its files replaced: same-version rebuilds)
#   tools/release.sh window [seconds]    make the repository public, wait (Enter, or the seconds given),
#                                        make it private again. Private again on Ctrl-C or any error.
#
# The updater reads the latest release from api.github.com (its tag, and the .nro's size and the
# SHA-256 GitHub records on upload), with no token, so it only works while the repository is public.
# update.txt is only for builds up to 1.6.1, which read nothing else (newer ones fall back to it
# when the API is refused): drop it once players have moved on. Never put game files in a release.
set -euo pipefail
REPO=digdat0/mkdd-nx
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

case "${1:-}" in
publish)
    NRO="$ROOT/build-switch/MKDD-NX.nro"
    [ -f "$NRO" ] || { echo "no $NRO: run switch/build.sh first" >&2; exit 1; }
    VER=$(sed -n 's/^set(MKDD_VERSION "\(.*\)")/\1/p' "$ROOT/switch/CMakeLists.txt")
    [ -n "$VER" ] || { echo "MKDD_VERSION not found in switch/CMakeLists.txt" >&2; exit 1; }
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    cp "$NRO" "$TMP/MKDD-NX.nro"
    SIZE=$(stat -c %s "$TMP/MKDD-NX.nro")
    SHA=$(sha256sum "$TMP/MKDD-NX.nro" | cut -d' ' -f1)
    printf 'version=%s\nsize=%s\nsha256=%s\n' "$VER" "$SIZE" "$SHA" > "$TMP/update.txt"
    cat "$TMP/update.txt"
    if gh release view "v$VER" --repo "$REPO" > /dev/null 2>&1; then
        gh release upload "v$VER" --repo "$REPO" --clobber "$TMP/MKDD-NX.nro" "$TMP/update.txt"
        echo "replaced the files of release v$VER"
    else
        gh release create "v$VER" --repo "$REPO" --title "v$VER" --notes "${2:-MKDD-NX $VER}" --latest \
            "$TMP/MKDD-NX.nro" "$TMP/update.txt"
        echo "created release v$VER"
    fi
    ;;
window)
    private() { gh repo edit "$REPO" --visibility private --accept-visibility-change-consequences && echo "$REPO is private"; }
    trap private EXIT
    gh repo edit "$REPO" --visibility public --accept-visibility-change-consequences
    echo "$REPO is PUBLIC: update on the Switch now (- menu > UPDATE)"
    if [ -n "${2:-}" ]; then
        sleep "$2"
    else
        read -r -p "press Enter when the Switch has downloaded the update... " _
    fi
    ;;
*)
    sed -n '2,13p' "$0"
    exit 1
    ;;
esac
