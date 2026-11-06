import sys, subprocess, time
# map chars to qemu sendkey names
m={'\\':'backslash',':':'shift-semicolon','.':'dot',' ':'spc','_':'shift-minus','-':'minus','/':'slash'}
keys=[]
for ch in sys.argv[1]:
    if ch.isalpha():
        keys.append(('shift-' if ch.isupper() else '')+ch.lower())
    elif ch.isdigit(): keys.append(ch)
    else: keys.append(m[ch])
cmds=['sendkey '+k for k in keys]
subprocess.run(['python3','-I','qmon.py']+cmds)
