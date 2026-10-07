import struct,sys,os,re
def read_index(path):
    f=open(path,'rb'); d=f.read(0x30)
    n=struct.unpack('>I',d[4:8])[0]; nameoff,namesz=struct.unpack('>II',d[0xC:0x14]); namelen=d[0x14]; pathlen=d[0x15]; npaths=struct.unpack('>H',d[0x16:0x18])[0]
    f.seek(0x30); tab=f.read(n*16)
    f.seek(nameoff); names=f.read(namesz)
    paths=[]
    po=n*namelen
    for i in range(npaths):
        p=names[po+i*pathlen:po+(i+1)*pathlen].split(b'\0')[0].decode(errors='replace'); paths.append(p)
    out=[]
    for i in range(n):
        off,z,size,h=struct.unpack('>IIII',tab[i*16:i*16+16])
        rec=names[i*namelen:(i+1)*namelen]
        pidx=struct.unpack('>H',rec[:2])[0]; nm=rec[2:].split(b'\0')[0].decode(errors='replace')
        out.append((nm, paths[pidx] if pidx<len(paths) else '?', off<<4, size, z))
    return out
if __name__=='__main__':
    path=sys.argv[1]; pat=re.compile(sys.argv[2]) if len(sys.argv)>2 else None; dest=sys.argv[3] if len(sys.argv)>3 else None
    idx=read_index(path)
    f=open(path,'rb')
    for nm,p,off,size,z in idx:
        if pat and not pat.search(nm): continue
        print(f'{p}{nm} off={off:#x} size={size} z={z}')
        if dest:
            f.seek(off); data=f.read(size); open(os.path.join(dest,nm),'wb').write(data)
