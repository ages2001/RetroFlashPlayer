#!/bin/sh
# Builds src/avm2_builtin.c: library classes written in ActionScript 3.
#
# Needs Java, asc.jar (the AS3 compiler from the Flex SDK, also shipped with Ruffle in
# tools/asc/asc.jar) and a Ruffle checkout: Ruffle's playerglobal sources (minus the
# classes provided here) are compiled into an import library that declares the player API.
#   RUFFLE=/path/to/ruffle ./build.sh
set -e
cd "$(dirname "$0")"
RUFFLE=${RUFFLE:?set RUFFLE to a Ruffle checkout}
ASC=${ASC:-$RUFFLE/tools/asc/asc.jar}
TMP=$(mktemp -d)
cp -r "$RUFFLE/core/src/avm2/globals/." "$TMP/"
python3 order.py
for f in $(cat classes.txt); do
    sed -i "s|^include \"$f\".*|// provided by flashnt|" "$TMP/globals.as"
done
java -cp "$ASC" macromedia.asc.embedding.ScriptCompiler -optimize -builtin -apiversioning -version 9 -outdir "$TMP" -out pgimport "$TMP/Toplevel.as" "$TMP/globals.as" > /dev/null
java -cp "$ASC" macromedia.asc.embedding.ScriptCompiler -optimize -import "$TMP/pgimport.abc" -outdir "$TMP" -out builtin all.as > /dev/null
test -f "$TMP/builtin.abc"
python3 ../tools/bin2c.py "$TMP/builtin.abc" g_avm2_builtin_abc > "$TMP/out.c"
python3 - "$TMP/out.c" <<'PY'
import sys
names = []
for l in open('classes.txt'):
    l = l.strip()
    if not l or l.startswith('#'): continue
    p = l[:-3].split('/'); names.append('.'.join(p[:-1]) + '::' + p[-1])
with open(sys.argv[1], 'a') as f:
    f.write('/* classes these scripts define (the C library leaves them out) */\nconst char *const g_avm2_builtin_names[] = {\n')
    for n in names: f.write('    "%s",\n' % n)
    f.write('    0\n};\n')
PY
mv "$TMP/out.c" ../src/avm2_builtin.c
rm -rf "$TMP"
