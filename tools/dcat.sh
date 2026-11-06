#!/bin/bash
# dcat.sh <category> <test path> [lines]
d=/home/claude/ruffle-rs/ruffle/tests/tests/swfs/$1/$2
n=$(grep -oP 'num_(frames|ticks)\s*=\s*\K\d+' $d/test.toml 2>/dev/null | head -1); n=${n:-1}
FLASHNT_NOW_MS=981152406000 FLASHNT_TZ_MINUTES=345 FLASHNT_VERSION="LNX 32,0,0,0" timeout 30 /home/claude/work/flashnt/flashnt-host $d/test.swf -n $n -e 100000 -T -o /tmp 2>/tmp/err.txt | true
grep "^trace: " /tmp/err.txt | sed 's/^trace: //' > /tmp/got.txt
grep -v "^trace: \|^frame\|^SWF\|^Action" /tmp/err.txt | head -3
diff /tmp/got.txt $d/output.txt | head -${3:-10}
