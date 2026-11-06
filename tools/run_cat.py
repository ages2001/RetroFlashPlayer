import os, subprocess, sys, re, glob

KEYMAP={'LeftControl':17,'RightControl':17,'LeftShift':16,'RightShift':16,'LeftAlt':18,'RightAlt':18,'Tab':9,'Return':13,'Enter':13,'Shift':16,'Control':17,'Alt':18,'Escape':27,'Space':32,'PageUp':33,'PageDown':34,'End':35,'Home':36,'Left':37,'Up':38,'Right':39,'Down':40,'ArrowLeft':37,'ArrowUp':38,'ArrowRight':39,'ArrowDown':40,'NumLock':144,'NumpadUp':38,'NumpadDown':40,'NumpadLeft':37,'NumpadRight':39,'NumpadHome':36,'NumpadEnd':35,'NumpadPageUp':33,'NumpadPageDown':34,'NumpadInsert':45,'NumpadDelete':46,'ScrollLock':145,'Pause':19,'Insert':45,'Delete':46,'Backspace':8,'CapsLock':20,'F1':112,'F2':113,'F3':114,'F4':115,'F5':116,'F6':117,'F7':118,'F8':119,'F9':120,'F10':121,'F11':122,'F12':123,'Numpad0':96,'Numpad1':97,'Numpad2':98,'Numpad3':99,'Numpad4':100,'Numpad5':101,'Numpad6':102,'Numpad7':103,'Numpad8':104,'Numpad9':105,'Multiply':106,'Plus':107,'Minus':109,'Slash':191,'Semicolon':186,'Equals':187,'Comma':188,'Period':190}
def keycode(k):
    if isinstance(k,dict) and 'Char' in k:
        ch=k['Char']; o=ord(ch.upper()) if ch.isalpha() else ord(ch)
        if ch.isdigit() or ch.isalpha(): return o
        m={';':186,'=':187,',':188,'-':189,'.':190,'/':191,'`':192,'[':219,'\\':220,']':221,"'":222,' ':32,'!':49,'@':50,'#':51,'$':52,'%':53,'^':54,'&':55,'*':56,'(':57,')':48,'_':189,'+':187,'{':219,'}':221,'|':220,':':186,'"':222,'<':188,'>':190,'?':191,'~':192}
        return m.get(ch, o)
    if isinstance(k,dict): return 0
    return KEYMAP.get(k, 0)
def convert_input(src, dst):
    import json
    ev=json.load(open(src)); out=[]; down=0
    for e in ev:
        t=e['type']
        if t=='KeyDown': out.append('K %d 1'%keycode(e['key']))
        elif t=='KeyUp': out.append('K %d 0'%keycode(e['key']))
        elif t=='TextInput':
            cp=e.get('codepoint') or ''
            if cp: out.append('T %d'%ord(cp[0]))
        elif t in ('MouseDown','MouseUp'):
            btn={'Left':0,'Middle':1,'Right':2}.get(e.get('btn','Left'),0)
            if btn==0:
                down=1 if t=='MouseDown' else 0; out.append('M %d %d %d %d'%(e['pos'][0],e['pos'][1],down,e.get('index',0)))
            else: out.append('B %d %d %d %d'%(e['pos'][0],e['pos'][1],btn,1 if t=='MouseDown' else 0))
        elif t=='MouseMove': out.append('M %d %d %d'%(e['pos'][0],e['pos'][1],down))
        elif t=='Wait': out.append('W')
        elif t=='MouseWheel': out.append('H %d'%int(e.get('lines',0)))
        elif t=='SetClipboardText': out.append('C '+e['text'].replace('\\','\\\\').replace('\n','\\n').replace('\r','\\r'))
        elif t=='TextControl': out.append('X '+e['code'])
        elif t=='FocusLost': out.append('F 0')
        elif t=='ImePreedit':
            tx=e.get('text',''); cu=e.get('cursor')
            if cu: bt=tx.encode(); fr=len(bt[:cu[0]].decode('utf-8','ignore')); to=len(bt[:cu[1]].decode('utf-8','ignore'))
            else: fr=to=-1
            out.append('I %d %d %s'%(fr,to,tx))
        elif t=='ImeCommit': out.append('J '+e.get('text',''))
        elif t=='FocusGained': out.append('F 1')
    open(dst,'w',encoding='utf-8').write('\n'.join(out)+'\n')

def max_exec_env(toml):
    import re,os
    if not os.path.exists(toml): return {}
    m=re.search(r'max_execution_duration\s*=\s*\{\s*secs\s*=\s*(\d+)',open(toml).read())
    return {'FLASHNT_MAX_EXEC':m.group(1)} if m else {}
def viewport_env(toml):
    if not os.path.exists(toml): return {}
    m=re.search(r'viewport_dimensions\s*=\s*\{([^}]*)\}',open(toml).read())
    if not m: return {}
    d=dict((k.strip(),v.strip()) for k,v in (x.split('=') for x in m.group(1).split(',') if '=' in x))
    return {'FLASHNT_VIEWPORT':'%s,%s,%s'%(d.get('width','0'),d.get('height','0'),d.get('scale_factor','1'))}
def nl_lines(t):
    l=[x[:-1] if x.endswith('\r') else x for x in t.split('\n')]
    if l and l[-1]=='': l.pop()
    return l
def approx_equal(out,exp,tomltxt):
    import math
    try:
        import tomllib; t=tomllib.loads(tomltxt).get('approximations',{})
    except Exception: return False
    eps=t.get('epsilon',0.0); rel=t.get('max_relative',None); pats=t.get('number_patterns',[])
    if len(out)!=len(exp): return False
    def num(x):
        try: return float(x)
        except: return float('nan')
    bare=t.get('bare_numbers',False)
    def rnum(x):
        x=x.strip() if False else x
        try:
            if x!=x.strip() or x=='': return None
            return float(x)
        except: return None
    for a,e in zip(out,exp):
        if a==e: continue
        if bare:
            fa,fe=rnum(a),rnum(e)
            if fa is not None and fe is not None:
                if math.isnan(fa) and math.isnan(fe): continue
                if fa==fe or abs(fa-fe)<=eps: continue
                r=rel if rel is not None else 2.220446049250313e-16
                if abs(fa-fe)<=r*max(abs(fa),abs(fe)): continue
                return False
        ra,re_=a,e
        for p in pats:
            ng=re.compile(p).groups; gs=list(range(1,ng+1)) if ng else [0]
            ma=[m.group(g) for m in re.finditer(p,ra) for g in gs]; me=[m.group(g) for m in re.finditer(p,re_) for g in gs]
            if len(ma)!=len(me): return False
            for x,y in zip(ma,me):
                fx,fy=num(x),num(y)
                if math.isnan(fx) and math.isnan(fy): continue
                if fx==fy: continue
                if abs(fx-fy)<=eps: continue
                if rel is not None and abs(fx-fy)<=rel*max(abs(fx),abs(fy)): continue
                return False
            def strip(t):
                out=''; last=0
                for m in re.finditer(p,t):
                    for g in gs: out+=t[last:m.start(g)]; last=m.end(g)
                return out+t[last:]
            ra=strip(ra); re_=strip(re_)
        if ra!=re_: return False
    return True


import concurrent.futures, tempfile
cat=sys.argv[1]
base='/home/claude/ruffle-rs/ruffle/tests/tests/swfs/'+cat
host='/home/claude/work/flashnt/flashnt-host'
dirs=[]
for dp,dn,fn in os.walk(base):
    if 'test.toml' in fn and 'output.txt' in fn: dirs.append(dp)
dirs.sort()
def run(dp):
    d=os.path.relpath(dp,base); swf=os.path.join(dp,'test.swf'); exp=os.path.join(dp,'output.txt'); toml=os.path.join(dp,'test.toml')
    tt=open(toml).read()
    if not os.path.exists(swf): return (d,'SKIP',0,0,0)
    if re.search(r'^\s*ignore\s*=\s*true',tt,re.M): return (d,'IGNORED',0,0,0)
    nf=1
    m=re.search(r'num_frames\s*=\s*(\d+)', tt) or re.search(r'num_ticks\s*=\s*(\d+)', tt)
    if m: nf=int(m.group(1))
    if nf>600: nf=600
    tmp=tempfile.mkdtemp(dir='/tmp/claude-0/-home-claude/7e2fcff1-753d-5a69-a06e-221f61c55b3d/scratchpad')
    args=[host,swf,'-n',str(nf),'-e','100000','-T']+(['-L'] if re.search(r'log_fetch\s*=\s*true',tt) else [])+['-o',tmp]
    if os.path.exists(os.path.join(dp,'input.json')):
        convert_input(os.path.join(dp,'input.json'),tmp+'/in.txt'); args+=['-i',tmp+'/in.txt']
    env=dict(os.environ,FLASHNT_NOW_MS='981152406000',FLASHNT_TZ_MINUTES='345',FLASHNT_VERSION='LNX 32,0,0,0',**viewport_env(toml),**max_exec_env(toml),**({'FLASHNT_RUNTIME':'AIR'} if re.search(r'runtime\s*=\s*"AIR"',tt) else {}),**({'FLASHNT_TEST_FONT':'1'} if re.search(r'with_default_font\s*=\s*true',tt) else {}))
    try:
        r=subprocess.run(args,capture_output=True,text=True,timeout=30,env=env,errors='replace')
        out=[l[7:] for l in nl_lines(r.stderr) if l.startswith('trace: ')]
        crashed=r.returncode<0
    except subprocess.TimeoutExpired:
        out=['<TIMEOUT>']; crashed=True
    import shutil; shutil.rmtree(tmp,ignore_errors=True)
    expected=nl_lines(open(exp,errors='replace',newline='').read())
    exp2=os.path.join(dp,'output.ruffle.txt'); expected2=nl_lines(open(exp2,errors='replace',newline='').read()) if os.path.exists(exp2) else None
    ok=(out==expected or (expected2 is not None and out==expected2)) and not crashed
    if not ok and not crashed and '[approximations]' in tt: ok=approx_equal(out,expected,tt)
    kf='known_failure = true' in tt
    return (d,'PASS' if ok else ('CRASH' if crashed else ('KFAIL' if kf else 'FAIL')),nf,len(out),len(expected))
with concurrent.futures.ThreadPoolExecutor(8) as ex: results=list(ex.map(run,dirs))
open('cat_%s_results.txt'%cat.replace('/','_'),'w').write('\n'.join(f"{s} {d} frames={nf} got={g} exp={e}" for d,s,nf,g,e in results))
from collections import Counter
c=Counter(r[1] for r in results); print(cat, dict(c))
