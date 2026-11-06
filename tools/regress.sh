#!/bin/bash
# full regression: from_avmplus + avm2 + avm1, compared to the saved baselines
S=/tmp/claude-0/-home-claude/7e2fcff1-753d-5a69-a06e-221f61c55b3d/scratchpad
cd /home/claude/work/testswf
python3 run_cat.py from_avmplus >/dev/null 2>&1 &
python3 run_ruffle2.py >/dev/null 2>&1 &
wait
python3 run_ruffle1.py >/dev/null 2>&1
echo "avmplus $(grep -c ^PASS cat_from_avmplus_results.txt)  avm2 $(grep -c ^PASS ruffle2_results.txt)  avm1 $(grep -c ^PASS ruffle1_results.txt)"
L=$( { diff <(grep ^PASS $S/cat_av_prev.txt|awk '{print $2}'|sort) <(grep ^PASS cat_from_avmplus_results.txt|awk '{print $2}'|sort) | grep "^<" | sed 's/^/avmplus lost: /'
diff <(grep ^PASS $S/r2_prev.txt|awk '{print $2}'|sort) <(grep ^PASS ruffle2_results.txt|awk '{print $2}'|sort) | grep "^<" | sed 's/^/avm2 lost: /'
[ -f $S/r1_prev.txt ] && diff <(grep ^PASS $S/r1_prev.txt|awk '{print $2}'|sort) <(grep ^PASS ruffle1_results.txt|awk '{print $2}'|sort) | grep "^<" | sed 's/^/avm1 lost: /'; } )
[ -n "$L" ] && { echo "$L"; exit 1; }
if [ "$1" = "save" ]; then cp cat_from_avmplus_results.txt $S/cat_av_prev.txt; cp ruffle2_results.txt $S/r2_prev.txt; cp ruffle1_results.txt $S/r1_prev.txt; fi
exit 0
