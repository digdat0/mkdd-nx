"""Pause-screen art (switch/pause_art_data.S): a Pro Controller (tools/pro_controller.png, cut out) in
place of the game's GameCube controller picture (console(): the earlier Switch drawing), and one badge per Switch button for the button icons.

The console picture replaces a 176x128 texture whose layout lines end at fixed points (the
GameCube buttons); the sticks, shoulders and face buttons are placed near those points.
Drawn at 4x and downsampled. Output, top row first:
  switch/pause_console.rgba  352x256 RGBA
  switch/pause_badges.rgba   BADGE_COLS x BADGE_ROWS cells of 64x64 RGBA (order: BADGES)
usage: py -3 tools/make_pause_art.py [preview.png]"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'switch')
FONT = 'C:/Windows/Fonts/arialbd.ttf'

# keep in step with switch/pause_art.c
BADGES = ['A', 'B', 'X', 'Y', 'L', 'R', 'ZL', 'ZR', '+', 'LS', 'RS', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'SL', 'SR', 'STICK', 'DPAD']
BADGE_COLS, CELL = 6, 64

INK = (16, 16, 24, 255)
BLUE, BLUE_D = (10, 185, 230, 255), (6, 120, 160, 255)
RED, RED_D = (255, 60, 40, 255), (180, 36, 24, 255)
BODY, SCREEN = (58, 58, 64, 255), (14, 16, 22, 255)
KEY, KEY_RIM = (40, 40, 46, 255), (205, 205, 212, 255)
STICK, STICK_TOP = (60, 60, 66, 255), (112, 112, 120, 255)


def console():
    S = 8  # 176x128 units -> 1408x1024, downsampled to 352x256
    W, H = 176 * S, 128 * S
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    o = 2.2 * S  # outline width

    def rr(box, r, fill, outline=INK, corners=None):
        b = [v * S for v in box]
        d.rounded_rectangle(b, radius=r * S, fill=fill, outline=outline, width=int(o), corners=corners)

    def circ(cx, cy, r, fill, outline=INK, w=o):
        d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=fill, outline=outline, width=int(w))

    top, bot = 18, 104
    # shoulder buttons behind the Joy-Cons
    rr((8, 10, 44, 26), 7, (200, 200, 208, 255))
    rr((132, 10, 168, 26), 7, (200, 200, 208, 255))
    # Joy-Cons and the console between them
    rr((4, top, 58, bot), 22, BLUE, corners=(True, False, False, True))
    rr((118, top, 172, bot), 22, RED, corners=(False, True, True, False))
    rr((56, top, 120, bot), 3, BODY)
    rr((61, top + 6, 115, bot - 6), 2, SCREEN, outline=(30, 30, 36, 255))
    # screen glare
    d.polygon([(64 * S, (top + 9) * S), (84 * S, (top + 9) * S), (70 * S, (bot - 9) * S), (64 * S, (bot - 9) * S)], fill=(40, 44, 56, 255))
    # rail shading
    d.rectangle([52 * S, (top + 3) * S, 56 * S, (bot - 3) * S], fill=BLUE_D)
    d.rectangle([120 * S, (top + 3) * S, 124 * S, (bot - 3) * S], fill=RED_D)

    # left Joy-Con: minus, stick, d-pad, capture
    d.rectangle([44 * S, 25 * S, 50 * S, 27 * S], fill=INK)
    circ(33, 50, 10, STICK)
    circ(33, 50, 6.5, STICK_TOP, w=o * 0.6)
    for dx, dy in ((0, -7), (0, 7), (-7, 0), (7, 0)):
        circ(25 + dx, 79 + dy, 3.6, KEY, outline=INK, w=o * 0.5)
    rr((36, 92, 42, 98), 1, KEY)

    # right Joy-Con: plus, face buttons, stick, home
    d.rectangle([125 * S, 25 * S, 131 * S, 27 * S], fill=INK)
    d.rectangle([127 * S, 23 * S, 129 * S, 29 * S], fill=INK)
    for dx, dy in ((0, -9), (0, 9), (-9, 0), (9, 0)):
        circ(149 + dx, 46 + dy, 5, KEY, outline=INK, w=o * 0.5)
    circ(137, 76, 9, STICK)
    circ(137, 76, 5.8, STICK_TOP, w=o * 0.6)
    circ(156, 91, 3.6, KEY, outline=(150, 150, 156, 255), w=o * 0.5)
    return im.resize((352, 256), Image.LANCZOS)


LIGHTEN = 1.0  # 1 = as drawn; smaller = lighter
DETAIL = 1.5  # contrast around the face colour (85): grips, sticks and buttons darker, labels lighter
GLOW = (255, 255, 255, 6, 0.8)  # soft halo so the dark controller stands out on navy: RGB, blur radius, strength


def pro_controller():
    """tools/pro_controller.png (a flat Pro Controller on light paper) cut out: the light background
    and the controller's soft shadow, reached from the image border, become transparent; edge pixels
    get partial alpha. Fitted to 352x256, centred."""
    from collections import deque
    src = Image.open(os.path.join(HERE, 'pro_controller.png')).convert('RGB')
    W, H = src.size
    px = src.load()
    lum = [[sum(px[x, y]) / 3 for x in range(W)] for y in range(H)]
    LO, HI = 95, 140  # below LO: controller; flood-filled pixels above LO: background
    bg = [[False] * W for _ in range(H)]
    q = deque()
    for x in range(W):
        q.extend(((x, 0), (x, H - 1)))
    for y in range(H):
        q.extend(((0, y), (W - 1, y)))
    while q:
        x, y = q.popleft()
        if bg[y][x] or lum[y][x] < LO:
            continue
        bg[y][x] = True
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < W and 0 <= ny < H and not bg[ny][nx]:
                q.append((nx, ny))
    out = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    o = out.load()
    EDGE = (54, 57, 62)  # the grips' and top edge's colour, for partly covered pixels
    for y in range(H):
        for x in range(W):
            if not bg[y][x]:
                o[x, y] = px[x, y] + (255,)
            elif lum[y][x] < HI:
                o[x, y] = EDGE + (int(255 * (HI - lum[y][x]) / (HI - LO)),)
    out = out.crop(out.getbbox())
    # lighter greys: the dark original disappears against the pause screen's navy panels
    r, g, b, a = out.split()
    lift = lambda v: 255 - int((255 - v) * LIGHTEN)
    out = Image.merge('RGBA', (r.point(lift), g.point(lift), b.point(lift), a))
    r, g, b, a = out.split()
    crisp = lambda v: max(0, min(255, int(85 + (v - 85) * DETAIL)))
    out = Image.merge('RGBA', (r.point(crisp), g.point(crisp), b.point(crisp), a))
    P = 4  # room for the glow
    sc = min((352 - 2 * P) / out.width, (256 - 2 * P) / out.height)
    out = out.resize((round(out.width * sc), round(out.height * sc)), Image.LANCZOS)
    im = Image.new('RGBA', (352, 256), (0, 0, 0, 0))
    pos = ((352 - out.width) // 2, (256 - out.height) // 2)
    from PIL import ImageFilter
    sil = Image.new('L', (352, 256), 0)
    sil.paste(out.split()[3], pos)
    if GLOW:
        halo = sil.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(GLOW[3])).point(lambda v: int(v * GLOW[4]))
        im.paste(Image.new('RGBA', (352, 256), GLOW[:3] + (255,)), (0, 0), halo)
    im.alpha_composite(out, pos)
    return im


def badge(name):
    S = 4
    N = CELL * S
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = N / 2
    ow = int(3.5 * S)

    def text(s, size, fill, cy=c):
        f = ImageFont.truetype(FONT, int(size * S))
        d.text((c, cy), s, font=f, fill=fill, anchor='mm')

    def stick(letter):  # same dark key and white rim as the face buttons, with a ringed cap
        d.ellipse([c - 27 * S, c - 27 * S, c + 27 * S, c + 27 * S], fill=KEY, outline=KEY_RIM, width=ow)
        d.ellipse([c - 16 * S, c - 16 * S, c + 16 * S, c + 16 * S], fill=(64, 64, 72, 255), outline=KEY_RIM, width=int(5 * S))
        if letter: text(letter, 22, (255, 255, 255, 255), c + 1 * S)

    if name in ('A', 'B', 'X', 'Y', '+'):
        d.ellipse([c - 27 * S, c - 27 * S, c + 27 * S, c + 27 * S], fill=KEY, outline=KEY_RIM, width=ow)
        if name == '+':
            d.rectangle([c - 14 * S, c - 4 * S, c + 14 * S, c + 4 * S], fill=(255, 255, 255, 255))
            d.rectangle([c - 4 * S, c - 14 * S, c + 4 * S, c + 14 * S], fill=(255, 255, 255, 255))
        else:
            text(name, 34, (255, 255, 255, 255), c + 1 * S)
    elif name in ('L', 'R', 'ZL', 'ZR', 'SL', 'SR'):
        d.rounded_rectangle([c - 29 * S, c - 19 * S, c + 29 * S, c + 19 * S], radius=12 * S,
                            fill=(226, 226, 232, 255), outline=INK, width=ow)
        text(name, 28 if len(name) == 2 else 32, INK, c + 1 * S)
    elif name in ('LS', 'RS'):
        stick(name[0])
    elif name == 'STICK':
        stick(None)
    elif name == 'DPAD':  # the + badge with D-pad arms
        d.ellipse([c - 27 * S, c - 27 * S, c + 27 * S, c + 27 * S], fill=KEY, outline=KEY_RIM, width=ow)
        a, l = 6.5 * S, 18 * S
        d.rounded_rectangle([c - l, c - a, c + l, c + a], radius=2 * S, fill=(255, 255, 255, 255))
        d.rounded_rectangle([c - a, c - l, c + a, c + l], radius=2 * S, fill=(255, 255, 255, 255))
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):  # arrows on the arms
            tip, base, w = 15 * S, 9 * S, 3.5 * S
            px, py = -dy, dx
            d.polygon([(c + dx * tip, c + dy * tip), (c + dx * base + px * w, c + dy * base + py * w),
                       (c + dx * base - px * w, c + dy * base - py * w)], fill=KEY)
    else:  # d-pad direction
        d.ellipse([c - 27 * S, c - 27 * S, c + 27 * S, c + 27 * S], fill=KEY, outline=KEY_RIM, width=ow)
        tri = {'UP': [(0, -14), (-13, 9), (13, 9)], 'DOWN': [(0, 14), (-13, -9), (13, -9)],
               'LEFT': [(-14, 0), (9, -13), (9, 13)], 'RIGHT': [(14, 0), (-9, -13), (-9, 13)]}[name]
        d.polygon([(c + x * S, c + y * S) for x, y in tri], fill=(255, 255, 255, 255))
    return im.resize((CELL, CELL), Image.LANCZOS)


def main():
    con = pro_controller()
    rows = (len(BADGES) + BADGE_COLS - 1) // BADGE_COLS
    atlas = Image.new('RGBA', (BADGE_COLS * CELL, rows * CELL), (0, 0, 0, 0))
    for i, n in enumerate(BADGES):
        atlas.paste(badge(n), ((i % BADGE_COLS) * CELL, (i // BADGE_COLS) * CELL))
    open(os.path.join(OUT, 'pause_console.rgba'), 'wb').write(con.tobytes())
    open(os.path.join(OUT, 'pause_badges.rgba'), 'wb').write(atlas.tobytes())
    print('console', con.size, 'badges', atlas.size, 'rows', rows)
    if len(sys.argv) > 1:
        bg = Image.new('RGBA', (max(con.width, atlas.width), con.height + atlas.height + 8), (12, 14, 48, 255))
        bg.alpha_composite(con, (0, 0))
        bg.alpha_composite(atlas, (0, con.height + 8))
        bg.save(sys.argv[1])


if __name__ == '__main__':
    main()
