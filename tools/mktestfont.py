#!/usr/bin/env python3
"""mktestfont.py - like mkfonts.py, for the Ruffle test harness default font.

Ruffle runs its tests with Noto Sans as every device font (with_default_font).
This builds src/testfont.c (host build only) from Ruffle's subset, with the
typo metrics ttf-parser reports, so device text measures like Ruffle.
usage: mktestfont.py NotoSans.ttf > src/testfont.c
(NotoSans.ttf: Ruffle's core/assets/notosans.subset.ttf.gz, raw-deflate decoded.)
The outlines and metrics stay in font units (test_fonts_em per EM).

Based on mkfonts.py - build src/builtin_fonts.c: the player's device fonts.

Flash Player draws "device" text (_sans, _serif, _typewriter, Arial, Times
New Roman, ...) with system fonts.  FLASHNT carries its own outlines so text
looks and measures the same on every Windows version (NT 3.1 has neither Arial
nor Times New Roman as TrueType fonts).  The outlines are the Liberation fonts
(SIL Open Font License 1.1, see LICENSE-FONTS.txt), which share the advance
widths of Arial, Times New Roman and Courier New.

Output: a zlib-compressed blob holding every face, decoded once at run time.
Blob layout (little endian):
  u16 nfaces
  per face: u8 family (0 sans, 1 serif, 2 mono), u8 style (1 bold, 2 italic),
            s16 ascent, s16 descent, s16 linegap  (EM = 1024 units)
            u16 nglyphs, u32 glyph_table_offset, u32 kern_offset, u16 nkern
  glyph table (sorted by code): u16 code, u16 advance, u32 outline_offset
  outline: varint ncontours; per contour: varint npoints, then per point
           varint zigzag(dx*2 | on_curve) and varint zigzag(dy), deltas from the
           previous point of the glyph (starting at 0,0); y up, font units
  kern table: u16 left, u16 right, s16 value
usage: mkfonts.py [fontdir] > src/builtin_fonts.c
"""
import sys, struct, zlib
from fontTools.ttLib import TTFont

FONTDIR = '/usr/share/fonts/truetype/liberation'
FACES = [   # italics are drawn as slanted regular/bold faces
    (0, 0, 'LiberationSans-Regular'), (0, 1, 'LiberationSans-Bold'),
    (1, 0, 'LiberationSerif-Regular'), (1, 1, 'LiberationSerif-Bold'),
    (2, 0, 'LiberationMono-Regular'),
]
CODES = list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) + [0x192, 0x2C6, 0x2DC] + \
    list(range(0x384, 0x3CF)) + list(range(0x400, 0x460)) + list(range(0x490, 0x492)) + \
    [0x2002, 0x2003, 0x2009, 0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
     0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x2039, 0x203A, 0x2044, 0x20A4, 0x20A9, 0x20AA, 0x20AC, 0x20BA, 0x20BD, 0x2116, 0x2122,
     0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x221A, 0x221E, 0x2248, 0x2260, 0x2264, 0x2265, 0x25A0, 0x25CF, 0xFB01, 0xFB02]

KSCALE = None   # glyph coordinate scale (None: to a 1024-unit EM)
def zz(v): return (v << 1) if v >= 0 else ((-v) << 1) - 1
def varint(v):
    b = b''
    while v >= 0x80: b += bytes([(v & 0x7F) | 0x80]); v >>= 7
    return b + bytes([v])

def face_blob(path, fam, sty):
    t = TTFont(path)
    cmap = t.getBestCmap(); gs = t.getGlyphSet(); glyf = t['glyf']; hmtx = t['hmtx']
    upem = t['head'].unitsPerEm; k = KSCALE if KSCALE else 1024.0 / upem
    hh = t['hhea']; os2 = t['OS/2']
    # Flash on Windows uses the usWin metrics for device fonts
    asc = round(os2.usWinAscent * k); desc = round(os2.usWinDescent * k); gap = round(max(0, os2.usWinAscent + os2.usWinDescent - upem) * 0)  # leading: none
    glyphs = []
    for code in CODES:
        gn = cmap.get(code)
        if gn is None: continue
        g = glyf[gn]
        adv = round(hmtx[gn][0] * k)
        contours = []
        if g.numberOfContours > 0:
            coords, ends, flags = g.getCoordinates(glyf)
            start = 0
            for e in ends:
                pts = []
                for i in range(start, e + 1):
                    x, y = coords[i]
                    pts.append((round(x * k), round(y * k), flags[i] & 1))
                start = e + 1
                if len(pts) >= 2: contours.append(pts)
        elif g.numberOfContours < 0:  # composite
            coords, ends, flags = g.getCoordinates(glyf)
            start = 0
            for e in ends:
                pts = [(round(coords[i][0] * k), round(coords[i][1] * k), flags[i] & 1) for i in range(start, e + 1)]
                start = e + 1
                if len(pts) >= 2: contours.append(pts)
        out = varint(len(contours)); px = py = 0
        for c in contours:
            out += varint(len(c))
            for x, y, on in c:
                out += varint(zz(x - px) * 2 | on) + varint(zz(y - py)); px, py = x, y
        glyphs.append((code, adv, out))
    # kerning pairs (legacy 'kern' table, if present) between the subset's glyphs
    kern = []
    if 'kern' in t:
        rev = {}
        for code, *_ in glyphs: rev.setdefault(cmap[code], []).append(code)
        for sub in t['kern'].kernTables:
            for (l, r), v in getattr(sub, 'kernTable', {}).items():
                if l in rev and r in rev and v:
                    for lc in rev[l]:
                        for rc in rev[r]: kern.append((lc, rc, round(v * k)))
    return asc, desc, gap, glyphs, kern

def main_unused():
    faces = [(fam, sty) + face_blob('%s/%s.ttf' % (FONTDIR, nm), fam, sty) for fam, sty, nm in FACES]
    hdr = struct.pack('<H', len(faces)); hdr_size = 2 + len(faces) * (2 + 6 + 2 + 4 + 4 + 2)
    # every face shares one glyph table
    outl = b''; table = b''; base = hdr_size; tsize = len(glyphs) * 8
    for code, adv, o in glyphs:
        table += struct.pack('<HHI', code, adv, base + tsize + len(outl)); outl += o
    kb = b''.join(struct.pack('<HHh', l, r, v) for l, r, v in sorted(kern))
    body = table + outl + kb
    entries = [struct.pack('<BBhhhHIIH', fam, sty, asc, desc, gap, len(glyphs), base, base + tsize + len(outl), len(kern)) for fam, sty, asc, desc, gap, glyphs, kern in faces]
    blob = hdr + b''.join(entries) + body
    z = zlib.compress(blob, 9)
    w = sys.stdout.write
    w('/* builtin_fonts.c - generated by tools/mkfonts.py; do not edit.\n')
    w(' * Device-font outlines from the Liberation fonts 2.1 (SIL Open Font License 1.1,\n')
    w(' * see LICENSE-FONTS.txt): sans/serif/mono, metric-compatible with Arial,\n')
    w(' * Times New Roman and Courier New.  %d faces, %d bytes raw. */\n' % (len(faces), len(blob)))
    w('#include "flashnt.h"\n#include <stddef.h>\n')
    w('const unsigned long builtin_fonts_raw_size = %dUL;\n' % len(blob))
    w('const unsigned long builtin_fonts_z_size = %dUL;\n' % len(z))
    w('const u8 builtin_fonts_z[%d] = {\n' % len(z))
    for i in range(0, len(z), 20):
        w('  ' + ','.join(str(b) for b in z[i:i + 20]) + ',\n')
    w('};\n')
    sys.stderr.write('raw %d, compressed %d bytes\n' % (len(blob), len(z)))


def main_test(path):
    import fontTools.ttLib as T
    global CODES, KSCALE
    t = T.TTFont(path)
    CODES = sorted(c for c in t.getBestCmap() if c < 0x10000)
    os2 = t['OS/2']; upem = t['head'].unitsPerEm; k = 1.0   # font units, as Ruffle reads them (EM = upem)
    KSCALE = 1.0
    asc0, desc0, gap0, glyphs, kern = face_blob(path, 0, 0)
    asc = round(os2.sTypoAscender * k); desc = round(-os2.sTypoDescender * k); gap = round(os2.sTypoLineGap * k)
    faces = [(fam, sty, asc, desc, gap, glyphs, kern) for fam in (0, 1, 2) for sty in (0, 1)]
    hdr = struct.pack('<H', len(faces)); hdr_size = 2 + len(faces) * (2 + 6 + 2 + 4 + 4 + 2)
    body = b''; entries = []
    for fam, sty, asc, desc, gap, glyphs, kern in faces:
        outl = b''; table = b''
        base = hdr_size + len(body)
        tsize = len(glyphs) * 8
        for code, adv, o in glyphs:
            table += struct.pack('<HHI', code, adv, base + tsize + len(outl)); outl += o
        kb = b''.join(struct.pack('<HHh', l, r, v) for l, r, v in sorted(kern))
        entries.append(struct.pack('<BBhhhHIIH', fam, sty, asc, desc, gap, len(glyphs), base, base + tsize + len(outl), len(kern)))
        body += table + outl + kb
    blob = hdr + b''.join(entries) + body
    z = zlib.compress(blob, 9)
    w = sys.stdout.write
    w('/* testfont.c - generated by tools/mktestfont.py; do not edit.\n')
    w(' * Noto Sans (SIL Open Font License 1.1), the default font of Ruffle\'s test\n')
    w(' * harness; used by the host build when FLASHNT_TEST_FONT is set. */\n')
    w('#include "flashnt.h"\n#include <stddef.h>\n')
    w('const int test_fonts_em = %d;   /* units per EM of the outlines and metrics */\n' % upem)
    w('const unsigned long test_fonts_raw_size = %dUL;\n' % len(blob))
    w('const unsigned long test_fonts_z_size = %dUL;\n' % len(z))
    w('const u8 test_fonts_z[%d] = {\n' % len(z))
    for i in range(0, len(z), 20):
        w('  ' + ','.join(str(b) for b in z[i:i + 20]) + ',\n')
    w('};\n')
    sys.stderr.write('raw %d, compressed %d bytes\n' % (len(blob), len(z)))

main_test(sys.argv[1])
