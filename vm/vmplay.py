import subprocess, time, sys
def mon(cmds, wait=0.15):
    subprocess.run(['python3','-I','qmon.py']+cmds)
def shot(n): subprocess.run(['python3','-I','qmon.py','shot',n])
def goto(x,y):
    cmds=['mouse_move -2000 -2000']*2
    cmds+=['mouse_move 4 0']*(x//4)+(['mouse_move %d 0'%(x%4)] if x%4 else [])
    cmds+=['mouse_move 0 3']*(y//3)+(['mouse_move 0 %d'%(y%3)] if y%3 else [])
    mon(cmds)
def click(x,y, hover=1.5):
    goto(x,y); time.sleep(hover); mon(['mouse_button 1']); time.sleep(0.3); mon(['mouse_button 0'])
stage=sys.argv[1]
if stage=='skip':
    click(512,392); time.sleep(2); shot('p1')
elif stage=='start':
    click(316,357); time.sleep(3); shot('p2')
elif stage=='story':
    click(176,297); time.sleep(3); shot('p3')
    click(161,207); time.sleep(3); shot('p4')
    click(446,202); time.sleep(3); shot('p5')
elif stage=='play':
    for i in range(6):
        time.sleep(20); shot('p6_%d'%i)
elif stage=='keys':
    mon(['sendkey z 300']); time.sleep(1); mon(['sendkey right 1500']); time.sleep(0.5); shot('p7')
    mon(['sendkey right 1500']); mon(['sendkey z 300']); time.sleep(1); shot('p8')
