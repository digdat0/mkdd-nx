"""One-step build: your own Mario Kart: Double Dash!! (USA) disc image -> MKDD-NX.nro.

usage: python tools/build_from_iso.py <disc image (.iso/.gcm/.ciso)> [options]
       (Windows: py -3 tools/build_from_iso.py ...)

Steps (each is skipped when its output is already there; --force redoes them all):
  1. read the disc, check it is GM4E01 and extract main.dol (its SHA-1 must match the sheets)
  2. fetch MKDD-Recomp at a pinned commit (its function list, metadata and Ghidra scripts)
  3. Ghidra headless: import + analyse main.dol, CreateGapFunctions.java, ExportPcode.java
     -> instructions.csv, memory.csv, registers.csv
  4. assemble the sheets folder, run tools/gen_c.py -> generated C
  5. switch/build.sh -> build-switch/sdcard/switch/MKDD-NX/MKDD-NX.nro

Everything made from the disc goes to the work folder (default: ../mkdd-nx-work next to this
repository), never into the repository. It contains the game's code: keep it to yourself.

Options:
  --work DIR      work folder
  --ghidra DIR    Ghidra 12.1.4 install folder (default: $GHIDRA_INSTALL_DIR)
  --recomp DIR    an existing MKDD-Recomp checkout instead of cloning one
  --bash PATH     bash for switch/build.sh (default: Git for Windows' bash on Windows)
  --no-build      stop after generating the C
  --force         redo every step
"""
import csv
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RECOMP_URL = "https://github.com/LeftInWalmart/MKDD-Recomp.git"
RECOMP_COMMIT = "c7fd65e905"
GAME_ID = b"GM4E01"
EXPECT_INSTRUCTIONS = 740738
RECOMP_SHEETS = ["functions.csv", "meta.csv", "hle.csv"]
EXPORT_SHEETS = ["instructions.csv", "memory.csv", "registers.csv"]


def step(msg):
    print(f"\n== {msg}", flush=True)


def fail(msg):
    sys.exit(f"\nerror: {msg}")


def run(cmd, **kw):
    print("   $ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        fail(f"{Path(str(cmd[0])).name} exited with {r.returncode}")


# ---- disc ----
class Disc:
    """Plain .iso/.gcm, or .ciso (block map after a 'CISO' header)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x8000)
        self.bs = 0
        if hdr[:4] == b"CISO":
            self.bs = struct.unpack_from("<I", hdr, 4)[0]
            self.map, n = [], 0
            for used in hdr[8:0x8000]:
                self.map.append(n if used else -1)
                n += used != 0

    def read(self, off, size):
        if not self.bs:
            self.f.seek(off)
            return self.f.read(size)
        out = bytearray()
        while size:
            blk, inner = divmod(off, self.bs)
            k = min(size, self.bs - inner)
            slot = self.map[blk] if blk < len(self.map) else -1
            if slot < 0:
                out += bytes(k)
            else:
                self.f.seek(0x8000 + slot * self.bs + inner)
                out += self.f.read(k)
            off += k
            size -= k
        return bytes(out)


def extract_dol(disc_path, out):
    d = Disc(disc_path)
    gid = d.read(0, 6)
    if gid != GAME_ID:
        fail(f"{disc_path} is {gid!r}, not Mario Kart: Double Dash!! (USA, {GAME_ID.decode()})")
    dol_off = struct.unpack(">I", d.read(0x420, 4))[0]
    h = d.read(dol_off, 0x100)
    offs = struct.unpack_from(">18I", h, 0)
    sizes = struct.unpack_from(">18I", h, 0x90)
    size = max(o + s for o, s in zip(offs, sizes) if s)
    dol = d.read(dol_off, size)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(dol)
    return hashlib.sha1(dol).hexdigest()


# ---- tools ----
def find_ghidra(arg):
    g = Path(arg or os.environ.get("GHIDRA_INSTALL_DIR", ""))
    if not str(g) or not g.is_dir():
        fail("Ghidra not found: pass --ghidra <Ghidra 12.1.4 folder> or set GHIDRA_INSTALL_DIR")
    tool = g / "support" / ("analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")
    if not tool.exists():
        fail(f"{tool} not found: is {g} a Ghidra install folder?")
    ext = [g / "Ghidra" / "Extensions" / "GameCubeLoader"]
    base = Path(os.environ.get("APPDATA", Path.home() / ".config")) / "ghidra"
    ext += list(base.glob("*/Extensions/GameCubeLoader")) + list((Path.home() / ".ghidra").glob("*/Extensions/GameCubeLoader"))
    if not any(e.is_dir() for e in ext):
        fail("the GameCubeLoader extension is not installed in Ghidra (File > Install Extensions, "
             "or unzip it into <Ghidra>/Ghidra/Extensions)")
    return tool


def find_bash(arg):
    if arg:
        return arg
    if os.name == "nt":
        for p in (r"C:\Program Files\Git\bin\bash.exe", r"C:\Program Files (x86)\Git\bin\bash.exe"):
            if Path(p).exists():
                return p
        fail("Git for Windows' bash not found: pass --bash <path to bash.exe> "
             "(not WSL's bash, which can't see devkitPro)")
    return shutil.which("bash") or fail("bash not found")


def count_rows(path):
    with open(path, newline="", encoding="utf-8") as fh:
        return sum(1 for _ in csv.reader(fh)) - 1


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    opts, pos, i = {}, [], 0
    while i < len(args):
        a = args[i]
        if a in ("--work", "--ghidra", "--recomp", "--bash"):
            if i + 1 >= len(args):
                fail(f"{a} needs a value")
            opts[a] = args[i + 1]
            i += 2
        elif a in ("--no-build", "--force"):
            opts[a] = True
            i += 1
        elif a.startswith("--"):
            fail(f"unknown option {a}")
        else:
            pos.append(a)
            i += 1
    if len(pos) != 1:
        fail("give exactly one disc image")
    disc = Path(pos[0]).resolve()
    if not disc.exists():
        fail(f"{disc} not found")
    force = "--force" in opts
    work = Path(opts.get("--work", REPO.parent / "mkdd-nx-work")).resolve()
    if work == REPO or REPO in work.parents:
        fail("the work folder must be outside the repository (it holds the game's code)")
    work.mkdir(parents=True, exist_ok=True)
    print(f"disc: {disc}\nwork: {work}")

    # 1. MKDD-Recomp (function list, metadata, HLE list, Ghidra scripts)
    step("MKDD-Recomp sheets and Ghidra scripts")
    recomp = Path(opts["--recomp"]).resolve() if "--recomp" in opts else work / "MKDD-Recomp"
    if not (recomp / "sheets" / "functions.csv").exists():
        if "--recomp" in opts:
            fail(f"{recomp} is not an MKDD-Recomp checkout")
        run(["git", "clone", "--no-checkout", RECOMP_URL, recomp])
        run(["git", "-C", recomp, "checkout", "-q", RECOMP_COMMIT])
    meta = {r["key"]: r["value"] for r in csv.DictReader(open(recomp / "sheets" / "meta.csv", newline=""))}
    want_sha = meta["main_dol_sha1"]

    # 2. main.dol
    step("extract main.dol")
    dol = work / "extracted" / "main.dol"
    sha = extract_dol(disc, dol)
    if sha != want_sha:
        fail(f"main.dol SHA-1 {sha} does not match the supported build ({want_sha}). "
             "Only the USA release (GM4E01, revision 0) is supported; check your dump.")
    print(f"   {dol} ({dol.stat().st_size} bytes), SHA-1 OK")

    # 3. Ghidra export
    step("Ghidra: analyse and export (about 10-20 minutes)")
    export = work / "export"
    done = export / ".done"
    if force or not done.exists() or done.read_text().strip() != sha:
        tool = find_ghidra(opts.get("--ghidra"))
        proj = work / "ghidra_project"
        if proj.exists():
            shutil.rmtree(proj)
        proj.mkdir(parents=True)
        export.mkdir(parents=True, exist_ok=True)
        for f in EXPORT_SHEETS:
            (export / f).unlink(missing_ok=True)
        # autoloadMaps off: otherwise GameCubeLoader opens a "Load Symbols?" dialog even headless
        run([tool, proj, "MKDD", "-import", dol, "-overwrite", "-loader", "GameCubeLoader",
             "-loader-autoloadMaps", "false", "-scriptPath", recomp / "tools",
             "-postScript", "CreateGapFunctions.java", "-postScript", "ExportPcode.java", export])
        for f in EXPORT_SHEETS:
            if not (export / f).exists():
                fail(f"Ghidra did not write {export / f}: see the output above")
        n = count_rows(export / "instructions.csv")
        if n != EXPECT_INSTRUCTIONS:
            fail(f"Ghidra exported {n} instructions, expected {EXPECT_INSTRUCTIONS}: check the "
                 "Ghidra (12.1.4) and GameCubeLoader versions")
        done.write_text(sha)
    else:
        print("   already exported")

    # 4. sheets + generated C
    step("generate C")
    sheets, gen = work / "sheets", work / "gen"
    sheets.mkdir(exist_ok=True)
    for f in RECOMP_SHEETS:
        shutil.copy2(recomp / "sheets" / f, sheets / f)
    for f in EXPORT_SHEETS:
        dst = sheets / f
        if force or not dst.exists() or dst.stat().st_mtime < (export / f).stat().st_mtime:
            shutil.copy2(export / f, dst)
    run([sys.executable, REPO / "tools" / "gen_c.py", sheets, gen])

    if "--no-build" in opts:
        print(f"\ngenerated C in {gen}; build with: switch/build.sh {gen}")
        return

    # 5. Switch build
    step("build the Switch .nro (the first build takes 30-60 minutes)")
    bash = find_bash(opts.get("--bash"))
    gen_arg = gen.as_posix()
    if os.name == "nt":  # build.sh expects /e/... style paths from Git Bash
        gen_arg = "/" + gen_arg[0].lower() + gen_arg[2:] if gen_arg[1] == ":" else gen_arg
    run([bash, "switch/build.sh", gen_arg], cwd=REPO)
    nro = REPO / "build-switch" / "sdcard" / "switch" / "MKDD-NX" / "MKDD-NX.nro"
    if not nro.exists():
        fail("the build finished without an .nro: see the output above")
    print(f"\ndone: {nro}\nCopy the MKDD-NX folder to sdmc:/switch/ and put your disc image next to MKDD-NX.nro.")


if __name__ == "__main__":
    main()
