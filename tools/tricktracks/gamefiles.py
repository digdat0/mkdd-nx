"""The game's files the track builder works from, taken from the player's own disc image (GM4E01) into the work
folder (default tools/tricktracks/work, kept out of git: it holds the game's data). Nothing here is shipped.

  work/Waluigi.arc           collision flag rule (build7 learns it from waluigi_course.bco)
  work/mini/MiniN.arc        the battle slots' archives (the cup tracks are built on Cookie Land's, Mini7.arc)
  work/wal_materials.json    SuperBMD's export of waluigi_course.bmd (build7's material template, wl_dirt_v)
  work/ms/Mini7_name.bti     the course banner's header (art8: format fields only)
  work/menu.arc              the mode page's archive (menu/menu4.py adds the TRICKS row)
  work/balloon00.bti, work/cup_pict_balloon.bti  icon templates (menu/icons.py, cuppict.py: format fields only)

usage: py -3.10 gamefiles.py <disc image> [--work DIR] [--superbmd PATH]   (build7.py --disc does the same)
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # tools/ (charpack: disc reader, yaz0, rarc)
import charpack  # noqa: E402

WORK = os.path.join(HERE, "work")


def superbmd(path=None):
    """SuperBMD.exe: the --superbmd path, else SUPERBMD in the environment, else on PATH (not in the repo)"""
    p = path or os.environ.get("SUPERBMD") or shutil.which("SuperBMD") or shutil.which("SuperBMD.exe")
    if not p or not os.path.isfile(p):
        sys.exit("SuperBMD.exe not found: pass --superbmd <path to SuperBMD.exe> (github.com/RenolY2/SuperBMD releases)")
    return os.path.abspath(p)


def fetch(disc_path, work=WORK, sbmd=None):
    """extracts what is missing from the disc into work (files already there are kept)"""
    os.makedirs(os.path.join(work, "mini"), exist_ok=True)
    os.makedirs(os.path.join(work, "ms"), exist_ok=True)
    d = charpack.Disc(disc_path)

    def put(rel, data):
        p = os.path.join(work, rel)
        if not os.path.exists(p):
            with open(p, "wb") as f:
                f.write(data)

    put("Waluigi.arc", d.file("Course/Waluigi.arc"))
    for n in range(1, 9):
        if f"course/mini{n}.arc" in d.files:
            put(f"mini/Mini{n}.arc", d.file(f"Course/Mini{n}.arc"))
    put("ms/Mini7_name.bti", d.file("CourseName/English/Mini7_name.bti"))
    menu = d.file("SceneData/English/menu.arc")
    put("menu.arc", menu)
    put("balloon00.bti", charpack.rarc(charpack.yaz0(menu))[("timg", "balloon00.bti")])  # icons.py's template
    if not os.path.exists(os.path.join(work, "cup_pict_balloon.bti")):  # cuppict.py's template (MRAM.arc race2d.arc)
        r2 = charpack.rarc(charpack.yaz0(charpack.rarc(charpack.yaz0(d.file("MRAM.arc")))[("mram", "race2d.arc")]))
        put("cup_pict_balloon.bti", r2[("timg", "cup_pict_balloon.bti")])
    mats = os.path.join(work, "wal_materials.json")
    if not os.path.exists(mats):
        tmp = os.path.join(work, "wal_export")
        os.makedirs(tmp, exist_ok=True)
        wal = charpack.rarc(charpack.yaz0(d.file("Course/Waluigi.arc")))
        bmd = os.path.join(tmp, "waluigi_course.bmd")
        with open(bmd, "wb") as f:
            f.write(wal[("waluigi", "waluigi_course.bmd")])
        subprocess.run([superbmd(sbmd), bmd, os.path.join(tmp, "wal.dae")], check=True, cwd=tmp, stdout=subprocess.DEVNULL)
        shutil.copy(os.path.join(tmp, "waluigi_course_materials.json"), mats)
        shutil.rmtree(tmp)
    return work


def need(work, *rel):
    """exits with how to get them when any of the work files is missing"""
    miss = [r for r in rel if not os.path.exists(os.path.join(work, r))]
    if miss:
        sys.exit(f"{work}: missing {', '.join(miss)}; pass --disc <your GM4E01 disc image> (once) to extract them")


def flag(argv, name, default=None):
    """removes --name VALUE from argv and returns VALUE"""
    if name in argv:
        i = argv.index(name)
        v = argv[i + 1]
        del argv[i:i + 2]
        return v
    return default


if __name__ == "__main__":
    a = sys.argv[1:]
    w = flag(a, "--work", WORK)
    s = flag(a, "--superbmd")
    if len(a) != 1:
        sys.exit(__doc__)
    print("game files in", fetch(a[0], w, s))
