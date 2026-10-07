def unrefpack(b):
    if len(b)<5 or b[1]!=0xFB: return b
    flags=b[0]; p=2
    if flags & 0x80: size=int.from_bytes(b[p:p+4],'big'); p+=4
    else: size=int.from_bytes(b[p:p+3],'big'); p+=3
    if flags & 0x01: p+= 4 if flags & 0x80 else 3
    out=bytearray()
    while p<len(b):
        c0=b[p]
        if c0<0x80:
            c1=b[p+1]; p+=2
            lit=c0&3; out+=b[p:p+lit]; p+=lit
            ln=((c0>>2)&7)+3; off=((c0&0x60)<<3)+c1+1
        elif c0<0xC0:
            c1,c2=b[p+1],b[p+2]; p+=3
            lit=c1>>6; out+=b[p:p+lit]; p+=lit
            ln=(c0&0x3F)+4; off=((c1&0x3F)<<8)+c2+1
        elif c0<0xE0:
            c1,c2,c3=b[p+1],b[p+2],b[p+3]; p+=4
            lit=c0&3; out+=b[p:p+lit]; p+=lit
            ln=((c0&0x0C)<<6)+c3+5; off=((c0&0x10)<<12)+(c1<<8)+c2+1
        elif c0<0xFC:
            lit=((c0&0x1F)<<2)+4; p+=1; out+=b[p:p+lit]; p+=lit; continue
        else:
            lit=c0&3; p+=1; out+=b[p:p+lit]; p+=lit; break
        for i in range(ln): out.append(out[-off])
    return bytes(out)
