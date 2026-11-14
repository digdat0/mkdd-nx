"""Art for the sixteen cup tracks (2026-11-12). For each track of tools/cups8.txt (stem, track):
  CourseName/<stem>_name.bti  the course intro's name banner (RGB5A3 208 x 104, as art7's)
  Art/ui_<stem>.rgba          the track picker's picture (menu.c: u16 width, u16 height little-endian, RGBA8 rows)
and Art/ui_cupN.rgba, the four cups' icons (tools/cupicons.py). The thumbnails come from render7's view of each track
(work/a7_<track>/thumb_big.png, rendered here from the track's cam when missing or with -r).
usage: py -3.10 tools/tricktracks/art8.py <outdir> [-r] [--work DIR] [--disc GM4E01 image]"""
import sys, os, struct, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import btix, gamefiles
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import numpy as np

W = gamefiles.flag(sys.argv, '--work', gamefiles.WORK)
_disc = gamefiles.flag(sys.argv, '--disc')
if _disc: gamefiles.fetch(_disc, W)
gamefiles.need(W, 'ms/Mini7_name.bti')
OUT = sys.argv[1]
RERENDER = '-r' in sys.argv
LIST = [l.split() for l in open(os.path.join(HERE, 'cups8.txt')) if l.strip()]
# banner colours (top, bottom) per track
COL = {
    'stadium': ((255, 210, 60), (240, 110, 20)), 'meadow': ((200, 255, 120), (60, 160, 50)), 'coral': ((140, 250, 240), (20, 150, 190)),
    'flowline': ((170, 240, 120), (50, 150, 60)), 'dustdevil': ((255, 200, 120), (210, 90, 40)), 'redrock': ((255, 170, 120), (190, 50, 30)),
    'sunburst': ((255, 230, 110), (240, 120, 20)), 'volcano': ((255, 160, 60), (200, 30, 20)), 'harbor': ((255, 170, 120), (200, 60, 90)),
    'rhythmridge': ((120, 200, 255), (40, 90, 220)), 'steelworks': ((240, 220, 120), (170, 120, 40)), 'neon': ((255, 120, 240), (90, 40, 230)),
    'frostbite': ((235, 250, 255), (80, 160, 235)), 'glacier': ((200, 240, 255), (40, 120, 230)), 'alpine': ((220, 255, 200), (60, 140, 90)),
    'megaramp': ((255, 120, 120), (200, 20, 50)),
}


# (from art7.py, the six-track stage page's art, retired)
def font(sz):
    for f in ('impact.ttf', 'arialbd.ttf'):
        try: return ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:/Windows'), 'Fonts', f), sz)
        except OSError: pass
    return ImageFont.load_default()
def fit_font(text, w, h, start):
    sz = start
    while sz > 8:
        f = font(sz); bb = ImageDraw.Draw(Image.new('L', (1, 1))).textbbox((0, 0), text, font=f)
        if bb[2] - bb[0] <= w and bb[3] - bb[1] <= h: return f, bb
        sz -= 1
    return font(8), (0, 0, 8, 8)
def outlined(size, text, f, bb, xy, fill, outline, ow):
    """RGBA layer: text with an outline (dilated alpha)"""
    m = Image.new('L', size, 0); ImageDraw.Draw(m).text((xy[0] - bb[0], xy[1] - bb[1]), text, font=f, fill=255)
    o = m.filter(ImageFilter.MaxFilter(2 * ow + 1))
    lay = Image.new('RGBA', size, outline + (0,)); lay.putalpha(o)
    top = Image.new('RGBA', size, (0, 0, 0, 0))
    if isinstance(fill, Image.Image): top.paste(fill, (0, 0), m)
    else: top.paste(Image.new('RGBA', size, fill + (255,)), (0, 0), m)
    lay.alpha_composite(top)
    return lay


def lines_of(title):
    w = title.split()
    if len(w) == 1: return (title,)
    best = min(range(1, len(w)), key=lambda i: abs(len(' '.join(w[:i])) - len(' '.join(w[i:]))))
    return (' '.join(w[:best]), ' '.join(w[best:]))


def rgba_file(path, im):
    im = im.convert('RGBA')
    with open(path, 'wb') as f: f.write(struct.pack('<HH', *im.size) + im.tobytes())


def banner(big, lines, c0, c1, tm_ban):
    ban = Image.new('RGBA', (208, 104), (0, 0, 0, 0))
    em = big.resize((300, 212), Image.LANCZOS).crop((90, 40, 210, 160)).resize((84, 84), Image.LANCZOS).convert('RGBA')
    mk = Image.new('L', (84, 84), 0); ImageDraw.Draw(mk).ellipse((0, 0, 83, 83), fill=255); em.putalpha(mk)
    ring = Image.new('RGBA', (92, 92), (0, 0, 0, 0)); ImageDraw.Draw(ring).ellipse((0, 0, 91, 91), fill=(255, 255, 255, 255))
    ImageDraw.Draw(ring).ellipse((2, 2, 89, 89), fill=c1 + (255,))
    ring.alpha_composite(em, (4, 4)); ban.alpha_composite(ring, (114, 6))
    for li, txt in enumerate(lines):
        f, bb = fit_font(txt, 150, 40, 44)
        tht = bb[3] - bb[1]
        g = np.zeros((104, 208, 4), np.uint8); y0 = 8 + li * 48 if len(lines) > 1 else 30
        ys = np.clip((np.arange(104) - y0) / max(1, tht), 0, 1)[:, None]
        for k in range(3): g[..., k] = (c0[k] + (c1[k] - c0[k]) * ys).astype(np.uint8)
        g[..., 3] = 255
        ban.alpha_composite(outlined((208, 104), txt, f, bb, (6 + li * 10, y0), Image.fromarray(g, 'RGBA'), (20, 20, 30), 3))
    return btix.encode(ban, tm_ban)


tm_ban = open(W + '/ms/Mini7_name.bti', 'rb').read()
os.makedirs(OUT + '/Art', exist_ok=True); os.makedirs(OUT + '/CourseName', exist_ok=True)
import tracks7
sheet = Image.new('RGB', (4 * 300, 4 * 260 + 180), (20, 24, 60))
for i, (stem, trk) in enumerate(LIST):
    T = tracks7.TRACKS[trk]; wd = W + '/a7_' + trk
    if RERENDER or not os.path.exists(wd + '/thumb_big.png'):
        cam = [str(v) for v in T['cam']]
        subprocess.run([sys.executable, os.path.join(HERE, 'render7.py'), wd, wd + '/thumb_big.png'] + cam, check=True)
    big = Image.open(wd + '/thumb_big.png').convert('RGB')
    c0, c1 = COL[trk]
    open(OUT + '/CourseName/%s_name.bti' % stem, 'wb').write(banner(big, lines_of(T['title']), c0, c1, tm_ban))
    th = big.resize((288, 204), Image.LANCZOS)
    rgba_file(OUT + '/Art/ui_%s.rgba' % stem, th)
    x, y = (i % 4) * 300, (i // 4) * 260
    sheet.paste(th, (x + 6, y + 6))
    bd = btix.decode(open(OUT + '/CourseName/%s_name.bti' % stem, 'rb').read()); bg = Image.new('RGBA', (208, 104), (0, 0, 100, 255)); bg.alpha_composite(bd)
    sheet.paste(bg.convert('RGB').resize((104, 52)), (x + 6, y + 206))
    ImageDraw.Draw(sheet).text((x + 116, y + 220), stem + ' ' + T['title'], fill=(255, 255, 255))
import cupicons
for n, mk in enumerate(cupicons.ICONS):  # acorn, tornado, green pipe, snowy shell
    ic = mk(160)
    rgba_file(OUT + '/Art/ui_cup%d.rgba' % (n + 1), ic)
    bg = Image.new('RGBA', ic.size, (20, 24, 60, 255)); bg.alpha_composite(ic)
    sheet.paste(bg.convert('RGB'), (n * 300 + 60, 4 * 260 + 10))
sheet.save(OUT + '/art8_sheet.png')
print('art written to', OUT)
