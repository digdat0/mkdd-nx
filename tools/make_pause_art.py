"""Pause-screen art (switch/pause_art_data.S): a Pro Controller (tools/pro_controller.png, cut out) in
place of the game's GameCube controller picture, and one badge per Switch button for the button icons.

The controller picture replaces a 176x128 texture (drawn at 2x) on the pause and player-count
screens. Output, top row first:
  switch/pause_console.rgba  352x256 RGBA
  switch/pause_badges.rgba   BADGE_COLS x BADGE_ROWS cells of 64x64 RGBA (order: BADGES)
usage: py -3 tools/make_pause_art.py [preview.png]"""
import os, sys
from collections import deque
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'switch')
FONT = 'C:/Windows/Fonts/arialbd.ttf'

# keep in step with switch/pause_art.c
BADGES = ['A', 'B', 'X', 'Y', 'L', 'R', 'ZL', 'ZR', '+', 'LS', 'RS', 'UP', 'DOWN', 'LEFT', 'RIGHT', 'SL', 'SR', 'STICK', 'DPAD']
BADGE_COLS, CELL = 6, 64

INK = (16, 16, 24, 255)
KEY, KEY_RIM = (40, 40, 46, 255), (205, 205, 212, 255)
DETAIL = 1.5  # contrast around the face colour (85): grips, sticks and buttons darker, labels lighter
GLOW = (255, 255, 255, 6, 0.8)  # soft halo so the dark controller stands out on navy: RGB, blur radius, strength


def pro_controller():
    """tools/pro_controller.png (a flat Pro Controller on light paper) cut out: the light background
    and the controller's soft shadow, reached from the image border, become transparent; edge pixels
    get partial alpha. Fitted to 352x256, centred."""
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
    r, g, b, a = out.split()
    crisp = lambda v: max(0, min(255, int(85 + (v - 85) * DETAIL)))
    out = Image.merge('RGBA', (r.point(crisp), g.point(crisp), b.point(crisp), a))
    P = 4  # room for the glow
    sc = min((352 - 2 * P) / out.width, (256 - 2 * P) / out.height)
    out = out.resize((round(out.width * sc), round(out.height * sc)), Image.LANCZOS)
    im = Image.new('RGBA', (352, 256), (0, 0, 0, 0))
    pos = ((352 - out.width) // 2, (256 - out.height) // 2)
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
