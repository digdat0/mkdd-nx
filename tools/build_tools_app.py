"""Packages MKDD-NX Tools (tools/mkddnx_tools.py: the Characters, HD Textures and Battle Routes apps in
one window): Windows -> dist/MKDD-NX-Tools.exe (one file, no console), macOS ->
dist/MKDD-NX-Tools-<arch>.dmg (the .app inside). Python, Pillow, zstandard (for .rvz images), the
BC7 encoder and the repository's routes/ (the ones built into the .nro, the route editor's start)
are bundled, so it needs nothing installed. Run on the platform being built for:

    py -3 -m pip install -r tools/apps_requirements.txt
    py -3 tools/build_tools_app.py [--zstd-from DIR]

Needs a C compiler for the encoder (Windows: MSYS2 ucrt64 gcc or any MinGW gcc; macOS: Xcode's
command line tools). --zstd-from: a folder holding the zstandard package, when the Python running
this has none. (A Python with the obsolete enum34 package can't run PyInstaller: use a venv, e.g.
build-tools/venv.) Output goes to build-tools-app/ and dist/ (both ignored by git);
.github/workflows/apps.yml builds it for each platform.
"""
import argparse
import os
from pathlib import Path

import hdtex_pack
import pyi_app

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build-tools-app"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zstd-from", help="a folder holding the zstandard package, added to the search path")
    a = ap.parse_args()
    WORK.mkdir(exist_ok=True)
    lib = WORK / hdtex_pack.LIB.name
    hdtex_pack.build_encoder(lib, native=False)  # any CPU of this architecture, not just this one
    extra = ["--paths", a.zstd_from] if a.zstd_from else []
    hidden = ["charpack", "route_editor", "battle_routes", "build_from_iso", "rvz", "zstandard", "hdtex_gui", "hdtex_pack"]
    pyi_app.build("MKDD-NX Tools", "MKDD-NX-Tools", ROOT / "tools" / "mkddnx_tools.py", WORK, "io.github.digdat0.mkddnx.tools",
                  ["--paths", str(ROOT / "tools"), "--paths", str(ROOT / "tools" / "hdtex_app"), *extra,
                   *[x for h in hidden for x in ("--hidden-import", h)],
                   "--add-binary", f"{lib}{os.pathsep}.",
                   "--add-data", f"{ROOT / 'routes' / '*.txt'}{os.pathsep}routes"])


if __name__ == "__main__":
    main()
