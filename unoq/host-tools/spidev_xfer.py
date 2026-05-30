#!/usr/bin/env python3
"""Minimal /dev/spidev full-duplex transfer using raw ioctl (no py-spidev dep).

Usage:
  spidev_xfer.py <dev> <hexbytes> [--speed HZ] [--mode N] [--bits N]
Example:
  spidev_xfer.py /dev/spidev0.0 0102030405 --speed 1000000 --mode 0

Sends the given bytes (MOSI) and prints the simultaneously-received bytes
(MISO) as hex. For the SPI-slave echo test, run it twice: first xfer reads the
slave's preload banner; second xfer reads back what the first xfer sent.
"""
import array, fcntl, struct, sys, argparse

# spidev ioctl numbers (asm-generic). _IOC(dir,type,nr,size); type='k'(0x6b).
SPI_IOC_MAGIC = ord('k')

def _IOC(d, t, nr, size):
    return (d << 30) | (size << 16) | (t << 8) | nr

_IOC_WRITE = 1
_IOC_READ = 2

def SPI_IOC_WR_MODE():        return _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 1, 1)
def SPI_IOC_WR_BITS():        return _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 3, 1)
def SPI_IOC_WR_MAX_SPEED():   return _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 4, 4)
def SPI_IOC_RD_MAX_SPEED():   return _IOC(_IOC_READ,  SPI_IOC_MAGIC, 4, 4)

# struct spi_ioc_transfer is 32 bytes on 64-bit:
# u64 tx_buf; u64 rx_buf; u32 len; u32 speed_hz;
# u16 delay_usecs; u8 bits_per_word; u8 cs_change; u8 tx_nbits; u8 rx_nbits;
# u8 word_delay_usecs; u8 pad;  -> packed to 32 bytes
SPI_IOC_TRANSFER_FMT = "QQIIHBBBBBB"
SPI_IOC_TRANSFER_SIZE = 32

def SPI_IOC_MESSAGE(n):
    return _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 0, SPI_IOC_TRANSFER_SIZE * n)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dev")
    ap.add_argument("hexbytes")
    ap.add_argument("--speed", type=int, default=1000000)
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--bits", type=int, default=8)
    args = ap.parse_args()

    tx = bytes.fromhex(args.hexbytes)
    n = len(tx)
    fd = open(args.dev, "r+b", buffering=0)
    fdno = fd.fileno()

    fcntl.ioctl(fdno, SPI_IOC_WR_MODE(), struct.pack("B", args.mode))
    fcntl.ioctl(fdno, SPI_IOC_WR_BITS(), struct.pack("B", args.bits))
    fcntl.ioctl(fdno, SPI_IOC_WR_MAX_SPEED(), struct.pack("I", args.speed))

    # array buffers stay alive for the ioctl; pass their addresses to the kernel.
    txbuf = array.array("B", tx)
    rxbuf = array.array("B", [0] * n)
    tx_addr, _ = txbuf.buffer_info()
    rx_addr, _ = rxbuf.buffer_info()

    xfer = struct.pack(SPI_IOC_TRANSFER_FMT,
                       tx_addr, rx_addr, n, args.speed,
                       0, args.bits, 0, 0, 0, 0, 0)
    fcntl.ioctl(fdno, SPI_IOC_MESSAGE(1), xfer)

    rx = bytes(rxbuf)
    print("TX:", tx.hex())
    print("RX:", rx.hex())
    fd.close()

if __name__ == "__main__":
    main()
