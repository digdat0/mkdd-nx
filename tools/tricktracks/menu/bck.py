import struct
def load(path='selectmode.bck'):
    d = open(path, 'rb').read()
    a = 0x20
    loop, amul, dur, nb, ns, nr, nt, bo, so, ro, to = struct.unpack('>BBHHHHHIIII', d[a+8:a+0x24])
    sc = struct.unpack('>%df' % ns, d[a+so:a+so+4*ns])
    tr = struct.unpack('>%df' % nt, d[a+to:a+to+4*nt])
    rt = struct.unpack('>%dh' % nr, d[a+ro:a+ro+2*nr])
    bones = []
    for b in range(nb):
        comps = [struct.unpack('>HHH', d[a+bo+b*54+6*i:a+bo+b*54+6*i+6]) for i in range(9)]
        bones.append(comps)
    return dict(d=d, dur=dur, sc=sc, tr=tr, rt=rt, bones=bones, offs=(bo, so, ro, to))
def keys(arr, c):
    n, i, t = c
    if n == 1: return [(0, arr[i])]
    st = 3 if t == 0 else 4
    return [tuple(arr[i+k*st:i+k*st+st]) for k in range(n)]
if __name__ == '__main__':
    import sys
    B = load()
    names = [l.split()[0] for l in open('panes.txt')] if False else None
    print('dur', B['dur'])
    for b in map(int, sys.argv[1:]):
        c = B['bones'][b]
        print(b, 'tx', keys(B['tr'], c[2])[:6], '\n   ty', keys(B['tr'], c[5])[:8], '\n   sx', keys(B['sc'], c[0])[:5])
