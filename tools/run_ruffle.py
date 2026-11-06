import os, subprocess, sys, re, glob
root='ruffle/tests/tests/swfs/avm1'
host='/home/claude/work/flashnt/flashnt-host'
results=[]; passed=0; total=0
limit=int(sys.argv[1]) if len(sys.argv)>1 else 10**9
for d in sorted(os.listdir(root)):
    swf=os.path.join(root,d,'test.swf'); exp=os.path.join(root,d,'output.txt'); toml=os.path.join(root,d,'test.toml')
    if not (os.path.exists(swf) and os.path.exists(exp)): continue
    nf=1
    if os.path.exists(toml):
        m=re.search(r'num_frames\s*=\s*(\d+)', open(toml).read())
        if m: nf=int(m.group(1))
        if 'ignore = true' in open(toml).read() or 'known_failure = true' in open(toml).read(): pass
    if nf>2000: nf=2000
    try:
        r=subprocess.run([host,swf,'-n',str(nf),'-e','100000','-o','/tmp/claude-0/-home-claude/7e2fcff1-753d-5a69-a06e-221f61c55b3d/scratchpad'],capture_output=True,text=True,timeout=60,errors='replace')
        out=[l[7:] for l in r.stderr.splitlines() if l.startswith('trace: ')]
        crashed = r.returncode<0
    except subprocess.TimeoutExpired:
        out=['<TIMEOUT>']; crashed=True
    expected=open(exp,errors='replace').read().splitlines()
    ok=(out==expected) and not crashed
    total+=1; passed+=ok
    results.append((d,ok,crashed,nf,len(out),len(expected)))
    if total>=limit: break
open('ruffle_results.txt','w').write('\n'.join(f"{'PASS' if ok else ('CRASH' if cr else 'FAIL')} {d} frames={nf} got={g} exp={e}" for d,ok,cr,nf,g,e in results))
print(f"passed {passed}/{total} ({100*passed/total:.1f}%)  crashes={sum(1 for r in results if r[2])}")
