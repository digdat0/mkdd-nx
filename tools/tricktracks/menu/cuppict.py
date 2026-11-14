"""The trick battle's picture beside its name (stage page, course intro): the TRICKS row's ramp + arrow
(icons.py, drawn here) as race2d.arc timg/cup_pict_balloon.bti is: C8 40 x 40, 48 RGB5A3 palette
entries; the header comes from the game's file. The runtime copies its texels and palette over the
balloon's while Trick Style is on (battle.c).
py -3.10 cuppict.py <cup_pict_balloon.bti> <out .bti>"""
import sys, struct
from PIL import Image
import icons

t = open(sys.argv[1], 'rb').read()
fmt, w, h, palfmt, count = t[0], *struct.unpack('>HH', t[2:6]), t[9], struct.unpack('>H', t[10:12])[0]
assert (fmt, w, h, palfmt) == (9, 40, 40, 2), (fmt, w, h, palfmt)
imgoff, paloff = struct.unpack('>I', t[0x1c:0x20])[0], struct.unpack('>I', t[12:16])[0]
im = icons.ramp().resize((40, 40), Image.LANCZOS)
# index 0 transparent; the rest the picture's colours (opaque or not) quantised
alpha = im.split()[3]
rgb = Image.new('RGB', im.size, (0, 0, 0)); rgb.paste(im.convert('RGB'), (0, 0), alpha)
q = rgb.quantize(colors=count - 1, method=Image.Quantize.MEDIANCUT)
pal = q.getpalette()[:3 * (count - 1)]
def rgb5a3(r, g, b, a):
    if a >= 0xF0: return 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
    return (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)
idx = [[0] * 40 for _ in range(40)]
for y in range(40):
    for x in range(40):
        idx[y][x] = 0 if alpha.getpixel((x, y)) < 48 else q.getpixel((x, y)) + 1
entries = [0] + [rgb5a3(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255) for i in range(count - 1)]
entries += [0] * (count - len(entries))
texels = bytearray()
for by in range(0, 40, 4):
    for bx in range(0, 40, 8):
        for y in range(4):
            for x in range(8):
                texels.append(idx[by + y][bx + x])
out = bytearray(t)
out[imgoff:imgoff + len(texels)] = texels
out[paloff:paloff + 2 * count] = b''.join(struct.pack('>H', e) for e in entries)
open(sys.argv[2], 'wb').write(out)
prev = Image.new('RGBA', (40, 40))
for y in range(40):
    for x in range(40):
        i = idx[y][x]
        prev.putpixel((x, y), (0, 0, 0, 0) if not i else (pal[3 * (i - 1)], pal[3 * (i - 1) + 1], pal[3 * (i - 1) + 2], 255))
bg = Image.new('RGBA', (40, 40), (0, 0, 120, 255)); bg.alpha_composite(prev)
bg.resize((160, 160), Image.NEAREST).save(sys.argv[2].rsplit('.', 1)[0] + '_preview.png')
print('cup picture', sys.argv[2], len(out), 'bytes')
