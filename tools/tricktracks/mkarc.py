import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from io import BytesIO
from lib.rarc import Archive, File
def load(path):
    from lib.yaz0 import decompress
    d=open(path,'rb').read()
    if d[:4]==b'Yaz0':
        out=BytesIO(); decompress(BytesIO(d),out); d=out.getvalue()
    return Archive.from_file(BytesIO(d))
def save(arc,path):
    with open(path,'wb') as f: arc.write_arc_compressed(f)
def set_file(arc,name,data):
    f=arc.root.files[name]; f.seek(0); f.truncate(); f.write(data); f.seek(0)
