"""tools/mmio_keep.txt from census runs: the guest functions seen reaching hardware addresses.

usage: py -3 tools/mmio_census.py MKDD-NX.elf mkdd.log [mkdd.log ...]   (build with MKDD_MMIO_CENSUS=ON)

Prints the list; gen_c.py reads tools/mmio_keep.txt ("<address> <name>" lines). Sites are offsets from
rt_mmio_read, so the symbol table gives the f_<ADDR> / fp<n>_<ADDR> function each is in.
"""
import bisect, os, re, subprocess, sys

_DKP = os.environ.get("DEVKITPRO", "")
NM = os.path.join(_DKP if os.path.isdir(_DKP) else "C:/devkitPro", "devkitA64/bin/aarch64-none-elf-nm")


def main():
    addrs, names, base = [], [], None
    for line in subprocess.run([NM, "-n", "--defined-only", sys.argv[1]], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
            if p[2] == "rt_mmio_read":
                base = addrs[-1]
    found, other = {}, {}
    for log in sys.argv[2:]:
        for m in re.finditer(r"mmio site (-?\d+) addr (0x[0-9a-f]+)", open(log, errors="replace").read()):
            name = names[bisect.bisect_right(addrs, base + int(m[1])) - 1]
            g = re.fullmatch(r"fp?i?\d*_([0-9A-F]{8})", name)
            (found if g else other).setdefault(int(g[1], 16) if g else name, set()).add(m[2])
    print("# guest functions seen reaching hardware addresses (tools/mmio_census.py)")
    for a in sorted(found):
        print(f"{a:08x} {' '.join(sorted(found[a]))}")
    for n in other:
        print(f"# not generated code: {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
