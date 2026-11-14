"""Draws the coin layouts runtime/coins.c writes with LOGGING on (logs/coins_<course ID>_<hash>.txt, in a build with -DCOINS_DUMP)
from above, as SVG: the computer karts' line (grey, its width shaded), the item boxes (blue), the
start (green) and the coins (gold; cyan over water).

    py -3 tools/coins_view.py LOGS_DIR_OR_FILE... [-o OUT_DIR]

One .svg per layout next to it (or in OUT_DIR), plus index.html showing them all."""
import argparse
import html
from pathlib import Path


def read(path):
    d = {"cl": [], "step": [], "box": [], "start": None, "coin": [], "title": path.stem}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "#":
            d["title"] = line[1:].strip()
        elif f[0] == "cl" and len(f) >= 7:
            d["cl"].append((float(f[2]), float(f[3]), float(f[4]), int(f[5]), float(f[6])))
        elif f[0] == "step" and len(f) >= 3:
            d["step"].append((int(f[1]), int(f[2])))
        elif f[0] == "box" and len(f) >= 4:
            d["box"].append((float(f[1]), float(f[3])))
        elif f[0] == "start" and len(f) >= 4:
            d["start"] = (float(f[1]), float(f[3]))
        elif f[0] == "coin" and len(f) >= 5:
            d["coin"].append((float(f[1]), float(f[3]), int(f[4]), int(f[5]) if len(f) > 5 else 0))
    return d


def svg(d, size=900):
    xs = [p[0] for p in d["cl"]] + [b[0] for b in d["box"]] + [c[0] for c in d["coin"]]
    zs = [p[2] for p in d["cl"]] + [b[1] for b in d["box"]] + [c[1] for c in d["coin"]]
    if not xs:
        return None
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    span = max(x1 - x0, z1 - z0, 1.0)
    pad = span * 0.04
    k = size / (span + 2 * pad)

    def at(x, z):
        return (x - x0 + pad) * k, (z - z0 + pad) * k

    w, h = (x1 - x0 + 2 * pad) * k, (z1 - z0 + 2 * pad) * k
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="100%" viewBox="0 0 {w:.0f} {h + 30:.0f}">',
           f'<rect width="100%" height="100%" fill="#14161a"/>',
           f'<text x="8" y="{h + 20:.0f}" fill="#ccc" font-family="sans-serif" font-size="14">{html.escape(d["title"])}: '
           f'{len(d["coin"])} coins, {len(d["box"])} item boxes</text>']
    cl = d["cl"]
    for a, b in d["step"]:
        if a >= len(cl) or b >= len(cl):
            continue
        (ax, az), (bx, bz) = at(cl[a][0], cl[a][2]), at(cl[b][0], cl[b][2])
        width = max((cl[a][4] + cl[b][4]) * k, 1.0)  # the scale is a half width
        out.append(f'<line x1="{ax:.1f}" y1="{az:.1f}" x2="{bx:.1f}" y2="{bz:.1f}" stroke="#3a4048" stroke-width="{width:.1f}" stroke-linecap="round"/>')
    for a, b in d["step"]:
        if a >= len(cl) or b >= len(cl):
            continue
        (ax, az), (bx, bz) = at(cl[a][0], cl[a][2]), at(cl[b][0], cl[b][2])
        out.append(f'<line x1="{ax:.1f}" y1="{az:.1f}" x2="{bx:.1f}" y2="{bz:.1f}" stroke="#8a929c" stroke-width="1"/>')
    for x, z in d["box"]:
        px, pz = at(x, z)
        out.append(f'<rect x="{px - 4:.1f}" y="{pz - 4:.1f}" width="8" height="8" fill="#4a90e2"/>')
    if d["start"]:
        px, pz = at(*d["start"])
        out.append(f'<circle cx="{px:.1f}" cy="{pz:.1f}" r="7" fill="none" stroke="#3ecf6e" stroke-width="3"/>')
    r = max(2.0, min(4.0, 60 * k))
    for x, z, attr, water in d["coin"]:
        px, pz = at(x, z)
        out.append(f'<circle cx="{px:.1f}" cy="{pz:.1f}" r="{r:.1f}" fill="{"#38d6e8" if water else "#f5c518"}"><title>attr {attr}</title></circle>')
    out.append("</svg>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    files = []
    for p in map(Path, a.paths):
        files += sorted(p.glob("coins_*.txt")) if p.is_dir() else [p]
    shown = []
    for f in files:
        s = svg(read(f))
        if not s:
            print(f"{f}: nothing to draw")
            continue
        out = (Path(a.out) if a.out else f.parent) / (f.stem + ".svg")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(s, encoding="utf-8")
        shown.append(out)
        print(out)
    if shown:
        idx = shown[0].parent / "index.html"
        idx.write_text("<!doctype html><meta charset=utf-8><title>Coin layouts</title><body style='background:#0d0e10'>"
                       + "".join(f"<img src='{p.name}' style='margin:6px;max-width:48%'>" for p in shown), encoding="utf-8")
        print(idx)


if __name__ == "__main__":
    main()
