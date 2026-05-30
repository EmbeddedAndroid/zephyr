#!/usr/bin/env python3
# CRC negative test: send a GOOD block (valid CRC) then a CORRUPT block (bad CRC).
# Verify via MCU globals: good -> frames_injected rises; corrupt -> rx_crc_err rises, no inject.
import array, fcntl, struct, time, sys
SPI_IOC_MAGIC=ord('k')
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
def MSG(n): return _IOC(1,SPI_IOC_MAGIC,0,32*n)
def WR(nr): return _IOC(1,SPI_IOC_MAGIC,nr,1)
BLOCK=256;REC=72;MAGIC=0xA5;VER=0x03;CRC_OFF=254
def crc16(b,n):
    c=0xFFFF
    for i in range(n):
        c^=b[i]<<8
        for _ in range(8): c=((c<<1)^0x1021)&0xFFFF if c&0x8000 else (c<<1)&0xFFFF
    return c
def blk(seq,n,good=True):
    b=bytearray(BLOCK);b[0]=MAGIC;b[1]=VER;b[2]=n;b[3]=seq
    for i in range(n):
        r=4+i*REC;cid=0x300+i;b[r]=cid&0xff;b[r+1]=(cid>>8)&0xff;b[r+4]=8
        for k in range(8): b[r+8+k]=k
    c=crc16(b,CRC_OFF)
    if not good: c^=0xFFFF   # corrupt the CRC
    b[CRC_OFF]=c&0xff;b[CRC_OFF+1]=(c>>8)&0xff
    return bytes(b)
dev=sys.argv[1] if len(sys.argv)>1 else "/dev/spidev0.0"; sp=1000000
f=open(dev,"r+b",buffering=0);fd=f.fileno()
fcntl.ioctl(fd,WR(1),struct.pack("B",0));fcntl.ioctl(fd,WR(3),struct.pack("B",8));fcntl.ioctl(fd,_IOC(1,SPI_IOC_MAGIC,4,4),struct.pack("I",sp))
rxb=array.array("B",bytes(BLOCK))
def xfer(tx):
    txb=array.array("B",tx);ta,_=txb.buffer_info();ra,_=rxb.buffer_info()
    fcntl.ioctl(fd,MSG(1),struct.pack("QQIIHBBBBBB",ta,ra,BLOCK,sp,0,8,0,0,0,0,0))
# 5 good blocks (3 frames each = 15 injects), then 5 corrupt blocks (0 injects, 5 crc_err)
for s in range(5): xfer(blk(s,3,good=True)); time.sleep(0.005)
for s in range(5,10): xfer(blk(s,3,good=False)); time.sleep(0.005)
print("sent 5 good (15 frames) + 5 corrupt blocks; check MCU g_frames_injected (+15) and g_rx_crc_err (+5)")
