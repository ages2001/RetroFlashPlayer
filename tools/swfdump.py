#!/usr/bin/env python3
"""swfdump.py - list the tags of a SWF (sprites included) and disassemble
its AVM1 code: frame actions, init actions, clip event handlers and button
actions.  Usage: swfdump.py file.swf [--no-code]"""
import struct, sys, zlib

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

TAGS={0:'End',1:'ShowFrame',2:'DefineShape',4:'PlaceObject',5:'RemoveObject',6:'DefineBits',7:'DefineButton',9:'SetBackgroundColor',10:'DefineFont',11:'DefineText',12:'DoAction',14:'DefineSound',15:'StartSound',17:'DefineButtonSound',18:'SoundStreamHead',19:'SoundStreamBlock',20:'DefineBitsLossless',21:'DefineBitsJPEG2',22:'DefineShape2',26:'PlaceObject2',28:'RemoveObject2',32:'DefineShape3',33:'DefineText2',34:'DefineButton2',35:'DefineBitsJPEG3',36:'DefineBitsLossless2',37:'DefineEditText',39:'DefineSprite',43:'FrameLabel',45:'SoundStreamHead2',46:'DefineMorphShape',48:'DefineFont2',56:'ExportAssets',57:'ImportAssets',59:'DoInitAction',60:'DefineVideoStream',62:'DefineFontInfo2',69:'FileAttributes',70:'PlaceObject3',73:'DefineFontAlignZones',74:'CSMTextSettings',75:'DefineFont3',76:'SymbolClass',77:'Metadata',82:'DoABC',83:'DefineShape4',86:'DefineSceneAndFrameLabelData',88:'DefineFontName'}

def tags(data, pos, end):
    while pos + 2 <= end:
        th = struct.unpack('<H', data[pos:pos+2])[0]; pos += 2
        code, ln = th >> 6, th & 0x3f
        if ln == 0x3f: ln = struct.unpack('<I', data[pos:pos+4])[0]; pos += 4
        yield code, data[pos:pos+ln]
        pos += ln
        if code == 0: break

def cstr(d, i):
    e = d.index(b'\0', i); return d[i:e].decode('latin1'), e + 1

def skip_matrix(d, i):
    bits = int.from_bytes(d[i:i+32].ljust(32, b'\0'), 'big'); n = 0
    def rd(k):
        nonlocal n; v = (bits >> (256 - n - k)) & ((1 << k) - 1); n += k; return v
    if rd(1): nb = rd(5); rd(nb); rd(nb)
    if rd(1): nb = rd(5); rd(nb); rd(nb)
    nb = rd(5); rd(nb); rd(nb)
    return i + (n + 7) // 8

def skip_cxform(d, i):
    bits = int.from_bytes(d[i:i+20].ljust(20, b'\0'), 'big'); n = 0
    def rd(k):
        nonlocal n; v = (bits >> (160 - n - k)) & ((1 << k) - 1); n += k; return v
    ha, hm = rd(1), rd(1); nb = rd(4)
    for _ in range(4 * (ha + hm)): rd(nb)
    return i + (n + 7) // 8

def clip_actions(d, i, ver, ind, code):
    i += 2
    wide = ver >= 6
    i += 4 if wide else 2
    while i < len(d):
        fl = struct.unpack('<I' if wide else '<H', d[i:i+(4 if wide else 2)])[0]; i += 4 if wide else 2
        if not fl: break
        sz = struct.unpack('<I', d[i:i+4])[0]; i += 4
        body = d[i:i+sz]; i += sz
        kc = ''
        if fl & 0x20000: kc = f' key={body[0]}'; body = body[1:]
        print(ind + f'  onClipEvent flags={fl:#x}{kc} ({len(body)} bytes)')
        if code: print('\n'.join(ind + '    ' + l for l in disasm(body)))

def place2(d, ver, ind, code, v3=False):
    fl = d[0]; i = 1; fl2 = 0
    if v3: fl2 = d[1]; i = 2
    depth = struct.unpack('<H', d[i:i+2])[0]; i += 2
    s = f'depth={depth}'
    if v3 and (fl2 & 8 or (fl2 & 16 and fl & 2)): cn, i = cstr(d, i); s += f' class={cn!r}'
    if fl & 2: s += f' id={struct.unpack("<H", d[i:i+2])[0]}'; i += 2
    if fl & 1: s = 'move ' + s
    if fl & 4: i = skip_matrix(d, i); s += ' matrix'
    if fl & 8: i = skip_cxform(d, i); s += ' cxform'
    if fl & 16: s += f' ratio={struct.unpack("<H", d[i:i+2])[0]}'; i += 2
    if fl & 32: nm, i = cstr(d, i); s += f' name={nm!r}'
    if fl & 64: s += f' clipdepth={struct.unpack("<H", d[i:i+2])[0]}'; i += 2
    if v3:
        if fl2 & 1: s += ' filters'; return print(ind + s)
        if fl2 & 2: s += f' blend={d[i]}'; i += 1
        if fl2 & 4: s += ' cacheAsBitmap'; i += 1
    print(ind + s)
    if fl & 128: clip_actions(d, i, ver, ind, code)

def button(d, v2, ver, ind, code):
    bid = struct.unpack('<H', d[:2])[0]; i = 2
    if v2: track = d[i]; i += 1; aoff = struct.unpack('<H', d[i:i+2])[0]; i += 2
    print(ind + f'DefineButton{"2" if v2 else ""} id={bid}' + (f' trackAsMenu={track & 1}' if v2 else ''))
    while d[i]:
        st = d[i]; cid, dep = struct.unpack('<HH', d[i+1:i+5]); print(ind + f'  record states={st:#x} id={cid} depth={dep}')
        i = skip_matrix(d, i + 5)
        if v2: i = skip_cxform(d, i)
        if v2 and st & 0x10: n = d[i]; i += 1 + n * 0  # filters: not decoded
        if v2 and st & 0x20: i += 1
    i += 1
    if not v2:
        print(ind + '  actions'); 
        if code: print('\n'.join(ind + '    ' + l for l in disasm(d[i:])))
        return
    if not aoff: return
    i = 2 + 1 + aoff
    while i < len(d):
        sz, cond = struct.unpack('<HH', d[i:i+4])
        body = d[i+4:i+sz] if sz else d[i+4:]
        print(ind + f'  condition {cond:#06x} key={cond >> 9}')
        if code: print('\n'.join(ind + '    ' + l for l in disasm(body)))
        if not sz: break
        i += sz

def dump(data, pos, end, ver, ind, code):
    frame = 1
    print(ind + f'frame {frame}')
    for c, d in tags(data, pos, end):
        name = TAGS.get(c, str(c))
        if c == 1:
            frame += 1; print(ind + f'frame {frame}'); continue
        if c == 0: print(ind + 'End'); break
        if c == 39:
            sid, nf = struct.unpack('<HH', d[:4]); print(ind + f'  DefineSprite id={sid} frames={nf}')
            dump(d, 4, len(d), ver, ind + '    ', code); continue
        if c in (26, 70): print(ind + f'  {name} ', end=''); place2(d, ver, '', code, c == 70); continue
        if c == 4: print(ind + f'  PlaceObject depth={struct.unpack("<H", d[2:4])[0]} id={struct.unpack("<H", d[:2])[0]}'); continue
        if c == 28: print(ind + f'  RemoveObject2 depth={struct.unpack("<H", d[:2])[0]}'); continue
        if c == 5: print(ind + f'  RemoveObject id={struct.unpack("<H", d[:2])[0]} depth={struct.unpack("<H", d[2:4])[0]}'); continue
        if c == 12:
            print(ind + f'  DoAction ({len(d)} bytes)')
            if code: print('\n'.join(ind + '    ' + l for l in disasm(d)))
            continue
        if c == 59:
            print(ind + f'  DoInitAction sprite={struct.unpack("<H", d[:2])[0]}')
            if code: print('\n'.join(ind + '    ' + l for l in disasm(d[2:])))
            continue
        if c in (7, 34): button(d, c == 34, ver, ind + '  ', code); continue
        if c == 43: print(ind + f'  FrameLabel {cstr(d, 0)[0]!r}'); continue
        if c == 56:
            n = struct.unpack('<H', d[:2])[0]; i = 2; out = []
            for _ in range(n):
                cid = struct.unpack('<H', d[i:i+2])[0]; nm, i = cstr(d, i + 2); out.append(f'{cid}={nm!r}')
            print(ind + '  ExportAssets ' + ' '.join(out)); continue
        if c == 37:
            cid = struct.unpack('<H', d[:2])[0]; i = 2 + 9 + 2  # rect approximated below
            print(ind + f'  DefineEditText id={cid}'); continue
        cid = struct.unpack('<H', d[:2])[0] if len(d) >= 2 and name.startswith('Define') else None
        print(ind + f'  {name}' + (f' id={cid}' if cid is not None else '') + f' ({len(d)} bytes)')

def main():
    path = [a for a in sys.argv[1:] if not a.startswith('--')][0]
    code = '--no-code' not in sys.argv
    raw = open(path, 'rb').read()
    ver = raw[3]
    if raw[:1] == b'C': body = zlib.decompress(raw[8:])
    elif raw[:1] == b'Z':
        import lzma
        props = raw[12:17]; dsz = struct.unpack('<I', raw[4:8])[0] - 8
        body = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)]).decompress(raw[17:])[:dsz]
    else: body = raw[8:]
    nb = body[0] >> 3; p = (5 + 4 * nb + 7) // 8 + 4
    rate, frames = body[p-4:p-2], struct.unpack('<H', body[p-2:p])[0]
    print(f'SWF version {ver}, {frames} frames, rate {rate[1] + rate[0] / 256}')
    dump(body, p, len(body), ver, '', code)

main()
