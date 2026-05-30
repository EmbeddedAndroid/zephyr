#!/usr/bin/env python3
# CAN-FD bidirectional correctness test: inject N FD frames with 64-byte
# payloads, read them all back, verify exact data round-trip. RDY-gated drain.
import array, fcntl, struct, time, sys, subprocess, os
SPI_IOC_MAGIC = ord('k')
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
def WR_MODE(): return _IOC(1,SPI_IOC_MAGIC,1,1)
def WR_BITS(): return _IOC(1,SPI_IOC_MAGIC,3,1)
def WR_SPEED():return _IOC(1,SPI_IOC_MAGIC,4,4)
XFMT="QQIIHBBBBBB"; XSZ=32
def MSG(n): return _IOC(1,SPI_IOC_MAGIC,0,XSZ*n)
BLOCK=256; REC=72; MAXR=3; MAGIC=0xA5; VER=0x02; PAYLOAD=64
OO="/home/root/zephyr-flash/oo"
def rdy():
    env=dict(os.environ, LD_LIBRARY_PATH=OO+"/lib")
    try:
        o=subprocess.check_output([OO+"/bin/gpioget","-c","/dev/gpiochip1","70"],env=env,stderr=subprocess.STDOUT).decode()
    except Exception: return -1
    return 0 if "inactive" in o else (1 if "active" in o else 0)
def xfer(fd,sp,txb,rxb):
    ta,_=txb.buffer_info(); ra,_=rxb.buffer_info()
    fcntl.ioctl(fd, MSG(1), struct.pack(XFMT,ta,ra,BLOCK,sp,0,8,0,0,0,0,0))
def mblk(seq,frames):
    b=bytearray(BLOCK); b[0]=MAGIC;b[1]=VER;b[2]=len(frames);b[3]=seq&0xff
    for i,(cid,data) in enumerate(frames[:MAXR]):
        r=4+i*REC
        b[r]=cid&0xff;b[r+1]=(cid>>8)&0xff;b[r+2]=(cid>>16)&0xff;b[r+3]=(cid>>24)&0xff
        b[r+4]=len(data);b[r+5]=0
        b[r+8:r+8+len(data)]=data[:64]
    return bytes(b)
def parse(b):
    if b[0]!=MAGIC: return None
    c=b[2]; out=[]
    for i in range(min(c,MAXR)):
        r=b[4+i*REC:4+(i+1)*REC]
        cid=r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24); n=r[4]
        out.append((cid, bytes(r[8:8+min(n,64)])))
    return out
def main():
    N=int(sys.argv[1]) if len(sys.argv)>1 else 12
    dev=sys.argv[2] if len(sys.argv)>2 else "/dev/spidev0.0"; sp=int(sys.argv[3]) if len(sys.argv)>3 else 1000000
    f=open(dev,"r+b",buffering=0); fd=f.fileno()
    fcntl.ioctl(fd,WR_MODE(),struct.pack("B",0));fcntl.ioctl(fd,WR_BITS(),struct.pack("B",8));fcntl.ioctl(fd,WR_SPEED(),struct.pack("I",sp))
    want=[]
    for i in range(N):
        data=bytes(((i+k)&0xff) for k in range(64))   # full 64-byte payload
        want.append((0x200+i, data))
    rxb=array.array("B",bytes(BLOCK)); got=[]
    def collect(): 
        p=parse(rxb)
        if p: got.extend(p)
    seq=0; sent=0
    while sent<len(want):
        batch=want[sent:sent+MAXR]; txb=array.array("B",mblk(seq,batch))
        xfer(fd,sp,txb,rxb); collect(); sent+=len(batch); seq+=1; time.sleep(0.003)
    time.sleep(0.05)
    z=array.array("B",bytes(BLOCK)); idle=0
    for _ in range(300):
        if rdy()<=0:
            idle+=1
            if idle>=3: break
            time.sleep(0.01); continue
        idle=0; xfer(fd,sp,z,rxb); collect(); time.sleep(0.002)
    ws=set((c,d) for c,d in want); gs=set(got)
    miss=ws-gs; extra=gs-ws
    print("FD test: injected %d (64B each), read %d, missing %d, extra %d" % (len(ws),len(gs),len(miss),len(extra)))
    # verify a sample payload fully
    if got:
        c,d=got[0]; print("sample frame id=0x%x len=%d data[0:8]=%s data[56:64]=%s" % (c,len(d),d[:8].hex(),d[56:64].hex()))
    print("PASS: full 64-byte FD frames round-tripped, no loss" if not miss and not extra else "FAIL")
main()
