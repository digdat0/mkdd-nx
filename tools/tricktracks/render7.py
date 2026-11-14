"""Software renderer for the stage-page thumbnails: reads a course.dae written by build6/build7 (own simple format)
plus its textures, renders a perspective view with a z-buffer (near-plane clipping, perspective-correct UVs, vertex
colours, distance haze) over the sky texture.
usage: py -3.10 tools/render7.py <dir with course.dae> <out.png> ex ey ez tx ty tz [fov] [w h]"""
import sys, re, os, math
import numpy as np
from PIL import Image

def load_dae(path):
    s = open(path, encoding='utf-8').read()
    imgs = dict(re.findall(r'<image id="(m\d+[^"]*)-diffuse-image"><init_from>([^<]+)</init_from>', s))
    out = []
    for g in re.finditer(r'<geometry id="meshId(\d+)".*?</geometry>', s, re.S):
        i = int(g.group(1)); body = g.group(0)
        arr = {}
        for nm in ('positions', 'tex0', 'color0'):
            m = re.search(r'id="meshId%d-%s-array" count="\d+">([^<]*)<' % (i, nm), body)
            arr[nm] = np.array([float(x) for x in m.group(1).split()], float)
        p = np.array([int(x) for x in re.search(r'<p>([^<]*)</p>', body).group(1).split()], int).reshape(-1, 3)
        key = [k for k in imgs if re.match(r'm%d[a-z]' % i, k)]
        tex = imgs[key[0]].replace('%3a', ':') if key else None
        out.append(dict(pos=arr['positions'].reshape(-1, 3), uv=arr['tex0'].reshape(-1, 2), col=arr['color0'].reshape(-1, 3), tri=p, tex=tex))
    return out

def render(meshes, eye, tgt, sky_png, fov=55.0, W=576, H=408, haze=(0.78, 0.82, 0.88), hz_d=150000.0):
    eye = np.array(eye, float); tgt = np.array(tgt, float)
    f = tgt - eye; f /= np.linalg.norm(f)
    r = np.cross(f, [0, 1, 0]); r /= np.linalg.norm(r); u = np.cross(r, f)
    fl = (H / 2) / math.tan(math.radians(fov) / 2)
    img = np.zeros((H, W, 3)); zb = np.full((H, W), np.inf)
    # sky: the dome texture by view direction (v by elevation)
    if sky_png and os.path.exists(sky_png):
        st = np.asarray(Image.open(sky_png).convert('RGB'), float) / 255
        ys, xs = np.mgrid[0:H, 0:W]
        dirs = (f[None, None, :] * fl + r[None, None, :] * (xs - W / 2)[..., None] - u[None, None, :] * (ys - H / 2)[..., None])
        dirs /= np.linalg.norm(dirs, axis=2, keepdims=True)
        el = np.clip(dirs[..., 1] * 2.2 + 0.05, 0, 1); az = (np.arctan2(dirs[..., 2], dirs[..., 0]) / (2 * math.pi) * 3) % 1
        img = st[((1 - el) * (st.shape[0] - 1)).astype(int), (az * (st.shape[1] - 1)).astype(int)]
        haze = tuple(st[-1, :, :].mean(0))
    hz = np.array(haze)
    for m in meshes:
        tex = np.asarray(Image.open(m['tex']).convert('RGB'), float) / 255 if m['tex'] and os.path.exists(m['tex']) else np.ones((2, 2, 3))
        th, tw = tex.shape[:2]
        P = m['pos'] - eye
        cam = np.stack([P @ r, P @ u, P @ f], 1)          # x right, y up, z forward
        for t in m['tri']:
            v = cam[t]
            if (v[:, 2] < 10).all(): continue
            poly = [(v[k], m['uv'][t[k]], m['col'][t[k]]) for k in range(3)]
            if (v[:, 2] < 10).any():                    # clip to z >= 10
                out = []
                for k in range(len(poly)):
                    a, b = poly[k], poly[(k + 1) % len(poly)]
                    ina, inb = a[0][2] >= 10, b[0][2] >= 10
                    if ina: out.append(a)
                    if ina != inb:
                        s = (10 - a[0][2]) / (b[0][2] - a[0][2])
                        out.append(tuple(a[j] + (b[j] - a[j]) * s for j in range(3)))
                poly = out
                if len(poly) < 3: continue
            for k in range(1, len(poly) - 1):
                tri = (poly[0], poly[k], poly[k + 1])
                z = np.array([q[0][2] for q in tri])
                sx = np.array([W / 2 + q[0][0] / q[0][2] * fl for q in tri]); sy = np.array([H / 2 - q[0][1] / q[0][2] * fl for q in tri])
                x0, x1 = max(0, int(sx.min())), min(W - 1, int(math.ceil(sx.max()))); y0, y1 = max(0, int(sy.min())), min(H - 1, int(math.ceil(sy.max())))
                if x0 > x1 or y0 > y1: continue
                den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
                if abs(den) < 1e-9: continue
                ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]; px, py = xs + 0.5, ys + 0.5
                l0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / den
                l1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / den
                l2 = 1 - l0 - l1
                inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
                if not inside.any(): continue
                iw = l0 / z[0] + l1 / z[1] + l2 / z[2]; zz = 1 / iw
                sub = zb[y0:y1 + 1, x0:x1 + 1]
                ok = inside & (zz < sub)
                if not ok.any(): continue
                uvs = np.array([q[1] for q in tri]); cols = np.array([q[2] for q in tri])
                uu = (l0 * uvs[0, 0] / z[0] + l1 * uvs[1, 0] / z[1] + l2 * uvs[2, 0] / z[2]) * zz
                vv = (l0 * uvs[0, 1] / z[0] + l1 * uvs[1, 1] / z[1] + l2 * uvs[2, 1] / z[2]) * zz
                c = (l0[..., None] * cols[0] / z[0] + l1[..., None] * cols[1] / z[1] + l2[..., None] * cols[2] / z[2]) * zz[..., None]
                ti = ((1 - vv % 1) * (th - 1)).astype(int); tj = ((uu % 1) * (tw - 1)).astype(int)
                col = tex[ti, tj] * c
                k_ = np.clip(zz / hz_d, 0, 1)[..., None] ** 1.3 * 0.75
                col = col * (1 - k_) + hz * k_
                sub[ok] = zz[ok]
                img[y0:y1 + 1, x0:x1 + 1][ok] = col[ok]
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8), 'RGB')

if __name__ == '__main__':
    d, out = sys.argv[1], sys.argv[2]
    ev = [float(x) for x in sys.argv[3:9]]
    fov = float(sys.argv[9]) if len(sys.argv) > 9 else 55.0
    W_, H_ = (int(sys.argv[10]), int(sys.argv[11])) if len(sys.argv) > 11 else (576, 408)
    ms = load_dae(os.path.join(d, 'course.dae'))
    sky = [os.path.join(d, n) for n in ('t7_sky.png', 't6_sky.png') if os.path.exists(os.path.join(d, n))]
    im = render(ms, ev[:3], ev[3:], sky[0] if sky else None, fov, W_, H_)
    im.save(out); print('saved', out, im.size)
