import socket,time,sys,zlib,struct
def mon(cmds, wait=1.0):
    s=socket.socket(socket.AF_UNIX); s.connect('mon.sock'); time.sleep(0.3); s.setblocking(False)
    try: s.recv(65536)
    except: pass
    for c in cmds:
        s.send((c+'\n').encode()); time.sleep(wait)
        try: s.recv(65536)
        except: pass
    s.close()
def shot(name):
    mon(['screendump '+name+'.ppm'],1.5)
    d=open(name+'.ppm','rb').read()
    parts=d.split(b'\n',3); w,h=map(int,parts[1].split()); px=parts[3]
    raw=b''.join(b'\x00'+px[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t,b): return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    open(name+'.png','wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw,6))+chunk(b'IEND',b''))
    print(name, w, h)
if __name__=='__main__':
    if sys.argv[1]=='shot': shot(sys.argv[2])
    else: mon(sys.argv[1:], 0.4)
