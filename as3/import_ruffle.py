#!/usr/bin/env python3
"""import_ruffle.py RUFFLE path...: copy Ruffle playerglobal classes (paths relative to
core/src/avm2/globals) into as3/, dropping Ruffle's stub markers, and list them in classes.txt."""
import sys, re, os
root = os.path.join(sys.argv[1], 'core/src/avm2/globals')
listed = [l.strip() for l in open('classes.txt')] if os.path.exists('classes.txt') else []
for rel in sys.argv[2:]:
    s = open(os.path.join(root, rel), encoding='utf-8-sig').read()
    s = re.sub(r'^\s*import __ruffle__\.[\w.]+;\s*\n', '', s, flags=re.M)
    s = re.sub(r'^\s*stub_\w+\([^;]*\);\s*\n', '', s, flags=re.M)
    if os.path.dirname(rel): os.makedirs(os.path.dirname(rel), exist_ok=True)
    open(rel, 'w').write('// From Ruffle (core/src/avm2/globals), MIT/Apache-2.0.\n' + s)
    if rel not in listed: listed.append(rel)
open('classes.txt', 'w').write(''.join(l + '\n' for l in listed if l))
