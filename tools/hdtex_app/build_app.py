"""Packages the HD texture app: Windows -> dist/MKDD-NX-HD-Textures.exe (one file), macOS ->
dist/MKDD-NX-HD-Textures-<arch>.dmg (the .app inside). Python, Pillow and the BC7 encoder are
bundled, so the app needs nothing installed. Run on the platform being built for:

    py -3 -m pip install -r tools/hdtex_app/requirements.txt
    py -3 tools/hdtex_app/build_app.py

Needs a C compiler for the encoder (Windows: MSYS2 ucrt64 gcc or any MinGW gcc; macOS: Xcode's
command line tools). Output goes to build-hdtex-app/ and dist/ (both ignored by git).
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import hdtex_pack  # noqa: E402
import pyi_app  # noqa: E402

WORK = ROOT / "build-hdtex-app"


def main():
    WORK.mkdir(exist_ok=True)
    lib = WORK / hdtex_pack.LIB.name
    hdtex_pack.build_encoder(lib, native=False)  # any CPU of this architecture, not just this one
    pyi_app.build("MKDD-NX HD Textures", "MKDD-NX-HD-Textures", HERE / "hdtex_gui.py", WORK,
                  "io.github.digdat0.mkddnx.hdtextures",
                  ["--paths", str(ROOT / "tools"), "--add-binary", f"{lib}{os.pathsep}."])


if __name__ == "__main__":
    main()
