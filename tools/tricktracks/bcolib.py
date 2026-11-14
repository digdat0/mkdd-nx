"""MKDD BCO (collision, format "0003") reader/writer."""
import struct, math

class BCO:
    def __init__(s):
        s.verts=[]; s.tris=[]  # tris: dict(v=(a,b,c), typ=u16, flag=u8, tail=u32)
        s.mats=[]  # (u8,u8,u16,u32,u32)
        s.x0=-25000; s.z0=-15000; s.cx=1000; s.cz=1000; s.gx=0; s.gz=0

def read(d):
    b=BCO()
    assert d[:4]==b'0003'
    b.gx,b.gz,b.x0,b.z0,b.cx,b.cz=struct.unpack_from('>HHiiii',d,4)
    n,_,io,to,vo,mo=struct.unpack_from('>HHIIII',d,0x18)
    b.verts=[struct.unpack_from('>fff',d,vo+i*12) for i in range((mo-vo)//12)]
    for i in range((vo-to)//0x24):
        o=to+i*0x24
        v=struct.unpack_from('>iii',d,o)
        typ=struct.unpack_from('>H',d,o+0x16)[0]
        b.tris.append(dict(v=v,typ=typ,flag=d[o+0x18],tail=struct.unpack_from('>I',d,o+0x20)[0],pl=bytes(d[o+0x0C:o+0x16]),p0=tuple(b.verts[k] for k in v)))
    b.mats=[struct.unpack_from('>BBHII',d,mo+i*12) for i in range(n)]
    b._raw=d
    return b

def tri_normal(a,b,c):
    u=(b[0]-a[0],b[1]-a[1],b[2]-a[2]);w=(c[0]-a[0],c[1]-a[1],c[2]-a[2])
    n=(u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0])
    l=math.sqrt(n[0]**2+n[1]**2+n[2]**2)
    if l==0: return (0.0,1.0,0.0)
    return (n[0]/l,n[1]/l,n[2]/l)

def _overlap_cell(a,b,c,x0,z0,x1,z1):
    """triangle (XZ projection) vs rectangle"""
    pts=[(a[0],a[2]),(b[0],b[2]),(c[0],c[2])]
    x0-=2;z0-=2;x1+=2;z1+=2
    if max(p[0] for p in pts)<x0 or min(p[0] for p in pts)>x1 or max(p[1] for p in pts)<z0 or min(p[1] for p in pts)>z1: return False
    # SAT with triangle edge normals
    for i in range(3):
        p,q=pts[i],pts[(i+1)%3]
        nx,nz=-(q[1]-p[1]),(q[0]-p[0])
        tv=[nx*r[0]+nz*r[1] for r in pts]
        cv=[nx*x+nz*z for x in (x0,x1) for z in (z0,z1)]
        if max(tv)<min(cv) or min(tv)>max(cv): return False
    return True

def write(b, flags=None, max_leaf=24, max_depth=5, order='xz'):
    V=b.verts; T=b.tris
    # grid bounds from verts
    xs=[v[0] for v in V]; zs=[v[2] for v in V]
    if b.gx==0:
        b.x0=int(math.floor(min(xs)/b.cx)*b.cx)-b.cx; b.z0=int(math.floor(min(zs)/b.cz)*b.cz)-b.cz
        b.gx=int((max(xs)-b.x0)//b.cx)+2; b.gz=int((max(zs)-b.z0)//b.cz)+2
    pos=[[V[i] for i in t['v']] for t in T]
    MARGIN=50.0
    def _overlap_cell(a,b,c,x0,z0,x1,z1):
        # bounding-box overlap like the original grids, plus a margin (the game does the exact test)
        return not (max(a[0],b[0],c[0])<x0-MARGIN or min(a[0],b[0],c[0])>x1+MARGIN or max(a[2],b[2],c[2])<z0-MARGIN or min(a[2],b[2],c[2])>z1+MARGIN)
    table=[]; lists=[]
    def make(cell_tris,x0,z0,size,depth):
        """returns entry tuple; appends children to table as needed"""
        if len(cell_tris)>max_leaf and depth<max_depth:
            base=len(table)
            table.extend([None]*4)
            h=size/2
            kids=[]
            for qi in range(4):
                qx,qz=(qi&1,qi>>1) if order=='xz' else (qi>>1,qi&1)
                cx0=x0+qx*h; cz0=z0+qz*h
                kids.append((qi,cx0,cz0))
            for qi,cx0,cz0 in kids:
                sub=[t for t in cell_tris if _overlap_cell(*pos[t],cx0,cz0,cx0+h,cz0+h)]
                table[base+qi]=make(sub,cx0,cz0,h,depth+1)
            return (0,0,base,0)
        cell_tris=sorted(cell_tris,key=lambda t:(-max(q[1] for q in pos[t]),t))  # original lists are highest-first
        off=len(lists); lists.extend(cell_tris)
        return (len(cell_tris),0,0,off)
    root=[]
    # root cells occupy the first gx*gz entries
    table.extend([None]*(b.gx*b.gz))
    # 2026-11-12: each triangle listed in the root cells its box (plus the margin) can touch, then checked as before
    # (same lists, same order: ascending triangle index), instead of every triangle tested against every cell
    cand=[[] for _ in range(b.gx*b.gz)]
    for i,(p,q,r) in enumerate(pos):
        gx0=max(0,int(math.floor((min(p[0],q[0],r[0])-MARGIN-b.x0)/b.cx))-1); gx1=min(b.gx-1,int(math.floor((max(p[0],q[0],r[0])+MARGIN-b.x0)/b.cx))+1)
        gz0=max(0,int(math.floor((min(p[2],q[2],r[2])-MARGIN-b.z0)/b.cz))-1); gz1=min(b.gz-1,int(math.floor((max(p[2],q[2],r[2])+MARGIN-b.z0)/b.cz))+1)
        for gz in range(gz0,gz1+1):
            for gx in range(gx0,gx1+1): cand[gz*b.gx+gx].append(i)
    for gz in range(b.gz):
        for gx in range(b.gx):
            x0=b.x0+gx*b.cx; z0=b.z0+gz*b.cz
            ct=[i for i in cand[gz*b.gx+gx] if _overlap_cell(*pos[i],x0,z0,x0+b.cx,z0+b.cz)]
            table[gz*b.gx+gx]=make(ct,x0,z0,b.cx,0)
    for e in table: assert e is not None
    # neighbours by shared edges (vertex index pairs)
    edges={}
    for i,t in enumerate(T):
        a,bb,c=t['v']
        for k,(p,q) in enumerate(((a,bb),(bb,c),(c,a))):
            edges.setdefault((min(p,q),max(p,q)),[]).append((i,k))
    nb=[[0xffff]*3 for _ in T]
    for key,lst in edges.items():
        if len(lst)==2:
            (i,k),(j,m)=lst
            nb[i][k]=j; nb[j][m]=i
        elif len(lst)>2:
            for (i,k) in lst:
                o=[x for x in lst if x[0]!=i][0]; nb[i][k]=o[0]
    out=bytearray(0x2c)
    tbl=bytearray()
    for (cnt,u2,nxt,off) in table: tbl+=struct.pack('>BBHi',cnt,u2,nxt,off)
    lst=b''.join(struct.pack('>H',x) for x in lists)
    while len(lst)%4: lst+=b'\0'
    io=0x2c+len(tbl); to=io+len(lst); vo=to+len(T)*0x24; mo=vo+len(V)*12
    out[0:4]=b'0003'
    struct.pack_into('>HHiiii',out,4,b.gx,b.gz,b.x0,b.z0,b.cx,b.cz)
    struct.pack_into('>HHIIII',out,0x18,len(b.mats),0,io,to,vo,mo)
    out+=tbl; out+=lst
    for i,t in enumerate(T):
        a,bb,c=[V[k] for k in t['v']]
        n=tri_normal(a,bb,c)
        d=-(n[0]*a[0]+n[1]*a[1]+n[2]*a[2])
        fl=t['flag'] if flags is None else flags[i]
        if t.get('pl') is not None and t.get('p0') == tuple((a,bb,c)):
            dd=struct.unpack('>fhhh',t['pl'])
            r=struct.pack('>iiifhhhHBBHHHI',*t['v'],dd[0],dd[1],dd[2],dd[3],t['typ'],fl,1,nb[i][0],nb[i][1],nb[i][2],t.get('tail',0x50000000))
        else:
            r=struct.pack('>iiifhhhHBBHHHI',*t['v'],d,*(int(round(x*10000)) for x in n),t['typ'],fl,1,nb[i][0],nb[i][1],nb[i][2],t.get('tail',0x50000000))
        assert len(r)==0x24,len(r)
        out+=r
    for v in V: out+=struct.pack('>fff',*v)
    for m in b.mats: out+=struct.pack('>BBHII',*m)
    return bytes(out)
