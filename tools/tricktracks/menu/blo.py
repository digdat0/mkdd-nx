import struct
PANE = (b'PAN2', b'PIC2', b'WIN2', b'TBX2')
def load(path='selectmode.blo'):
    return load_bytes(open(path, 'rb').read())
def load_bytes(data):
    d = bytearray(data)
    o = 0x20; blocks = []; depth = 0
    while o < len(d):
        t = bytes(d[o:o+4]); sz = struct.unpack('>I', d[o+4:o+8])[0]
        if sz == 0 or t == b'This': break
        blocks.append(dict(off=o, tag=t, size=sz, depth=depth))
        if t == b'BGN1': depth += 1
        elif t == b'END1': depth -= 1
        o += sz
    return d, blocks
def pan(d, b):
    """offset of the pan2 body (fields) inside a pane block"""
    o = b['off'] + 8
    if b['tag'] != b'PAN2': o += 8  # PIC2/WIN2/TBX2 embed a 'pan2' sub-header
    return o
def info(d, b):
    o = pan(d, b)
    name = bytes(d[o+8:o+16]).lstrip(b'\0').decode('latin1')
    w, h, sx, sy, rx, ry, rz, tx, ty = struct.unpack('>9f', d[o+0x18:o+0x3c])
    return name, w, h, sx, sy, rz, tx, ty
if __name__ == '__main__':
    d, bl = load()
    i = 0
    for b in bl:
        if b['tag'] in PANE:
            n, w, h, sx, sy, rz, tx, ty = info(d, b)
            print('%3d %-4s %s%-10s w%6.0f h%5.0f s%.2f,%.2f t(%6.1f,%6.1f)' % (i, b['tag'].decode(), '  ' * b['depth'], n, w, h, sx, sy, tx, ty))
            i += 1
