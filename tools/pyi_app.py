"""Shared by tools/build_charpack_app.py and tools/build_route_editor_app.py: PyInstaller packaging.
Windows -> dist/<FILE>.exe (one file); macOS -> dist/<FILE>-<arch>.dmg (the .app inside, with an
Applications link to drag it on). No console window either way.
"""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"


def build(name, file, script, work, bundle_id, extra=()):
    """name: the macOS app's name; file: the output's file name; extra: more PyInstaller arguments"""
    work.mkdir(exist_ok=True)
    DIST.mkdir(exist_ok=True)
    from PIL import Image
    icon = work / "icon.png"
    Image.open(ROOT / "switch" / "icon.jpg").save(icon)  # PyInstaller makes the .ico / .icns from it
    mac = sys.platform == "darwin"
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed",
           "--name", name if mac else file, "--icon", str(icon), "--workpath", str(work / "pyi"),
           "--specpath", str(work), "--distpath", str(work / "dist"), "--onedir" if mac else "--onefile", *extra]
    if mac:
        cmd += ["--osx-bundle-identifier", bundle_id]
    subprocess.run(cmd + [str(script)], check=True)
    if mac:
        app = work / "dist" / f"{name}.app"
        stage = work / "dmg"
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir()
        shutil.copytree(app, stage / app.name, symlinks=True)
        os.symlink("/Applications", stage / "Applications")
        out = DIST / f"{file}-{platform.machine()}.dmg"
        out.unlink(missing_ok=True)
        subprocess.run(["hdiutil", "create", "-volname", name, "-srcfolder", str(stage), "-ov", "-format", "UDZO",
                        str(out)], check=True)
    else:
        out = DIST / f"{file}.exe"
        shutil.copy2(work / "dist" / f"{file}.exe", out)
    print("built", out)
    return out
