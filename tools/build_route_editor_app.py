"""Packages the battle route editor (tools/route_editor.py): Windows -> dist/MKDD-NX-Route-Editor.exe (one
file, no console; it asks for the routes folder, the disc image and MKDD Plus's folder, and remembers
them). Python is bundled, so it needs nothing installed. It carries the repository's routes/ (the ones
built into the .nro) to start from. Run on Windows:

    py -3 -m pip install pyinstaller==6.16.0 pillow==12.0.0 zstandard==0.25.0
    py -3 tools/build_route_editor_app.py [--zstd-from DIR]

--zstd-from: a folder holding the zstandard package (for .rvz images), when the Python running this has
none (e.g. the system Python's site-packages, of the same Python version). Output goes to
build-route-editor-app/ and dist/ (both ignored by git).
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILE = "MKDD-NX-Route-Editor"
WORK = ROOT / "build-route-editor-app"
DIST = ROOT / "dist"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zstd-from", help="a folder holding the zstandard package, added to the search path")
    a = ap.parse_args()
    WORK.mkdir(exist_ok=True)
    DIST.mkdir(exist_ok=True)
    from PIL import Image
    icon = WORK / "icon.png"
    Image.open(ROOT / "switch" / "icon.jpg").save(icon)  # PyInstaller makes the .ico from it
    extra = ["--paths", a.zstd_from] if a.zstd_from else []
    sep = ";" if sys.platform == "win32" else ":"
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed", "--onefile",
                    "--name", FILE, "--icon", str(icon), "--workpath", str(WORK / "pyi"), "--specpath", str(WORK),
                    "--distpath", str(WORK / "dist"), "--paths", str(ROOT / "tools"), *extra,
                    "--hidden-import", "rvz", "--hidden-import", "zstandard", "--hidden-import", "build_from_iso",
                    "--hidden-import", "charpack", "--add-data", f"{ROOT / 'routes' / '*.txt'}{sep}routes",
                    str(ROOT / "tools" / "route_editor.py")], check=True)
    out = DIST / f"{FILE}.exe"
    shutil.copy2(WORK / "dist" / f"{FILE}.exe", out)
    print("built", out)


if __name__ == "__main__":
    main()
