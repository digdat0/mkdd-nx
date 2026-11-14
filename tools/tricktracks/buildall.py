"""Builds the sixteen cup tracks of cups8.txt (or the named ones) into <outdir>, then their art, then checks them
(trackval.py); exits 1 when the check fails, so nothing broken gets packed. Pack afterwards with tools/trickpak.py.

usage: py -3.10 tools/tricktracks/buildall.py <outdir> [track ...] [--disc GM4E01 image] [--superbmd SuperBMD.exe]
       [--work DIR] [--no-art]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamefiles  # noqa: E402

a = sys.argv[1:]
work = gamefiles.flag(a, "--work", gamefiles.WORK)
sbmd = gamefiles.flag(a, "--superbmd")
disc = gamefiles.flag(a, "--disc")
art = "--no-art" not in a
a = [x for x in a if x != "--no-art"]
if not a:
    sys.exit(__doc__)
out, only = a[0], set(a[1:])
if disc:
    gamefiles.fetch(disc, work, sbmd)
cups = [l.split() for l in open(os.path.join(HERE, "cups8.txt")) if l.strip()]
unknown = only - {t for _, t in cups}
if unknown:
    sys.exit(f"not in cups8.txt: {', '.join(sorted(unknown))}")
extra = ["--work", work] + (["--superbmd", sbmd] if sbmd else [])
for stem, trk in cups:
    if only and trk not in only:
        continue
    print(f"== {stem} {trk}", flush=True)
    subprocess.run([sys.executable, os.path.join(HERE, "build7.py"), out, trk, stem] + extra, check=True)
if art:
    subprocess.run([sys.executable, os.path.join(HERE, "art8.py"), out, "--work", work], check=True)
sys.exit(subprocess.run([sys.executable, os.path.join(HERE, "trackval.py"), out]).returncode)
