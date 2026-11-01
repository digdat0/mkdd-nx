#!/usr/bin/env python3
"""A page for trying KART FPV eyes on the PC: the stock karts' bodies from your own disc, drawn over a
screenshot, with sliders for the eye. Holds game models and textures, so it stays in debug/ (ignored by
git) and is never published.
  py -3 tools/fpv_mock.py <disc.iso> <background.png> <out.html>"""
import base64, io, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import charpack, wiichar
from charpack import yaz0
from PIL import Image

FOLDERS = ["mario", "dk", "yoshi", "noko", "peach", "baby", "wario", "koopa", "luigi", "diddy", "cathy", "pata", "daisy",
           "bblui", "walui", "jr", "kpio", "kpico", "teres", "pakku", "extra"]
# each body's place from the kart's centre (up, forward), by EKartID: fitted to screenshots in the game
BODY = [(0, 15), (3, 30), (3, 12), (27, 21), (96, 15), (25, 13), (45, 15), (21, 9), (0, 15), (75, 21), (3, 9), (27, 21), (96, 15),
        (25, 13), (69, 9), (24, 6), (102, 8), (102, 8), (96, 21), (96, 21), (60, -9)]


def png(im, size=None, fmt="PNG"):
    if size and max(im.size) > size:
        im = im.resize((min(im.width, size), min(im.height, size)))
    o = io.BytesIO()
    im.save(o, fmt)
    return "data:image/%s;base64,%s" % (fmt.lower(), base64.b64encode(o.getvalue()).decode())


def kart(bmd):
    m = wiichar.Mesh(bmd)
    w, pos, uvs = m.matrices(), m.arr[wiichar.POS], m.arr.get(wiichar.TEX0)
    dms, groups = {}, {}
    for tex, tri in m.tris:
        g = groups.setdefault(tex, ([], []))
        for v in tri:
            dm = dms.setdefault(v[0], m.draw_matrix(w, v[0]))
            p = pos[v[1]]
            g[0].extend(round(dm[r][0] * p[0] + dm[r][1] * p[1] + dm[r][2] * p[2] + dm[r][3], 1) for r in range(3))
            uv = uvs[v[3]] if uvs and v[3] is not None else (0, 0)
            g[1].extend((round(uv[0], 4), round(uv[1], 4)))
    images = {}
    for tex in groups:
        if tex >= 0:
            try:
                rows = m.image(tex)
            except wiichar.ConvertError:  # drawn plain grey
                continue
            im = Image.new("RGBA", (len(rows[0]), len(rows)))
            im.putdata([p for r in rows for p in r])
            images[tex] = png(im, 128)
    return [{"img": images.get(t), "pos": g[0], "uv": g[1]} for t, g in groups.items()]


def main():
    iso, bg, out = sys.argv[1:4]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    table = re.findall(r"\{(-?\d+), (-?\d+), (-?\d+)\}, /\* \d+ (.+?) \*/", open(os.path.join(root, "runtime", "kart_eye.h")).read())
    arc = charpack.rarc(yaz0(charpack.Disc(iso).file("mram.arc")))
    karts = []
    for (up, fwd, tilt, name), f, (by, bz) in zip(table, FOLDERS, BODY):
        b = arc[(f + "_all", f + "_k_body.bmd")]
        karts.append({"name": name, "up": int(up), "fwd": int(fwd), "tilt": int(tilt), "by": by, "bz": bz, "groups": kart(yaz0(b) if b[:4] == b"Yaz0" else b)})
    with open(os.path.join(root, "tools", "fpv_mock.html"), encoding="utf-8") as f:
        page = f.read()
    page = page.replace("/*KARTS*/null", json.dumps(karts, separators=(",", ":"))).replace("/*BG*/null", json.dumps(png(Image.open(bg).convert("RGB"), fmt="JPEG")))
    with open(out, "w", encoding="utf-8") as f:
        f.write(page)
    print("%s: %d MB" % (out, os.path.getsize(out) >> 20))


if __name__ == "__main__":
    main()
