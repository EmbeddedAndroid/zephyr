#!/usr/bin/env python3
# NMEA 2000 bridge correctness test: inject N classic CAN frames with 29-bit
# EXTENDED ids (realistic N2K PGNs) + 8-byte payloads, read all back, verify
# exact round-trip. RDY-gated drain. CRC-16 protected (ver 0x03).
import array, fcntl, struct, time, sys, subprocess, os
M=ord('k')
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
def MSG(n): return _IOC(1,M,0,32*n)
def WR(nr): return _IOC(1,M,nr,1)
BLOCK=256; REC=16; MAXR=15; MAGIC=0xA5; VER=0x03; CRC_OFF=254
OO="/home/root/zephyr-flash/oo"
def crc16(b,n): return __import__("binascii").crc_hqx(bytes(b[:n]),0xFFFF)
def rdy():
    env=dict(os.environ, LD_LIBRARY_PATH=OO+"/lib")
    try: o=subprocess.check_output([OO+"/bin/gpioget","-c","/dev/gpiochip1","70"],env=env,stderr=subprocess.STDOUT).decode()
    except Exception: return -1
    return 0 if "inactive" in o else (1 if "active" in o else 0)
def xfer(fd,sp,txb,rxb):
    ta,_=txb.buffer_info(); ra,_=rxb.buffer_info()
    fcntl.ioctl(fd,MSG(1),struct.pack("QQIIHBBBBBB",ta,ra,BLOCK,sp,0,8,0,0,0,0,0))
def mblk(seq,frames):
    b=bytearray(BLOCK); b[0]=MAGIC;b[1]=VER;b[2]=len(frames);b[3]=seq&0xff
    for i,(cid,data) in enumerate(frames[:MAXR]):
        r=4+i*REC
        b[r]=cid&0xff;b[r+1]=(cid>>8)&0xff;b[r+2]=(cid>>16)&0xff;b[r+3]=(cid>>24)&0xff
        b[r+4]=len(data);b[r+5]=0
        b[r+8:r+8+len(data)]=data[:8]
    c=crc16(b,CRC_OFF); b[CRC_OFF]=c&0xff;b[CRC_OFF+1]=(c>>8)&0xff
    return bytes(b)
def parse(b):
    if b[0]!=MAGIC: return None
    if crc16(b,CRC_OFF)!=(b[CRC_OFF]|(b[CRC_OFF+1]<<8)): return None
    c=b[2]; out=[]
    for i in range(min(c,MAXR)):
        r=b[4+i*REC:4+(i+1)*REC]
        cid=(r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24))&0x1FFFFFFF; n=r[4]
        out.append((cid, bytes(r[8:8+min(n,8)])))
    return out
def main():
    N=int(sys.argv[1]) if len(sys.argv)>1 else 30
    dev=sys.argv[2] if len(sys.argv)>2 else "/dev/spidev0.0"; sp=int(sys.argv[3]) if len(sys.argv)>3 else 1000000
    f=open(dev,"r+b",buffering=0); fd=f.fileno()
    fcntl.ioctl(fd,WR(1),struct.pack("B",0));fcntl.ioctl(fd,WR(3),struct.pack("B",8));fcntl.ioctl(fd,_IOC(1,M,4,4),struct.pack("I",sp))
    # realistic-looking N2K extended IDs (priority<<26 | PGN<<8 | src), 29-bit
    want=[]
    for i in range(N):
        cid=(0x18 << 21) | ((0x1F000 + i) << 0)   # arbitrary but distinct 29-bit ids
        cid &= 0x1FFFFFFF
        data=bytes(((0xA0+i+k)&0xff) for k in range(8))
        want.append((cid, data))
    rxb=array.array("B",bytes(BLOCK)); got=[]
    def collect():
        p=parse(rxb)
        if p: got.extend(p)
    # Pace injection to the 250 kbit/s bus: a classic 29-bit frame is ~0.53 ms
    # on the wire, so injecting K frames takes ~K*0.53 ms during which the MCU
    # SPI thread is busy in can_send() and cannot service the next MOSI transfer.
    # Sleep long enough for the MCU to finish injecting + return to transceive.
    FRAME_MS = 0.6
    seq=0; sent=0
    while sent<len(want):
        batch=want[sent:sent+MAXR]; txb=array.array("B",mblk(seq,batch))
        xfer(fd,sp,txb,rxb); collect(); sent+=len(batch); seq+=1
        time.sleep(len(batch)*FRAME_MS/1000.0 + 0.004)
    time.sleep(0.05)
    z=array.array("B",bytes(BLOCK)); idle=0
    for _ in range(300):
        if rdy()<=0:
            idle+=1
            if idle>=3: break
            time.sleep(0.01); continue
        idle=0; xfer(fd,sp,z,rxb); collect(); time.sleep(0.002)
    ws=set(want); gs=set(got); miss=ws-gs; extra=gs-ws
    print("N2K test: injected %d (29-bit ext, 8B), read %d, missing %d, extra %d" % (len(ws),len(gs),len(miss),len(extra)))
    if got:
        c,d=got[0]; print("sample: id=0x%08X (29-bit) data=%s" % (c,d.hex()))
        print("  id fits 29 bits:", c <= 0x1FFFFFFF)
    print("PASS: extended-ID classic CAN frames round-tripped, no loss" if not miss and not extra else "FAIL: "+str(sorted(miss)[:5]))
main()
