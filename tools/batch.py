import subprocess, os, sys, zlib, struct, time
H='/home/claude/work/flashnt/flashnt-host'
rows=[l.split('\t') for l in open('shumway_avm1.txt').read().splitlines()]
res=[]
for p,ver,sz in rows:
    name=os.path.basename(os.path.dirname(p))+'_'+os.path.basename(p)[:-4]
    out='shots/'+name; os.makedirs(out,exist_ok=True)
    t=time.time()
    try:
        r=subprocess.run([H,p,'-n','60','-e','59','-o',out],capture_output=True,text=True,timeout=90,errors='replace')
        status='ok' if r.returncode==0 else f'exit{r.returncode}'
    except subprocess.TimeoutExpired: status='timeout'
    res.append((p,ver,sz,status,round(time.time()-t,1)))
    print(p,ver,status,round(time.time()-t,1),flush=True)
open('shumway_results.txt','w').write('\n'.join('\t'.join(map(str,r)) for r in res))
from collections import Counter
print(Counter(r[3] for r in res))
