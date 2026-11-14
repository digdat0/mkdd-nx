"""The mode page with a fourth row (TRICKS): <out>/SceneData/English/menu.arc from the disc's menu.arc.

selectmode.blo: the Battle row's panes cloned as row 4 (M4crsr6, M4bar6, MNtrP21 + children, MtrickN +
children), the clone title on a new material (Entry_Trick.bti), the clone bar on a new material that
no colour animation names (the runtime lights it by giving it the lit bar's material).
selectmode.bck: bones appended (SOLO_BONES: the three rows' panes moved up for four rows, which the
runtime gives those panes on the 1-player page only; TRICK_BONES: the clone row's).
Bars have no transform animation: the runtime moves them.
"""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # tools/tricktracks (lib)
from lib.rarc import Archive, File
import blo

DY_GP, DY_VS, DY_BT, DY_TR = -20.0, -46.0, -72.0, 12.0   # DY_TR: row 4 from the Battle row's own place
# appended bones, in order (index 134 + k): (source bone, y change)
SOLO_BONES = [(71, DY_GP), (68, DY_GP), (74, DY_GP), (98, DY_GP), (116, DY_GP),
              (72, DY_VS), (69, DY_VS), (83, DY_VS), (104, DY_VS),
              (73, DY_BT), (70, DY_BT), (77, DY_BT), (101, DY_BT)]
TRICK_BONES = [(73, DY_TR), (70, DY_TR), (77, DY_TR), (101, DY_TR),
               (116, 368.0 + DY_TR - 148.0),  # the engine class panes beside row 4
               (125, DY_VS)]  # last (152): Versus's engine class panes (N_Pw_vs) moved with its row, 2-4 player page
PIC_BONES, PIC_SCALE = (74, 83, 77), 0.78  # the rows' pictures, smaller for four rows
CLONES = [('M4crsr3', 'M4crsr6', 147), ('M4bar3', 'M4bar6', 148), ('MNminP21', 'MNtrP21', 149), ('MminiN', 'MtrickN', 150)]
TITLE_SRC, BAR_SRC = 'Mmini', 'M4bar3'
# the clone's other pictures get materials of their own too (no colour animation names them: the
# runtime gives them Battle's while row 4 is lit, else Battle's lit ring shows on row 4 too)
KID_SRC = ['MMinb1', 'MMinP23', 'MMinP22', 'MMinP21', 'MminM21']
# the icons' textures (the game sets Battle's own at run time; not in the layout's list)
KID_TEX = {'MMinP21': 'trick_star00.bti', 'MMinP22': 'trick_ramp00.bti', 'MMinP23': 'trick_coin00.bti'}
# drawn by icons.py / banner.py (art/), added to timg: the icons, and the TRICK BATTLE banner the runtime
# puts over battlename.arc's BALLOON BATTLE on the stage page in a trick battle (battle.c trick_banner)
ART = ['trick_star00.bti', 'trick_ramp00.bti', 'trick_coin00.bti', 'cup_pict_trick.bti']
# The TRICK STYLE titles (entry_trick.bti: row 4's; mozi_trickbattle.bti: the stage page's name, trick.c) go in blank,
# IA4 256 x 32 on entry_mini_game.bti's header: the app letters them with the disc's MarioFont when it unpacks the
# pack (runtime/trickpak.c trick_title), so no picture of the game's is in it.
TITLES = ['entry_trick.bti', 'mozi_trickbattle.bti']
TEX_NAME = 'Entry_Trick.bti'

def be16(d, o): return struct.unpack('>H', d[o:o+2])[0]
def be32(d, o): return struct.unpack('>I', d[o:o+4])[0]
def pad(b, n, fill=b'\0'):
    return bytes(b) + fill * ((-len(b)) % n)

# ---------- J3D/J2D name tables ----------
def name_hash(s):
    h = 0
    for ch in s.encode():
        h = (h * 3 + ch) & 0xffff
    return h
def read_names(d, o):
    n = be16(d, o)
    out = []
    for i in range(n):
        so = be16(d, o + 4 + 4 * i + 2)
        out.append(d[o+so:d.index(b'\0', o+so)].decode())
    return out
def write_names(names):
    hdr = struct.pack('>HH', len(names), 0xffff)
    offs = b''; strs = b''
    base = 4 + 4 * len(names)
    for s in names:
        offs += struct.pack('>HH', name_hash(s), base + len(strs))
        strs += s.encode() + b'\0'
    return hdr + offs + strs

# ---------- TEX1 ----------
def tex1_add(block, name):
    count = be16(block, 8)
    st = 0x10
    n = be16(block, st)
    assert n == count
    names = []
    for i in range(n):
        so = be16(block, st + 2 + 2 * i)
        ln = block[st + so + 1]
        names.append(block[st + so:st + so + 2 + ln])  # type, length, text
    names.append(bytes([2, len(name)]) + name.encode())
    n += 1
    offs = b''; strs = b''
    base = 2 + 2 * n
    for s in names:
        offs += struct.pack('>H', base + len(strs)); strs += s
    out = block[:8] + struct.pack('>HHI', n, 0xffff, 0x10) + struct.pack('>H', n) + offs + strs
    out = pad(out, 0x20)
    return out[:4] + struct.pack('>I', len(out)) + out[8:], n - 1

# ---------- MAT1 ----------
MAT_FIELDS = 23  # offsets at +0xc .. +0x64
def mat1_add(block, src_mats):
    """append materials: list of (source material, new name, texture index or None); returns block, first new index"""
    count = be16(block, 8)
    offs = [be32(block, 0xc + 4 * i) for i in range(MAT_FIELDS)]
    order = sorted((o, i) for i, o in enumerate(offs) if o)
    secs = {}
    for k, (o, i) in enumerate(order):
        end = order[k + 1][0] if k + 1 < len(order) else len(block)
        secs[i] = bytearray(block[o:end])
    INIT, REMAP, NAMES, IND, TEXNO = 0, 1, 2, 3, 11
    init = secs[INIT]; ind = secs[IND]
    n_init = len(init) // 0xE8
    assert len(init) == n_init * 0xE8, hex(len(init))
    remap_vals = [be16(secs[REMAP], 2 * i) for i in range(count)]
    names = read_names(bytes(secs[NAMES]), 0)
    assert len(names) == count
    ind_size = len(ind) // n_init
    tex_vals = [be16(secs[TEXNO], 2 * i) for i in range(len(secs[TEXNO]) // 2)]
    while tex_vals and tex_vals[-1] > 0x400: tex_vals.pop()  # the table's padding
    first = count
    for src, name, tex in src_mats:
        r = remap_vals[src]
        e = bytearray(init[r * 0xE8:(r + 1) * 0xE8])
        if tex is not None:
            struct.pack_into('>H', e, 0x38, len(tex_vals))  # J2DMaterialInitData: texture slots from +0x38
            tex_vals.append(tex)
        init += e
        ind += ind[r * ind_size:(r + 1) * ind_size]
        remap_vals.append(n_init); n_init += 1
        names.append(name)
        count += 1
    secs[REMAP] = bytearray(pad(b''.join(struct.pack('>H', v) for v in remap_vals), 4))
    secs[NAMES] = bytearray(pad(write_names(names), 4))
    secs[TEXNO] = bytearray(pad(b''.join(struct.pack('>H', v) for v in tex_vals), 4, b'\xff'))
    out = bytearray(block[:order[0][0]])
    new_offs = [0] * MAT_FIELDS
    for o, i in order:
        new_offs[i] = len(out)
        out += secs[i]
    out = bytearray(pad(out, 0x20))
    struct.pack_into('>H', out, 8, count)
    for i in range(MAT_FIELDS):
        struct.pack_into('>I', out, 0xc + 4 * i, new_offs[i])
    struct.pack_into('>I', out, 4, len(out))
    return bytes(out), first

# ---------- blo panes ----------
def tag_bytes(name):
    return name.encode().rjust(8, b'\0')
def subtree(bl, i):
    """block indices [i, j) of pane i with its children (BGN1..END1 right after it)"""
    j = i + 1
    if j < len(bl) and bl[j]['tag'] == b'BGN1':
        depth = 0
        while True:
            if bl[j]['tag'] == b'BGN1': depth += 1
            elif bl[j]['tag'] == b'END1':
                depth -= 1
                if depth == 0: return i, j + 1
            j += 1
    return i, j

def add_clones(data, mats_extra):
    d, bl = blo.load_bytes(data)
    names = {}
    for k, b in enumerate(bl):
        if b['tag'] in blo.PANE:
            names.setdefault(blo.info(d, b)[0], k)
    new = b''; n_new = 0
    for src, dst, bone in CLONES:
        i, j = subtree(bl, names[src])
        n_new += j - i
        top = True
        for b in bl[i:j]:
            raw = bytearray(d[b['off']:b['off'] + b['size']])
            if b['tag'] in blo.PANE:
                po = blo.pan(d, b) - b['off']
                old = blo.info(d, b)[0]
                nm = dst if top else 'Q' + old[1:]
                raw[po+8:po+16] = tag_bytes(nm)
                if top: struct.pack_into('>H', raw, po + 2, bone)
                if b['tag'] == b'PIC2' and old in mats_extra:
                    struct.pack_into('>H', raw, po + 0x40 + 4, mats_extra[old])
                top = False
            new += bytes(raw)
    i, j = subtree(bl, names['N_Mode2'])
    assert bl[j - 1]['tag'] == b'END1'
    at = bl[j - 1]['off']
    return bytes(d[:at]) + new + bytes(d[at:]), n_new

def build_screen(data):
    d = bytes(data)
    o = 0x20; blocks = []
    while o < len(d):
        t = d[o:o+4]; sz = be32(d, o + 4)
        if sz == 0 or t == b'This': break
        blocks.append((o, t, sz)); o += sz
    tail = d[o:]
    d0, bl0 = blo.load_bytes(data)
    src_mat = {}
    for b in bl0:
        if b['tag'] == b'PIC2':
            src_mat[blo.info(d0, b)[0]] = be16(d0, blo.pan(d0, b) + 0x40 + 4)
    parts = []; tex_index = mat_first = None
    for off, t, sz in blocks:
        blk = d[off:off+sz]
        if t == b'TEX1':
            blk, tex_index = tex1_add(blk, TEX_NAME)
            kid_tex = {}
            for k, nm in KID_TEX.items(): blk, kid_tex[k] = tex1_add(blk, nm)
        parts.append([t, blk])
    for p in parts:
        if p[0] == b'MAT1':
            p[1], mat_first = mat1_add(p[1], [(src_mat[TITLE_SRC], 'mat_Trick_title', tex_index),
                                              (src_mat[BAR_SRC], 'mat_Trick_bar', None)] +
                                             [(src_mat[k], 'mat_Trick_' + k, kid_tex.get(k)) for k in KID_SRC])
    scr = d[:0x20] + b''.join(p[1] for p in parts) + tail
    extra = {TITLE_SRC: mat_first, BAR_SRC: mat_first + 1}
    extra.update({k: mat_first + 2 + n for n, k in enumerate(KID_SRC)})
    scr, n_new = add_clones(scr, extra)
    scr = bytearray(scr)
    struct.pack_into('>I', scr, 8, len(scr))
    struct.pack_into('>I', scr, 12, be32(scr, 12) + n_new)
    return bytes(scr), mat_first, tex_index

# ---------- bck ----------
def build_bck(data):
    d = bytes(data)
    a = 0x20
    loop, amul, dur, nb, ns, nr, nt, bo, so, ro, to = struct.unpack('>BBHHHHHIIII', d[a+8:a+0x24])
    bones = [bytearray(d[a+bo+54*b:a+bo+54*b+54]) for b in range(nb)]
    sc = list(struct.unpack('>%df' % ns, d[a+so:a+so+4*ns])); rt = d[a+ro:a+ro+2*nr]
    tr = list(struct.unpack('>%df' % nt, d[a+to:a+to+4*nt]))
    for src, dy in SOLO_BONES + TRICK_BONES:
        b = bytearray(bones[src])
        cnt, idx, tan = struct.unpack('>HHH', b[48:54])  # component 8: J2D's y
        stride = 1 if cnt == 1 else (3 if tan == 0 else 4)
        new_idx = len(tr)
        if cnt == 1:
            tr.append(tr[idx] + dy)
        else:
            for k in range(cnt):
                key = tr[idx + k * stride: idx + k * stride + stride]
                key[1] += dy
                tr += key
        struct.pack_into('>HHH', b, 48, cnt, new_idx, tan)
        if src in PIC_BONES:
            for comp in (0, 6):  # scale x, y
                cnt, idx, tan = struct.unpack('>HHH', b[6*comp:6*comp+6])
                stride = 1 if cnt == 1 else (3 if tan == 0 else 4)
                new_idx = len(sc)
                for k in range(cnt):
                    key = sc[idx + k * stride: idx + k * stride + stride]
                    if cnt == 1: key[0] *= PIC_SCALE
                    else: key[1] *= PIC_SCALE
                    sc += key
                struct.pack_into('>HHH', b, 6*comp, cnt, new_idx, tan)
        bones.append(b)
    nb2 = len(bones); nt2 = len(tr)
    body = bytearray(0x40)
    bo2 = len(body); body += b''.join(bones); body = bytearray(pad(body, 0x20))
    so2 = len(body); body += struct.pack('>%df' % len(sc), *sc); body = bytearray(pad(body, 0x20))
    ro2 = len(body); body += rt; body = bytearray(pad(body, 0x20))
    to2 = len(body); body += struct.pack('>%df' % nt2, *tr); body = bytearray(pad(body, 0x20))
    body[0:4] = b'ANK1'
    struct.pack_into('>I', body, 4, len(body))
    body[8:0x24] = struct.pack('>BBHHHHHIIII', loop, amul, dur, nb2, len(sc), nr, nt2, bo2, so2, ro2, to2)
    out = bytearray(d[:0x20]) + body
    struct.pack_into('>I', out, 8, len(out))
    return bytes(out), nb, nb2

def blank_title(timg):
    t = timg.files['entry_mini_game.bti'].getvalue()
    assert t[0] == 2 and struct.unpack('>HH', t[2:6]) == (256, 32), 'entry_mini_game.bti: an IA4 256 x 32 BTI'
    off = struct.unpack('>I', t[0x1c:0x20])[0]
    return t[:off] + bytes(256 * 32)

def build(src, out):
    a = Archive.from_file(open(src, 'rb'))
    scrn = a.root.subdirs['scrn']; timg = a.root.subdirs['timg']
    f = scrn.files['selectmode.blo']; data, mat_first, tex = build_screen(f.getvalue())
    f.seek(0); f.truncate(); f.write(data); f.seek(0)
    f = scrn.files['selectmode.bck']; data, nb, nb2 = build_bck(f.getvalue())
    f.seek(0); f.truncate(); f.write(data); f.seek(0)
    for n in TITLES:
        nf = File(n); nf.write(blank_title(timg)); nf.seek(0)
        timg.files[n] = nf
    for n in ART:
        nf = File(n); nf.write(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'art', n), 'rb').read()); nf.seek(0)
        timg.files[n] = nf
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as g: a.write_arc(g)
    print('bones %d -> %d, new materials from %d, texture %d' % (nb, nb2, mat_first, tex))

if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2])
