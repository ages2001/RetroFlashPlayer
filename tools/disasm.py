import struct, sys
import sys as _s
body=open(_s.argv[-1] if _s.argv[-1].endswith('.bin') else 'ref_body.bin','rb').read()
nb=body[0]>>3; p=(5+4*nb+7)//8+4
NAMES={0x04:'NextFrame',0x05:'PrevFrame',0x06:'Play',0x07:'Stop',0x09:'StopSounds',0x0A:'Add',0x0B:'Sub',0x0C:'Mul',0x0D:'Div',0x0E:'Equals',0x0F:'Less',0x10:'And',0x11:'Or',0x12:'Not',0x13:'StrEq',0x14:'StrLen',0x17:'Pop',0x18:'ToInt',0x1C:'GetVariable',0x1D:'SetVariable',0x20:'SetTarget2',0x21:'StrAdd',0x22:'GetProperty',0x23:'SetProperty',0x24:'CloneSprite',0x25:'RemoveSprite',0x26:'Trace',0x27:'StartDrag',0x28:'EndDrag',0x2A:'Throw',0x2B:'Cast',0x2C:'Implements',0x30:'Random',0x34:'GetTime',0x3A:'Delete',0x3B:'Delete2',0x3C:'DefineLocal',0x3D:'CallFunction',0x3E:'Return',0x3F:'Mod',0x40:'NewObject',0x41:'DefineLocal2',0x42:'InitArray',0x43:'InitObject',0x44:'TypeOf',0x45:'TargetPath',0x46:'Enumerate',0x47:'Add2',0x48:'Less2',0x49:'Equals2',0x4A:'ToNumber',0x4B:'ToString',0x4C:'Dup',0x4D:'Swap',0x4E:'GetMember',0x4F:'SetMember',0x50:'Inc',0x51:'Dec',0x52:'CallMethod',0x53:'NewMethod',0x54:'InstanceOf',0x55:'Enumerate2',0x60:'BitAnd',0x61:'BitOr',0x62:'BitXor',0x63:'Shl',0x64:'Shr',0x65:'UShr',0x66:'StrictEq',0x67:'Greater',0x68:'StrGreater',0x69:'Extends',0x81:'GotoFrame',0x83:'GetURL',0x87:'StoreRegister',0x88:'ConstantPool',0x8A:'WaitForFrame',0x8B:'SetTarget',0x8C:'GotoLabel',0x8D:'WaitForFrame2',0x8E:'DefineFunction2',0x8F:'Try',0x94:'With',0x96:'Push',0x99:'Jump',0x9A:'GetURL2',0x9B:'DefineFunction',0x9D:'If',0x9E:'Call',0x9F:'GotoFrame2'}
def disasm(code, indent=0, limit=400):
    pool=[]; i=0; out=[]
    while i<len(code) and len(out)<limit:
        op=code[i]; 
        if op>=0x80:
            ln=struct.unpack('<H',code[i+1:i+3])[0]; d=code[i+3:i+3+ln]; nxt=i+3+ln
        else: d=b''; nxt=i+1
        name=NAMES.get(op,hex(op)); extra=''
        if op==0x88:
            n=struct.unpack('<H',d[:2])[0]; pool=d[2:].split(b'\0')[:n]; extra=f'{n} strings: '+', '.join(repr(x.decode("latin1")) for x in pool[:12])+('...' if n>12 else '')
        elif op==0x96:
            j=0; items=[]
            while j<len(d):
                t=d[j]; j+=1
                if t==0: e=d.index(b'\0',j); items.append(repr(d[j:e].decode('latin1'))); j=e+1
                elif t==1: items.append(str(struct.unpack('<f',d[j:j+4])[0])); j+=4
                elif t==2: items.append('null')
                elif t==3: items.append('undefined')
                elif t==4: items.append(f'r{d[j]}'); j+=1
                elif t==5: items.append(str(bool(d[j]))); j+=1
                elif t==6: hi,lo=struct.unpack('<II',d[j:j+8]); items.append(str(struct.unpack('<d',struct.pack('<II',lo,hi))[0])); j+=8
                elif t==7: items.append(str(struct.unpack('<i',d[j:j+4])[0])); j+=4
                elif t==8: k=d[j]; items.append('c:'+(repr(pool[k].decode('latin1')) if k<len(pool) else f'#{k}')); j+=1
                elif t==9: k=struct.unpack('<H',d[j:j+2])[0]; items.append('c:'+(repr(pool[k].decode('latin1')) if k<len(pool) else f'#{k}')); j+=2
                else: break
            extra=' '.join(items)
        elif op in (0x99,0x9D): extra=f'-> {nxt+struct.unpack("<h",d[:2])[0]}'
        elif op==0x9B:
            e=d.index(b'\0'); nm=d[:e].decode('latin1'); np=struct.unpack('<H',d[e+1:e+3])[0]; q=e+3; params=[]
            for _ in range(np): e2=d.index(b'\0',q); params.append(d[q:e2].decode('latin1')); q=e2+1
            cs=struct.unpack('<H',d[q:q+2])[0]; extra=f'{nm or "<anon>"}({", ".join(params)}) size={cs}'
            out.append(' '*indent+f'{i:5d}: {name} {extra}'); out+=disasm(code[nxt:nxt+cs],indent+4,limit-len(out)); nxt+=cs; i=nxt; continue
        elif op==0x8E:
            e=d.index(b'\0'); nm=d[:e].decode('latin1'); np=struct.unpack('<H',d[e+1:e+3])[0]; q=e+3; nregs=d[q]; flags=struct.unpack('<H',d[q+1:q+3])[0]; q+=3; params=[]
            for _ in range(np): rg=d[q]; e2=d.index(b'\0',q+1); params.append(f'r{rg}:'+d[q+1:e2].decode('latin1')); q=e2+1
            cs=struct.unpack('<H',d[q:q+2])[0]; extra=f'{nm or "<anon>"}({", ".join(params)}) regs={nregs} flags={flags:#x} size={cs}'
            out.append(' '*indent+f'{i:5d}: {name} {extra}'); out+=disasm(code[nxt:nxt+cs],indent+4,limit-len(out)); nxt+=cs; i=nxt; continue
        elif op==0x87: extra=f'r{d[0]}'
        elif op==0x8B or op==0x8C: extra=repr(d.rstrip(b'\0').decode('latin1'))
        elif op==0x81: extra=str(struct.unpack('<H',d[:2])[0])
        elif op==0x9F: extra=f'flags={d[0]}'
        out.append(' '*indent+f'{i:5d}: {name} {extra}')
        i=nxt
        if op==0: break
    return out
# collect root-level DoAction per frame
frame=1; i=p; want=int(sys.argv[1]) if len(sys.argv)>1 else 1
while i<len(body):
    th=struct.unpack('<H',body[i:i+2])[0]; i+=2; code=th>>6; ln=th&0x3f
    if ln==0x3f: ln=struct.unpack('<I',body[i:i+4])[0]; i+=4
    if code==12 and frame==want:
        print(f'--- root frame {frame} DoAction ({ln} bytes) ---'); print('\n'.join(disasm(body[i:i+ln])))
    if code==1: frame+=1
    if code==0: break
    i+=ln

def sprite_actions(sid):
    i=p
    while i<len(body):
        th=struct.unpack('<H',body[i:i+2])[0]; i+=2; code=th>>6; ln=th&0x3f
        if ln==0x3f: ln=struct.unpack('<I',body[i:i+4])[0]; i+=4
        if code==39 and struct.unpack('<H',body[i:i+2])[0]==sid:
            j=i+4; fr=1
            while j<i+ln:
                th2=struct.unpack('<H',body[j:j+2])[0]; j+=2; c2=th2>>6; l2=th2&0x3f
                if l2==0x3f: l2=struct.unpack('<I',body[j:j+4])[0]; j+=4
                if c2==12: print(f'--- sprite {sid} frame {fr} DoAction ---'); print('\n'.join(disasm(body[j:j+l2])))
                if c2==26 or c2==70:
                    flags=body[j]; 
                    if flags&0x80: print(f'--- sprite {sid} frame {fr}: PlaceObject2 with CLIPACTIONS, depth {struct.unpack("<H",body[j+1:j+3])[0]}')
                if c2==1: fr+=1
                j+=l2
        if code==0: break
        i+=ln
def frame_places(want):
    i=p; frame=1
    while i<len(body):
        th=struct.unpack('<H',body[i:i+2])[0]; i+=2; code=th>>6; ln=th&0x3f
        if ln==0x3f: ln=struct.unpack('<I',body[i:i+4])[0]; i+=4
        if code in (26,70) and frame==want:
            flags=body[i]; q=i+1+(1 if code==70 else 0); depth=struct.unpack('<H',body[q:q+2])[0]; q+=2
            cid=struct.unpack('<H',body[q:q+2])[0] if flags&2 else -1
            print(f'frame {frame}: place depth={depth} char={cid} flags={flags:#x} clipactions={bool(flags&0x80)}')
        if code==1: frame+=1
        if code==0: break
        i+=ln
if len(sys.argv)>2 and sys.argv[2]=='places': frame_places(int(sys.argv[1]))
if len(sys.argv)>2 and sys.argv[2]=='sprite': sprite_actions(int(sys.argv[1]))
