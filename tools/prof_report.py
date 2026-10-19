"""Report for the Switch sampling profiler (switch/prof_switch.c).

usage: py -3 tools/prof_report.py mkdd_prof.bin MKDD-NX.elf [--sheets <sheets dir, for guest function names>] [--from S] [--to S] [--top N]

Self time: the function the game thread was executing. Inclusive: every function on its stack
(each counted once per sample). Percentages are of the game thread's busy samples (idle excluded).
"""
import argparse, bisect, csv, os, struct, subprocess, sys
from collections import Counter, defaultdict

# DEVKITPRO, unless it is a path Windows can't open (Git Bash and msys2 set /opt/devkitpro)
_DKP = os.environ.get("DEVKITPRO", "")
NM = os.path.join(_DKP if os.path.isdir(_DKP) else "C:/devkitPro", "devkitA64/bin/aarch64-none-elf-nm")
K_GAME, K_IDLE, K_GPU, K_FAIL, K_RENDER = 1, 2, 3, 4, 5


def load_syms(elf):
    out = subprocess.run([NM, "-n", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) != 3 or p[1] not in "tTwW":
            continue
        a = int(p[0], 16)
        n = p[2].split(".")[0]  # foo.cold / foo.part.0 / foo.isra.0 -> foo
        if names and addrs[-1] == a:
            continue
        addrs.append(a)
        names.append(n)
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("elf")
    ap.add_argument("--sheets", help="MKDD-Recomp sheets folder (functions.csv) to name guest functions")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e12)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--under", help="only samples with this game function on the stack (address or name part)")
    a = ap.parse_args()

    guest = {}
    if a.sheets:
        with open(os.path.join(a.sheets, "functions.csv"), newline="") as f:
            for r in csv.DictReader(f):
                guest[int(r["addr"], 16)] = r["name"]
    addrs, names = load_syms(a.elf)

    def sym(off):
        if not off:
            return "?"
        i = bisect.bisect_right(addrs, off) - 1
        return names[i] if i >= 0 else "?"

    def pretty(n):
        if n.startswith("f_") and len(n) == 10:
            g = int(n[2:], 16)
            return f"{n[2:].lower()} {guest.get(g, '')}"
        return n

    data = open(a.bin, "rb").read()
    magic, ver, b0, b1, e0, e1 = struct.unpack_from("<6I", data, 0)
    assert magic == 0x31504B4D, "not a mkdd_prof.bin"
    stamp = data[24:56].split(b"\0")[0].decode()
    w = struct.unpack_from(f"<{(len(data) - 56) // 4}I", data, 56)

    kinds = Counter()
    self_g, incl_g, self_d = Counter(), Counter(), Counter()
    self_r, incl_r = Counter(), Counter()
    last_fn = Counter()
    callers = defaultdict(Counter)  # runtime function -> first generated caller
    per_min = defaultdict(Counter)
    i, n_w, tmax = 0, len(w), 0
    while i + 3 <= n_w:
        kind, n = w[i] & 0xFF, (w[i] >> 8) & 0xFF
        t, lf = w[i + 1] / 1000.0, w[i + 2]
        fr = w[i + 3:i + 3 + n]
        i += 3 + n
        tmax = max(tmax, t)
        if not (a.t0 <= t < a.t1):
            continue
        kinds[kind] += 1
        per_min[int(t // 60)][kind] += 1
        if kind == K_GAME and n:
            pc_f = sym(fr[0])
            stack = [pc_f]
            lr_f = sym(fr[1])
            if lr_f != pc_f:  # in a leaf (or a prologue): LR is the caller
                stack.append(lr_f)
            stack += [sym(x) for x in fr[2:]]
            if a.under and not any(a.under in pretty(x) for x in stack):
                kinds[K_GAME] -= 1
                kinds[K_IDLE] += 0
                continue
            self_g[pc_f] += 1
            for s in set(stack) - {"?"}:
                incl_g[s] += 1
            last_fn[lf] += 1
            if not pc_f.startswith("f_"):
                gen = next((s for s in stack if s.startswith("f_")), "-")
                callers[pc_f][gen] += 1
        elif kind == K_GPU and n:
            self_d[sym(fr[0])] += 1
        elif kind == K_RENDER and n:
            st = [sym(fr[0])] + ([sym(fr[1])] if sym(fr[1]) != sym(fr[0]) else []) + [sym(x) for x in fr[2:]]
            self_r[st[0]] += 1
            for s in set(st) - {"?"}:
                incl_r[s] += 1

    busy = kinds[K_GAME]
    if a.under:
        print(f"(only samples under {a.under})")
    tot = busy + kinds[K_IDLE]
    print(f"build {stamp}; {tmax:.0f} s recorded; window {a.t0:g}-{min(a.t1, tmax):g} s")
    print(f"game thread: {busy} busy samples, {kinds[K_IDLE]} idle ({100 * kinds[K_IDLE] / max(tot, 1):.1f}% idle), "
          f"{kinds[K_FAIL]} failed; decoder: {kinds[K_GPU]} samples")
    print("\nper minute: idle% of the game thread")
    print("  " + "  ".join(f"{m}:{100 * c[K_IDLE] / max(c[K_IDLE] + c[K_GAME], 1):.0f}" for m, c in sorted(per_min.items())))

    if not busy:
        return
    gen = sum(c for s, c in self_g.items() if s.startswith("f_"))
    print(f"\nself time split: generated game code {100 * gen / busy:.1f}%, runtime/libraries {100 * (busy - gen) / busy:.1f}%")

    print(f"\n== game thread: self time (top {a.top})")
    for s, c in self_g.most_common(a.top):
        print(f"{100 * c / busy:6.2f}%  {pretty(s)}")

    print(f"\n== game thread: inclusive time, game functions (top {a.top * 2})")
    k = 0
    for s, c in incl_g.most_common():
        if s.startswith("f_"):
            print(f"{100 * c / busy:6.2f}%  {pretty(s)}")
            k += 1
            if k >= a.top * 2:
                break

    print(f"\n== game thread: inclusive time, runtime (top {a.top})")
    k = 0
    for s, c in incl_g.most_common():
        if not s.startswith("f_"):
            print(f"{100 * c / busy:6.2f}%  {s}")
            k += 1
            if k >= a.top:
                break

    print("\n== callers of the busiest runtime functions (nearest game function on the stack)")
    for s, c in [x for x in self_g.most_common() if not x[0].startswith("f_")][:12]:
        cs = ", ".join(f"{pretty(g)} {100 * n / c:.0f}%" for g, n in callers[s].most_common(4))
        print(f"{100 * c / busy:6.2f}%  {s}: {cs}")

    print("\n== game thread: last entered guest function (cross-check, top 20)")
    for g, c in last_fn.most_common(20):
        print(f"{100 * c / busy:6.2f}%  {g:08x} {guest.get(g, '')}")

    d = kinds[K_GPU]
    if d:
        print(f"\n== GX decoder thread: self time (top 25 of {d} samples)")
        for s, c in self_d.most_common(25):
            print(f"{100 * c / d:6.2f}%  {s}")

    r = kinds[K_RENDER]
    if r:
        print(f"\n== render thread: self time (top 30 of {r} samples)")
        for s, c in self_r.most_common(30):
            print(f"{100 * c / r:6.2f}%  {s}")
        print(f"\n== render thread: inclusive (top 30; frame-pointer chains, may stop in Mesa)")
        for s, c in incl_r.most_common(30):
            print(f"{100 * c / r:6.2f}%  {s}")


if __name__ == "__main__":
    main()
