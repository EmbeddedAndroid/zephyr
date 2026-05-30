#!/usr/bin/env python3
"""NMEA 2000 bridge throughput benchmark (UNO Q, classic CAN @ 250 kbit/s).

Unlike the CAN-FD bench (which is SPI-limited), the N2K link is BUS-LIMITED:
classic CAN at 250 kbit/s caps how fast frames physically move, even in on-chip
loopback (the controller still runs the bit-timing). So this benchmark measures
the *sustained lossless* round-trip rate: inject N extended-ID frames, read all
of them back, verify zero loss, and report frames/s + payload kB/s.

It sweeps the injection pace to find the fastest rate at which nothing is lost
(the practical N2K ceiling), then reports the best lossless run.

Usage: n2k_bench.py [N] [dev] [spi_hz]   (default 600 frames)
"""
import array, fcntl, struct, time, sys, binascii
M=ord('k')
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
def MSG(n): return _IOC(1,M,0,32*n)
def WR(nr): return _IOC(1,M,nr,1)
BLOCK=256; REC=16; MAXR=15; MAGIC=0xA5; VER=0x03; CRC_OFF=254
def crc16(b,n): return binascii.crc_hqx(bytes(b[:n]),0xFFFF)

def xfer(fd,sp,txb,rxb):
    ta,_=txb.buffer_info(); ra,_=rxb.buffer_info()
    fcntl.ioctl(fd,MSG(1),struct.pack("QQIIHBBBBBB",ta,ra,BLOCK,sp,0,8,0,0,0,0,0))
def mblk(seq,frames):
    b=bytearray(BLOCK); b[0]=MAGIC;b[1]=VER;b[2]=len(frames);b[3]=seq&0xff
    for i,(cid,data) in enumerate(frames[:MAXR]):
        r=4+i*REC
        b[r]=cid&0xff;b[r+1]=(cid>>8)&0xff;b[r+2]=(cid>>16)&0xff;b[r+3]=(cid>>24)&0xff
        b[r+4]=len(data);b[r+5]=0; b[r+8:r+8+len(data)]=data[:8]
    c=crc16(b,CRC_OFF); b[CRC_OFF]=c&0xff;b[CRC_OFF+1]=(c>>8)&0xff
    return bytes(b)
def parse(b):
    if b[0]!=MAGIC or crc16(b,CRC_OFF)!=(b[CRC_OFF]|(b[CRC_OFF+1]<<8)): return []
    c=b[2]; out=[]
    for i in range(min(c,MAXR)):
        r=b[4+i*REC:4+(i+1)*REC]
        cid=(r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24))&0x1FFFFFFF
        out.append((cid, bytes(r[8:8+min(r[4],8)])))
    return out

def run(fd, sp, want, frame_ms):
    """Inject all `want` paced at frame_ms/frame, drain, return (read_set, secs)."""
    rxb=array.array("B",bytes(BLOCK)); got=[]
    t0=time.monotonic()
    seq=0; sent=0
    while sent<len(want):
        batch=want[sent:sent+MAXR]
        xfer(fd,sp,array.array("B",mblk(seq,batch)),rxb); got+=parse(rxb)
        sent+=len(batch); seq+=1
        time.sleep(len(batch)*frame_ms/1000.0 + 0.003)
    # drain
    z=array.array("B",bytes(BLOCK)); empty=0
    while empty<5 and len(set(got))<len(want):
        xfer(fd,sp,z,rxb); r=parse(rxb)
        if r: got+=r; empty=0
        else: empty+=1
        time.sleep(0.003)
    return set(got), time.monotonic()-t0

def main():
    N=int(sys.argv[1]) if len(sys.argv)>1 else 600
    dev=sys.argv[2] if len(sys.argv)>2 else "/dev/spidev0.0"
    sp=int(sys.argv[3]) if len(sys.argv)>3 else 1000000
    f=open(dev,"r+b",buffering=0); fd=f.fileno()
    fcntl.ioctl(fd,WR(1),struct.pack("B",0));fcntl.ioctl(fd,WR(3),struct.pack("B",8));fcntl.ioctl(fd,_IOC(1,M,4,4),struct.pack("I",sp))

    want=[]
    for i in range(N):
        cid=((0x18<<21)|(0x1F000+i)) & 0x1FFFFFFF
        want.append((cid, bytes(((i+k)&0xff) for k in range(8))))
    wset=set(want)

    print("N2K throughput benchmark: %d frames, 29-bit ext, 8B payload, classic CAN" % N)
    print("sweeping injection pace to find the lossless ceiling...\n")
    best=None
    for frame_ms in (2.0, 1.0, 0.6, 0.5, 0.45, 0.4, 0.35, 0.3, 0.2):
        got, secs = run(fd, sp, want, frame_ms)
        read=len(got & wset); lost=N-read
        fps=read/secs; kbs=fps*8/1000.0
        tag="LOSSLESS" if lost==0 else ("lost %d"%lost)
        print("  pace %.1f ms/frame: %4d/%d read in %.2fs -> %5.0f frames/s, %4.1f kB/s  [%s]"
              % (frame_ms, read, N, secs, fps, kbs, tag))
        if lost==0:
            best=(frame_ms, fps, kbs, secs)
    print()
    if best:
        print("BEST LOSSLESS: %.0f frames/s | %.1f kB/s payload  (pace %.1f ms/frame)"
              % (best[1], best[2], best[0]))
        # theoretical 250k ceiling for 8-byte extended frames (~128 bits w/ stuffing)
        print("theoretical 250 kbit/s ceiling ~%.0f frames/s for 8-byte ext frames (~128 bit/frame)"
              % (250000/128))
    else:
        print("no fully-lossless run in the swept range")

main()
