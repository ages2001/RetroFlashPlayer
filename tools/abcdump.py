#!/usr/bin/env python3
"""abcdump.py - dump the ABC (AVM2 bytecode) in a SWF: classes, traits and
disassembled method bodies.  usage: abcdump.py file.swf [filter-substring]"""
import sys, zlib, struct, os

def read_swf(path):
    d = open(path, 'rb').read()
    sig = d[:3]
    if sig == b'CWS': d = d[:8] + zlib.decompress(d[8:])
    elif sig == b'ZWS':
        import lzma
        props = d[12:17]; body = d[17:]
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)])
        d = d[:8] + dec.decompress(body)
    nb = d[8] >> 3; p = 8 + (5 + 4 * nb + 7) // 8 + 4
    abcs = []
    while p < len(d):
        h = struct.unpack_from('<H', d, p)[0]; p += 2
        code, ln = h >> 6, h & 63
        if ln == 63: ln = struct.unpack_from('<I', d, p)[0]; p += 4
        body = d[p:p + ln]; p += ln
        if code == 82:  # DoABC
            flags = struct.unpack_from('<I', body)[0]; q = 4
            while body[q]: q += 1
            abcs.append((body[4:q].decode('latin1'), body[q + 1:]))
        elif code == 72: abcs.append(('', body))
        if code == 0: break
    return abcs

class R:
    def __init__(s, b, p=0): s.b = b; s.p = p
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.p)[0]; s.p += 2; return v
    def s24(s): v = int.from_bytes(s.b[s.p:s.p + 3], 'little', signed=True); s.p += 3; return v
    def u30(s):
        v = 0; sh = 0
        for i in range(5):
            c = s.b[s.p]; s.p += 1; v |= (c & 0x7f) << sh; sh += 7
            if not c & 0x80: break
        return v & 0xffffffff
    def s32(s):
        v = s.u30()
        return v - (1 << 32) if v & 0x80000000 else v
    def d64(s): v = struct.unpack_from('<d', s.b, s.p)[0]; s.p += 8; return v

NSK = {0x08: 'ns', 0x16: 'pkg', 0x17: 'pkgint', 0x18: 'prot', 0x19: 'expl', 0x1A: 'sprot', 0x05: 'priv'}

class Abc:
    def __init__(s, b):
        r = R(b); s.r = r
        r.u16(); r.u16()
        s.ints = [0] + [r.s32() for _ in range(max(r.u30() - 1, 0))]
        s.uints = [0] + [r.u30() for _ in range(max(r.u30() - 1, 0))]
        s.dbls = [float('nan')] + [r.d64() for _ in range(max(r.u30() - 1, 0))]
        n = r.u30(); s.strs = ['*']
        for _ in range(max(n - 1, 0)):
            l = r.u30(); s.strs.append(b[r.p:r.p + l].decode('utf8', 'replace')); r.p += l
        n = r.u30(); s.nss = ['*']
        for _ in range(max(n - 1, 0)):
            k = r.u8(); nm = r.u30(); s.nss.append('%s:%s' % (NSK.get(k, hex(k)), s.strs[nm] if nm else ''))
        n = r.u30(); s.nssets = [[]]
        for _ in range(max(n - 1, 0)):
            c = r.u30(); s.nssets.append([r.u30() for _ in range(c)])
        n = r.u30(); s.mn = ['*']
        for _ in range(max(n - 1, 0)):
            k = r.u8()
            if k in (0x07, 0x0D): ns = r.u30(); nm = r.u30(); s.mn.append('%s%s::%s' % ('@' if k == 0x0D else '', s.nss[ns], s.strs[nm] if nm else '*'))
            elif k in (0x0F, 0x10): nm = r.u30(); s.mn.append('RTQName(%s)' % s.strs[nm])
            elif k in (0x11, 0x12): s.mn.append('RTQNameL')
            elif k in (0x09, 0x0E): nm = r.u30(); ns = r.u30(); s.mn.append('%s{%s}::%s' % ('@' if k == 0x0E else '', s.nsset_str(ns), s.strs[nm] if nm else '*'))
            elif k in (0x1B, 0x1C): ns = r.u30(); s.mn.append('%sMultinameL{%s}' % ('@' if k == 0x1C else '', s.nsset_str(ns)))
            elif k == 0x1D:
                t = r.u30(); c = r.u30(); ps = [r.u30() for _ in range(c)]
                s.mn.append('%s.<%s>' % (s.mn[t] if t < len(s.mn) else t, ','.join(str(x) for x in ps)))
            else: raise Exception('mn kind %x' % k)
        # fix up typename params
        s.methods = []
        for _ in range(r.u30()):
            pc = r.u30(); rt = r.u30(); pt = [r.u30() for _ in range(pc)]; nm = r.u30(); fl = r.u8()
            opts = []
            if fl & 0x08:
                for _ in range(r.u30()): opts.append((r.u30(), r.u8()))
            pn = [r.u30() for _ in range(pc)] if fl & 0x80 else []
            s.methods.append(dict(pc=pc, rt=rt, pt=pt, name=s.strs[nm] if nm else '', flags=fl, opts=opts, pn=pn, body=None, owner=''))
        s.meta = []
        for _ in range(r.u30()):
            nm = r.u30(); c = r.u30(); ks = [r.u30() for _ in range(c)]; vs = [r.u30() for _ in range(c)]
            s.meta.append((s.strs[nm], [(s.strs[k] if k else '', s.strs[v]) for k, v in zip(ks, vs)]))
        nc = r.u30(); s.inst = []
        for _ in range(nc):
            nm = r.u30(); sup = r.u30(); fl = r.u8(); pns = r.u30() if fl & 0x08 else 0
            ifs = [r.u30() for _ in range(r.u30())]; ii = r.u30()
            s.inst.append(dict(name=nm, sup=sup, flags=fl, ifs=ifs, iinit=ii, traits=s.traits()))
        s.cls = [dict(cinit=r.u30(), traits=s.traits()) for _ in range(nc)]
        s.scripts = [dict(init=r.u30(), traits=s.traits()) for _ in range(r.u30())]
        for _ in range(r.u30()):
            m = r.u30(); ms = r.u30(); nl = r.u30(); isd = r.u30(); msd = r.u30(); cl = r.u30()
            code = b[r.p:r.p + cl]; r.p += cl
            ex = [(r.u30(), r.u30(), r.u30(), r.u30(), r.u30()) for _ in range(r.u30())]
            tr = s.traits()
            s.methods[m]['body'] = dict(ms=ms, nl=nl, isd=isd, msd=msd, code=code, ex=ex, traits=tr)
    def nsset_str(s, i):
        l = [s.nss[x] for x in s.nssets[i]]
        has_pub = 'pkg:' in l
        return ','.join(l) if os.environ.get('NSFULL') else (('pub+' if has_pub else 'NOPUB+') + str(len(l)) if len(l) > 3 else ','.join(l))
    def traits(s):
        r = s.r; out = []
        for _ in range(r.u30()):
            nm = r.u30(); k = r.u8(); kind = k & 15; t = dict(name=nm, kind=kind, attr=k >> 4)
            if kind in (0, 6): t.update(slot=r.u30(), type=r.u30(), vi=r.u30()); t['vk'] = r.u8() if t['vi'] else 0
            elif kind in (4,): t.update(slot=r.u30(), cls=r.u30())
            elif kind in (5,): t.update(slot=r.u30(), fn=r.u30())
            else: t.update(disp=r.u30(), m=r.u30())
            if k >> 4 & 4: t['meta'] = [r.u30() for _ in range(r.u30())]
            out.append(t)
        return out

OPS = {}
def op(code, name, *args): OPS[code] = (name, args)
for c, n in [(0x01,'bkpt'),(0x02,'nop'),(0x03,'throw'),(0x07,'dxnslate'),(0x09,'label'),(0x1C,'pushwith'),(0x1D,'popscope'),(0x1E,'nextname'),(0x1F,'hasnext'),(0x20,'pushnull'),(0x21,'pushundefined'),(0x23,'nextvalue'),(0x26,'pushtrue'),(0x27,'pushfalse'),(0x28,'pushnan'),(0x29,'pop'),(0x2A,'dup'),(0x2B,'swap'),(0x30,'pushscope'),(0x47,'returnvoid'),(0x48,'returnvalue'),(0x57,'newactivation'),(0x64,'getglobalscope'),(0x70,'convert_s'),(0x71,'esc_xelem'),(0x72,'esc_xattr'),(0x73,'convert_i'),(0x74,'convert_u'),(0x75,'convert_d'),(0x76,'convert_b'),(0x77,'convert_o'),(0x78,'checkfilter'),(0x81,'coerce_b'),(0x82,'coerce_a'),(0x83,'coerce_i'),(0x84,'coerce_d'),(0x85,'coerce_s'),(0x87,'astypelate'),(0x88,'coerce_u'),(0x89,'coerce_o'),(0x90,'negate'),(0x91,'increment'),(0x93,'decrement'),(0x95,'typeof'),(0x96,'not'),(0x97,'bitnot'),(0xA0,'add'),(0xA1,'subtract'),(0xA2,'multiply'),(0xA3,'divide'),(0xA4,'modulo'),(0xA5,'lshift'),(0xA6,'rshift'),(0xA7,'urshift'),(0xA8,'bitand'),(0xA9,'bitor'),(0xAA,'bitxor'),(0xAB,'equals'),(0xAC,'strictequals'),(0xAD,'lessthan'),(0xAE,'lessequals'),(0xAF,'greaterthan'),(0xB0,'greaterequals'),(0xB1,'instanceof'),(0xB3,'istypelate'),(0xB4,'in'),(0xC0,'increment_i'),(0xC1,'decrement_i'),(0xC4,'negate_i'),(0xC5,'add_i'),(0xC6,'subtract_i'),(0xC7,'multiply_i'),(0xD0,'getlocal0'),(0xD1,'getlocal1'),(0xD2,'getlocal2'),(0xD3,'getlocal3'),(0xD4,'setlocal0'),(0xD5,'setlocal1'),(0xD6,'setlocal2'),(0xD7,'setlocal3'),(0x35,'li8'),(0x36,'li16'),(0x37,'li32'),(0x38,'lf32'),(0x39,'lf64'),(0x3A,'si8'),(0x3B,'si16'),(0x3C,'si32'),(0x3D,'sf32'),(0x3E,'sf64'),(0x50,'sxi1'),(0x51,'sxi8'),(0x52,'sxi16')]:
    op(c, n)
for c, n in [(0x0C,'ifnlt'),(0x0D,'ifnle'),(0x0E,'ifngt'),(0x0F,'ifnge'),(0x10,'jump'),(0x11,'iftrue'),(0x12,'iffalse'),(0x13,'ifeq'),(0x14,'ifne'),(0x15,'iflt'),(0x16,'ifle'),(0x17,'ifgt'),(0x18,'ifge'),(0x19,'ifstricteq'),(0x1A,'ifstrictne')]:
    op(c, n, 'off')
for c, n, a in [(0x04,'getsuper','mn'),(0x05,'setsuper','mn'),(0x06,'dxns','str'),(0x08,'kill','u'),(0x24,'pushbyte','b'),(0x25,'pushshort','u'),(0x2C,'pushstring','str'),(0x2D,'pushint','int'),(0x2E,'pushuint','uint'),(0x2F,'pushdouble','dbl'),(0x31,'pushnamespace','ns'),(0x40,'newfunction','meth'),(0x41,'call','u'),(0x42,'construct','u'),(0x49,'constructsuper','u'),(0x53,'applytype','u'),(0x55,'newobject','u'),(0x56,'newarray','u'),(0x58,'newclass','cls'),(0x59,'getdescendants','mn'),(0x5A,'newcatch','u'),(0x5D,'findpropstrict','mn'),(0x5E,'findproperty','mn'),(0x5F,'finddef','mn'),(0x60,'getlex','mn'),(0x61,'setproperty','mn'),(0x62,'getlocal','u'),(0x63,'setlocal','u'),(0x65,'getscopeobject','u'),(0x66,'getproperty','mn'),(0x68,'initproperty','mn'),(0x6A,'deleteproperty','mn'),(0x6C,'getslot','u'),(0x6D,'setslot','u'),(0x6E,'getglobalslot','u'),(0x6F,'setglobalslot','u'),(0x80,'coerce','mn'),(0x86,'astype','mn'),(0x92,'inclocal','u'),(0x94,'declocal','u'),(0xB2,'istype','mn'),(0xC2,'inclocal_i','u'),(0xC3,'declocal_i','u'),(0xF0,'debugline','u'),(0xF1,'debugfile','str')]:
    op(c, n, a)
for c, n in [(0x43,'callmethod'),(0x44,'callstatic'),(0x45,'callsuper'),(0x46,'callproperty'),(0x4A,'constructprop'),(0x4C,'callproplex'),(0x4E,'callsupervoid'),(0x4F,'callpropvoid')]:
    op(c, n, 'mn', 'u')
op(0x32, 'hasnext2', 'u', 'u'); op(0xEF, 'debug', 'b', 'str', 'b', 'u')

def disasm(a, code, ind='    '):
    r = R(code); out = []
    while r.p < len(code):
        at = r.p; c = r.u8()
        if c == 0x1B:
            d = r.s24(); n = r.u30(); cs = [r.s24() for _ in range(n + 1)]
            out.append('%s%4d lookupswitch default=%d cases=%s' % (ind, at, at + d, [at + x for x in cs])); continue
        name, args = OPS.get(c, ('op_%02x' % c, ()))
        ps = []
        for t in args:
            if t == 'off': v = r.s24(); ps.append('->%d' % (r.p + v))
            elif t == 'b': ps.append(str(int.from_bytes(bytes([r.u8()]), 'little', signed=True)))
            elif t == 'u': ps.append(str(r.u30()))
            elif t == 'mn': v = r.u30(); ps.append(a.mn[v] if v < len(a.mn) else '?%d' % v)
            elif t == 'str': v = r.u30(); ps.append(repr(a.strs[v]) if v < len(a.strs) else '?%d' % v)
            elif t == 'int': v = r.u30(); ps.append(str(a.ints[v]) if v < len(a.ints) else '?')
            elif t == 'uint': v = r.u30(); ps.append(str(a.uints[v]) if v < len(a.uints) else '?')
            elif t == 'dbl': v = r.u30(); ps.append(repr(a.dbls[v]) if v < len(a.dbls) else '?')
            elif t == 'ns': v = r.u30(); ps.append(a.nss[v] if v < len(a.nss) else '?')
            elif t == 'meth': ps.append('method#%d' % r.u30())
            elif t == 'cls': ps.append('class#%d' % r.u30())
        out.append('%s%4d %s %s' % (ind, at, name, ' '.join(ps)))
    return out

TK = {0: 'var', 1: 'method', 2: 'getter', 3: 'setter', 4: 'class', 5: 'function', 6: 'const'}
def dump_method(a, mi, label, filt):
    m = a.methods[mi]
    sig = '(%s):%s' % (', '.join(a.mn[t] for t in m['pt']), a.mn[m['rt']])
    print('  %s method#%d %s flags=%02x' % (label, mi, sig, m['flags']))
    b = m['body']
    if not b: return
    print('    maxstack=%d locals=%d scope=%d..%d' % (b['ms'], b['nl'], b['isd'], b['msd']))
    for l in disasm(a, b['code']): print(l)
    for e in b['ex']: print('    catch %d-%d -> %d type=%s var=%s' % (e[0], e[1], e[2], a.mn[e[3]], a.mn[e[4]]))

def dump_traits(a, ts, filt):
    for t in ts:
        k = TK.get(t['kind'], t['kind']); nm = a.mn[t['name']]
        if t['kind'] in (0, 6): print('  %s %s : %s' % (k, nm, a.mn[t['type']]))
        elif t['kind'] == 4: print('  class %s (class#%d)' % (nm, t['cls']))
        elif t['kind'] == 5: dump_method(a, t['fn'], 'function ' + nm, filt)
        else: dump_method(a, t['m'], '%s %s%s' % (k, nm, ' final' if t['attr'] & 1 else '') + (' override' if t['attr'] & 2 else ''), filt)

def main():
    filt = sys.argv[2] if len(sys.argv) > 2 else None
    for nm, b in read_swf(sys.argv[1]):
        a = Abc(b)
        print('==== ABC %r' % nm)
        for i, (ins, cl) in enumerate(zip(a.inst, a.cls)):
            cn = a.mn[ins['name']]
            if filt and filt not in cn: continue
            print('class#%d %s extends %s%s flags=%x' % (i, cn, a.mn[ins['sup']], (' implements ' + ','.join(a.mn[x] for x in ins['ifs'])) if ins['ifs'] else '', ins['flags']))
            dump_method(a, ins['iinit'], 'constructor', filt)
            dump_traits(a, ins['traits'], filt)
            dump_method(a, cl['cinit'], 'static init', filt)
            dump_traits(a, cl['traits'], filt)
        if not filt:
            for i, s in enumerate(a.scripts):
                print('script#%d' % i)
                dump_method(a, s['init'], 'init', filt)
                dump_traits(a, s['traits'], filt)
        if os.environ.get('ALLM'):   # every method body by index (closures too)
            for i in range(len(a.methods) if hasattr(a, 'methods') else 0):
                dump_method(a, i, 'method', None)
main()
