"""Packages the battle route editor (tools/route_editor.py): Windows -> dist/MKDD-NX-Route-Editor.exe (one
file, no console), macOS -> dist/MKDD-NX-Route-Editor-<arch>.dmg (the .app inside). It asks for the
routes folder, the disc image and MKDD Plus's folder, and remembers them. Python is bundled, so it needs
nothing installed. It carries the repository's routes/ (the ones built into the .nro) to start from.
Run on the platform being built for:

    py -3 -m pip install pyinstaller==6.16.0 pillow==12.0.0 zstandard==0.25.0
    py -3 tools/build_route_editor_app.py [--zstd-from DIR]

--zstd-from: a folder holding the zstandard package (for .rvz images), when the Python running this has
none (e.g. the system Python's site-packages, of the same Python version). Output goes to
build-route-editor-app/ and dist/ (both ignored by git). .github/workflows/apps.yml builds both.
"""
import argparse
import os
from pathlib import Path

import pyi_app

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zstd-from", help="a folder holding the zstandard package, added to the search path")
    a = ap.parse_args()
    extra = ["--paths", a.zstd_from] if a.zstd_from else []
    pyi_app.build("MKDD-NX Route Editor", "MKDD-NX-Route-Editor", ROOT / "tools" / "route_editor.py",
                  ROOT / "build-route-editor-app", "io.github.digdat0.mkddnx.routeeditor",
                  ["--paths", str(ROOT / "tools"), *extra,
                   "--hidden-import", "rvz", "--hidden-import", "zstandard", "--hidden-import", "build_from_iso",
                   "--hidden-import", "charpack", "--add-data", f"{ROOT / 'routes' / '*.txt'}{os.pathsep}routes"])


if __name__ == "__main__":
    main()
