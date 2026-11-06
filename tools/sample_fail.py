import os,subprocess,sys,re,random
R='ruffle/tests/tests/swfs/avm1'; H='/home/claude/work/flashnt/flashnt-host'
fails=[l.split()[1] for l in open('ruffle_results.txt') if l.startswith('FAIL') or l.startswith('CRASH')]
random.seed(int(sys.argv[1]) if len(sys.argv)>1 else 1)
for t in random.sample(fails, int(sys.argv[2]) if len(sys.argv)>2 else 10):
    nf=1; toml=os.path.join(R,t,'test.toml')
    if os.path.exists(toml):
        m=re.search(r'num_frames\s*=\s*(\d+)',open(toml).read()); nf=int(m.group(1)) if m else 1
    try:
        r=subprocess.run([H,os.path.join(R,t,'test.swf'),'-n',str(min(nf,2000)),'-e','100000','-o','/tmp/claude-0/-home-claude/7e2fcff1-753d-5a69-a06e-221f61c55b3d/scratchpad'],capture_output=True,text=True,timeout=30,errors='replace')
        got=[l[7:] for l in r.stderr.splitlines() if l.startswith('trace: ')]
    except subprocess.TimeoutExpired: got=['<TIMEOUT>']
    exp=open(os.path.join(R,t,'output.txt'),errors='replace').read().splitlines()
    ver=open(os.path.join(R,t,'test.swf'),'rb').read()[3]
    print(f"=== {t} (v{ver}) got {len(got)} exp {len(exp)}")
    # first differing lines
    n=0
    for i in range(max(len(got),len(exp))):
        g=got[i] if i<len(got) else '<EOF>'; e=exp[i] if i<len(exp) else '<EOF>'
        if g!=e:
            print(f"  L{i+1}: got={g[:70]!r} exp={e[:70]!r}"); n+=1
            if n>=3: break
