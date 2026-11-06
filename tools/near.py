import os,subprocess,re
R='ruffle/tests/tests/swfs/avm1'; H='/home/claude/work/flashnt/flashnt-host'
fails=[l.split()[1] for l in open('ruffle_results.txt') if l.startswith('FAIL')]
out=[]
for t in fails:
    nf=1; toml=os.path.join(R,t,'test.toml')
    if os.path.exists(toml):
        m=re.search(r'num_frames\s*=\s*(\d+)',open(toml).read()); nf=int(m.group(1)) if m else 1
    try:
        r=subprocess.run([H,os.path.join(R,t,'test.swf'),'-n',str(min(nf,2000)),'-e','100000','-o','/tmp/claude-0/-home-claude/7e2fcff1-753d-5a69-a06e-221f61c55b3d/scratchpad'],capture_output=True,text=True,timeout=30,errors='replace')
        got=[l[7:] for l in r.stderr.splitlines() if l.startswith('trace: ')]
    except subprocess.TimeoutExpired: continue
    exp=open(os.path.join(R,t,'output.txt'),errors='replace').read().splitlines()
    if len(got)!=len(exp): continue
    d=[(i,got[i],exp[i]) for i in range(len(exp)) if got[i]!=exp[i]]
    if len(d)<=3: out.append((t,d))
for t,d in out:
    print("==",t)
    for i,g,e in d: print(f"   L{i+1}: {g[:60]!r} != {e[:60]!r}")
