"""Packages the character pack tool (tools/charpack.py): Windows -> dist/MKDD-NX-Characters.exe (one
file, no console; the window opens, folders can be dropped on it), macOS ->
dist/MKDD-NX-Characters-<arch>.dmg (the .app inside; no dropping, use the Add button). Python is
bundled, so it needs nothing installed. Run on the platform being built for:

    py -3 -m pip install pyinstaller==6.16.0 pillow==12.0.0
    py -3 tools/build_charpack_app.py

(a Python with the obsolete enum34 package can't run PyInstaller: use a venv, e.g. build-tools/venv.)
Output goes to build-charpack-app/ and dist/ (both ignored by git). .github/workflows/apps.yml builds
both.
"""
from pathlib import Path

import pyi_app

ROOT = Path(__file__).resolve().parent.parent


def main():
    pyi_app.build("MKDD-NX Characters", "MKDD-NX-Characters", ROOT / "tools" / "charpack.py",
                  ROOT / "build-charpack-app", "io.github.digdat0.mkddnx.characters")


if __name__ == "__main__":
    main()
