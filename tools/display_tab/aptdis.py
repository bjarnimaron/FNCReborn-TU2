# Apt (Flash-like) bytecode disassembler for the Settings screen movie; used by
# aptpatch_display.py to find the end of the frame script and to check the result.
import struct,sys,os
OPS={0x00:'End',0x04:'NextFrame',0x05:'PrevFrame',0x06:'Play',0x07:'Stop',0x0A:'Add',0x0B:'Subtract',0x0C:'Multiply',0x0D:'Divide',0x0E:'Equals',0x0F:'LessThan',0x10:'And',0x11:'Or',0x12:'Not',0x13:'StringEquals',0x14:'StringLength',0x15:'SubString',0x17:'Pop',0x18:'ToInteger',0x1C:'GetVariable',0x1D:'SetVariable',0x20:'SetTarget2',0x21:'StringConcat',0x22:'GetProperty',0x23:'SetProperty',0x24:'CloneSprite',0x25:'RemoveSprite',0x26:'Trace',0x29:'StringCompare',0x2A:'Throw',0x2B:'CastOp',0x2C:'ImplementsOp',0x30:'Random',0x34:'GetTimer',0x3A:'Delete',0x3B:'Delete2',0x3C:'DefineLocal',0x3D:'CallFunction',0x3E:'Return',0x3F:'Modulo',0x40:'NewObject',0x41:'Var',0x42:'InitArray',0x43:'InitObject',0x44:'TypeOf',0x45:'TargetPath',0x46:'Enumerate',0x47:'Add2',0x48:'LessThan2',0x49:'Equals2',0x4A:'ToNumber',0x4B:'ToString',0x4C:'PushDuplicate',0x4D:'StackSwap',0x4E:'GetMember',0x4F:'SetMember',0x50:'Increment',0x51:'Decrement',0x52:'CallMethod',0x53:'NewMethod',0x54:'InstanceOf',0x55:'Enumerate2',0x56:'PushThis',0x58:'PushGlobal',0x59:'PushZero',0x5A:'PushOne',0x5B:'CallFuncPop',0x5C:'CallFunc',0x5D:'CallMethodPop',0x5E:'CallMethod_EA',0x60:'BitAnd',0x61:'BitOr',0x62:'BitXor',0x63:'Shl',0x64:'Shr',0x65:'UShr',0x66:'StrictEqual',0x67:'Greater',0x68:'StringGreater',0x69:'Extends',0x70:'PushThisVar',0x71:'PushGlobalVar',0x72:'ZeroVar',0x73:'PushTrue',0x74:'PushFalse',0x75:'PushNull',0x76:'PushUndefined',0x77:'TraceStart',0x81:'GotoFrame',0x83:'GetURL',0x87:'SetRegister',0x88:'ConstantPool',0x8B:'SetTarget',0x8C:'GotoLabel',0x8E:'DefineFunction2',0x8F:'Try',0x94:'With',0x96:'PushData',0x99:'Branch',0x9A:'GetURL2',0x9B:'DefineFunction',0x9D:'BranchIfTrue',0x9E:'CallFrame',0x9F:'GotoFrame2',0xA1:'PushString',0xA2:'PushConstByte',0xA3:'PushConstWord',0xA4:'GetStringVar',0xA5:'GetStringMember',0xA6:'SetStringVar',0xA7:'SetStringMember',0xAE:'PushValueOfVar',0xAF:'GetNamedMember',0xB0:'CallNamedFuncPop',0xB1:'CallNamedFunc',0xB2:'CallNamedMethodPop',0xB3:'CallNamedMethod',0xB4:'PushFloat',0xB5:'PushByte',0xB6:'PushShort',0xB7:'PushLong',0xB8:'BranchIfFalse',0xB9:'PushRegister',0xFF:'Padding'}
ALIGNED={0x9B,0x8E,0x88,0x9D,0x99,0x96,0x83,0x8C,0x87,0x8B,0x81,0x9F,0x94,0xA1,0xB8,0xA4,0xA6,0xA5,0xA7}
CONSTIDX={0xA2,0xAE,0xAF,0xB0,0xB1,0xB2,0xB3}  # byte const index
def u32(b,o): return struct.unpack('>I',b[o:o+4])[0]
def cstr(b,o):
    e=b.index(b'\0',o); return b[o:e].decode('latin1')
class Const:
    def __init__(s,c):
        n=u32(c,0x18); off=u32(c,0x1C); s.e=[]
        for i in range(n):
            t,v=struct.unpack('>II',c[off+i*8:off+i*8+8])
            if t==1: s.e.append(('s',cstr(c,v)))
            elif t==6: s.e.append(('f',struct.unpack('>f',struct.pack('>I',v))[0]))
            elif t==7: s.e.append(('i',struct.unpack('>i',struct.pack('>I',v))[0]))
            elif t==5: s.e.append(('b',bool(v)))
            elif t==4: s.e.append(('reg',v))
            else: s.e.append((t,v))
def disasm(a,C,start,out,pool=None,depth=0,limit=None):
    p=start; ind='  '*depth
    while p < (limit if limit else len(a)):
        op=a[p]; pos=p; p+=1; name=OPS.get(op,'?%02X'%op); arg=''
        if op in ALIGNED: p=(p+3)&~3
        if op==0x88:
            n=u32(a,p); ptr=u32(a,p+4); p+=8; pool=[u32(a,ptr+i*4) for i in range(n)]; arg=f'{n} consts'
        elif op==0x8E:
            nm=u32(a,p); np_=u32(a,p+4); flags=u32(a,p+8); pp=u32(a,p+12); size=u32(a,p+16); p+=20+8
            params=[(u32(a,pp+i*8),cstr(a,u32(a,pp+i*8+4)) if u32(a,pp+i*8+4) else '') for i in range(np_)]
            fname=cstr(a,nm) if nm else ''
            out.append(f'{pos:06X} {ind}function {fname}({", ".join(f"r{r}:{n}" for r,n in params)}) size={size}')
            disasm(a,C,p,out,pool,depth+1,p+size); p+=size; continue
        elif op==0x9B:
            nm=u32(a,p); cnt=u32(a,p+4); lp=u32(a,p+8); size=u32(a,p+12); p+=16+8
            out.append(f'{pos:06X} {ind}function1 {cstr(a,nm) if nm else ""} size={size}'); disasm(a,C,p,out,pool,depth+1,p+size); p+=size; continue
        elif op==0x96:
            n=u32(a,p); ptr=u32(a,p+4); p+=8
            vals=[C.e[u32(a,ptr+i*4)] for i in range(n)]; arg=' '.join(repr(v[1]) for v in vals)
        elif op in (0x99,0x9D,0xB8):
            o=struct.unpack('>i',a[p:p+4])[0]; p+=4; arg=f'-> {p+o:06X}'
        elif op in (0xA1,0xA4,0xA5,0xA6,0xA7,0x8C):
            arg=repr(cstr(a,u32(a,p))); p+=4
        elif op in (0x87,0x81,0x9F):
            arg=str(u32(a,p)); p+=4
        elif op==0x83:
            arg=repr((cstr(a,u32(a,p)),cstr(a,u32(a,p+4)))); p+=8
        elif op in CONSTIDX:
            i=a[p]; p+=1; v=C.e[pool[i]] if pool and i<len(pool) else ('?',i); arg=repr(v[1])
        elif op==0xA3:
            i=struct.unpack('>H',a[p:p+2])[0]; p+=2; v=C.e[pool[i]] if pool and i<len(pool) else ('?',i); arg=repr(v[1])
        elif op==0xB4: arg=str(struct.unpack('>f',a[p:p+4])[0]); p+=4
        elif op==0xB5: arg=str(a[p]); p+=1
        elif op==0xB6: arg=str(struct.unpack('>H',a[p:p+2])[0]); p+=2
        elif op==0xB7: arg=str(struct.unpack('>i',a[p:p+4])[0]); p+=4
        elif op==0xB9: arg='r%d'%a[p]; p+=1
        elif name.startswith('?'):
            out.append(f'{pos:06X} {ind}{name} !! unknown opcode, stop'); return p
        out.append(f'{pos:06X} {ind}{name} {arg}'.rstrip())
        if op==0x00 and depth==0: return p
    return p
if __name__=='__main__':
    d=sys.argv[1]; a=open(os.path.join(d,'0.dec'),'rb').read(); c=open(os.path.join(d,'1.dec'),'rb').read()
    C=Const(c); out=[]
    start=int(sys.argv[2],16) if len(sys.argv)>2 else 0x78
    disasm(a,C,start,out)
    print('\n'.join(out))

def blocks(a,entry=None):
    """Yield (label, bytecode_offset) for every Action / InitAction in the movie and its sprites."""
    entry=entry if entry is not None else u32(a,0x10)
    seen=set()
    def frames(cnt,ptr,label):
        for fi in range(cnt):
            fr=ptr+fi*8
            icnt,iptr=u32(a,fr),u32(a,fr+4)
            for ii in range(icnt):
                it=u32(a,iptr+ii*4)
                if not it: continue
                t=u32(a,it)
                if t==1: yield (f'{label} frame{fi} action',u32(a,it+4))
                elif t==8: yield (f'{label} frame{fi} initaction(sprite {u32(a,it+4)})',u32(a,it+8))
    def char(p,label):
        if p in seen or not p: return
        seen.add(p)
        t,sig=u32(a,p),u32(a,p+4)
        if sig!=0x09876543: return
        if t==9:
            fc,fp=u32(a,p+16),u32(a,p+20)
            yield from frames(fc,fp,label)
            cc,cp=u32(a,p+28),u32(a,p+32)
            for ci in range(cc):
                cptr=u32(a,cp+ci*4)
                if cptr and cptr!=p: yield from char(cptr,f'char{ci}')
        elif t==5:
            fc,fp=u32(a,p+16),u32(a,p+20)
            yield from frames(fc,fp,label)
    yield from char(entry,'movie')

def disasm_all(d):
    a=open(os.path.join(d,'0.dec'),'rb').read(); c=open(os.path.join(d,'1.dec'),'rb').read(); C=Const(c)
    out=[]; done=set()
    for label,off in blocks(a,u32(c,0x14)):
        if off in done: continue
        done.add(off); out.append(f'; ===== {label} @ {off:06X}')
        try: disasm(a,C,off,out)
        except Exception as e: out.append(f'; !! {e}')
    return out
