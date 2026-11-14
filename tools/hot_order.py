"""Linker section order from profiles (switch/prof_switch.c), so the code that runs sits together.

usage: py -3 tools/hot_order.py out.ld MKDD-NX.elf mkdd_prof.bin[:from:to][@other.elf] ...

@other.elf: the build that recorded that profile, when it is not MKDD-NX.elf.

The game's text is about 50 MB with the running code scattered through it. Each thread's functions go
in order of how often they were sampled, the game thread's first; on the stacks count too, so a
function that is only ever passing through stays near its callees. Layout only: no code changes.
"""
import bisect, os, re, struct, subprocess, sys
from collections import Counter

_DKP = os.environ.get("DEVKITPRO", "")
NM = os.path.join(_DKP if os.path.isdir(_DKP) else "C:/devkitPro", "devkitA64/bin/aarch64-none-elf-nm")
K_GAME, K_GPU, K_RENDER = 1, 3, 5


def main():
    out, elf, profiles = sys.argv[1], sys.argv[2], sys.argv[3:]
    syms = {}

    def load(path):  # sorted text symbols of one elf (each profile must be read against its own build)
        if path not in syms:
            addrs, names = [], []
            for line in subprocess.run([NM, "-n", "--defined-only", path], capture_output=True, text=True, check=True).stdout.splitlines():
                p = line.split()
                if len(p) == 3 and p[1] in "tTwW" and not (addrs and addrs[-1] == int(p[0], 16)):
                    addrs.append(int(p[0], 16))
                    names.append(p[2])
            syms[path] = (addrs, names)
        return syms[path]
    counts = {K_GAME: Counter(), K_GPU: Counter(), K_RENDER: Counter()}
    for spec in profiles:
        spec, _, pelf = spec.partition("@")
        addrs, names = load(pelf or elf)
        m = re.fullmatch(r"(.+):([\d.]+):([\d.]+)", spec)
        path, t0, t1 = (m[1], float(m[2]), float(m[3])) if m else (spec, 0, 1e12)
        data = open(path, "rb").read()
        w = struct.unpack_from(f"<{(len(data) - 56) // 4}I", data, 56)
        i = 0
        while i + 3 <= len(w):
            kind, n = w[i] & 0xFF, (w[i] >> 8) & 0xFF
            t, fr = w[i + 1] / 1000.0, w[i + 3:i + 3 + n]
            i += 3 + n
            if kind in counts and t0 <= t < t1:
                for k, off in enumerate(fr):
                    if off:
                        counts[kind][names[bisect.bisect_right(addrs, off) - 1]] += 4 if k == 0 else 1
    seen, order = set(), []
    for kind in (K_GAME, K_GPU, K_RENDER):
        for name, _ in counts[kind].most_common():
            if name not in seen:
                seen.add(name)
                order.append(name)
    with open(out, "w", newline="\n") as f:
        f.write("/* tools/hot_order.py: the sampled functions first, busiest first */\n.text : {\n  KEEP(*(.crt0))\n") # the entry point stays at the image start
        for name in order:
            f.write(f"  *(.text.{name})\n")
        f.write("}\n")
    print(f"{len(order)} functions")


if __name__ == "__main__":
    main()
