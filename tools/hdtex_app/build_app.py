"""Packages the HD texture app: Windows -> dist/MKDD-NX-HD-Textures.exe (one file), macOS ->
dist/MKDD-NX-HD-Textures-<arch>.dmg (the .app inside). Python, Pillow and the BC7 encoder are
bundled, so the app needs nothing installed. Run on the platform being built for:

    py -3 -m pip install -r tools/hdtex_app/requirements.txt
    py -3 tools/hdtex_app/build_app.py

Needs a C compiler for the encoder (Windows: MSYS2 ucrt64 gcc or any MinGW gcc; macOS: Xcode's
command line tools). Output goes to build-hdtex-app/ and dist/ (both ignored by git).
"""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import hdtex_pack  # noqa: E402

NAME = "MKDD-NX HD Textures"
FILE = "MKDD-NX-HD-Textures"
WORK = ROOT / "build-hdtex-app"
DIST = ROOT / "dist"


def main():
    WORK.mkdir(exist_ok=True)
    DIST.mkdir(exist_ok=True)
    lib = WORK / hdtex_pack.LIB.name
    hdtex_pack.build_encoder(lib, native=False)  # any CPU of this architecture, not just this one
    from PIL import Image
    icon = WORK / "icon.png"
    Image.open(ROOT / "switch" / "icon.jpg").save(icon)  # PyInstaller makes the .ico / .icns from it

    mac = sys.platform == "darwin"
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed",
           "--name", NAME if mac else FILE, "--icon", str(icon),
           "--paths", str(ROOT / "tools"), "--add-binary", f"{lib}{os.pathsep}.",
           "--workpath", str(WORK / "pyi"), "--specpath", str(WORK), "--distpath", str(WORK / "dist"),
           "--onedir" if mac else "--onefile"]
    if mac:
        cmd += ["--osx-bundle-identifier", "io.github.digdat0.mkddnx.hdtextures"]
    cmd.append(str(HERE / "hdtex_gui.py"))
    subprocess.run(cmd, check=True)

    if mac:
        app = WORK / "dist" / f"{NAME}.app"
        stage = WORK / "dmg"
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir()
        shutil.copytree(app, stage / app.name, symlinks=True)
        os.symlink("/Applications", stage / "Applications")
        dmg = DIST / f"{FILE}-{platform.machine()}.dmg"
        dmg.unlink(missing_ok=True)
        subprocess.run(["hdiutil", "create", "-volname", NAME, "-srcfolder", str(stage), "-ov", "-format", "UDZO",
                        str(dmg)], check=True)
    else:
        dmg = DIST / f"{FILE}.exe"
        shutil.copy2(WORK / "dist" / f"{FILE}.exe", dmg)
    print("built", dmg)


if __name__ == "__main__":
    main()
