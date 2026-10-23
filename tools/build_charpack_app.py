"""Packages the character pack tool (tools/charpack.py): Windows -> dist/MKDD-NX-Characters.exe (one
file, no console; the window opens, folders can be dropped on it). Python is bundled, so it needs
nothing installed. Run on Windows:

    py -3 -m pip install pyinstaller==6.16.0 pillow==12.0.0
    py -3 tools/build_charpack_app.py

(a Python with the obsolete enum34 package can't run PyInstaller: use a venv, e.g. build-tools/venv.)
Output goes to build-charpack-app/ and dist/ (both ignored by git).
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILE = "MKDD-NX-Characters"
WORK = ROOT / "build-charpack-app"
DIST = ROOT / "dist"


def main():
    WORK.mkdir(exist_ok=True)
    DIST.mkdir(exist_ok=True)
    from PIL import Image
    icon = WORK / "icon.png"
    Image.open(ROOT / "switch" / "icon.jpg").save(icon)  # PyInstaller makes the .ico from it
    subprocess.run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed", "--onefile",
                    "--name", FILE, "--icon", str(icon), "--workpath", str(WORK / "pyi"), "--specpath", str(WORK),
                    "--distpath", str(WORK / "dist"), str(ROOT / "tools" / "charpack.py")], check=True)
    out = DIST / f"{FILE}.exe"
    shutil.copy2(WORK / "dist" / f"{FILE}.exe", out)
    print("built", out)


if __name__ == "__main__":
    main()
