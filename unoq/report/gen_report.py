#!/usr/bin/env python3
"""Build the CAN-FD<->SPI bridge PDF report with reportlab. All numbers MEASURED."""
import os
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY
from reportlab.platypus import (BaseDocTemplate, PageTemplate, Frame, Paragraph,
    Spacer, Image, Table, TableStyle, NextPageTemplate, PageBreak, HRFlowable)
from PIL import Image as PILImage

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "CAN-FD-SPI-bridge-report.pdf")

INK=colors.HexColor("#1b2733"); ACC=colors.HexColor("#2563eb")
GOOD=colors.HexColor("#16a34a"); WARN=colors.HexColor("#d97706")
MUT=colors.HexColor("#64748b"); LIGHT=colors.HexColor("#e2e8f0")
PALE=colors.HexColor("#f1f5f9"); PALEB=colors.HexColor("#eef4ff")
GREENBG=colors.HexColor("#dcfce7")

styles = getSampleStyleSheet()
def S(name, **kw):
    base = kw.pop("parent", styles["Normal"]); return ParagraphStyle(name, parent=base, **kw)
body = S("body", fontName="Helvetica", fontSize=9.5, leading=14, textColor=INK, alignment=TA_JUSTIFY, spaceAfter=6)
h1 = S("h1", fontName="Helvetica-Bold", fontSize=15, leading=19, textColor=ACC, spaceBefore=6, spaceAfter=8)
h2 = S("h2", fontName="Helvetica-Bold", fontSize=11.5, leading=15, textColor=INK, spaceBefore=10, spaceAfter=4)
small = S("small", fontName="Helvetica", fontSize=8, leading=11, textColor=MUT)
cap = S("cap", fontName="Helvetica-Oblique", fontSize=8, leading=11, textColor=MUT, alignment=TA_CENTER, spaceBefore=3, spaceAfter=10)
bullet = S("bullet", parent=body, leftIndent=14, spaceAfter=3)

def hr(): return HRFlowable(width="100%", thickness=0.6, color=LIGHT, spaceBefore=4, spaceAfter=8)
def img(name, w_mm):
    p=os.path.join(HERE,name); iw,ih=PILImage.open(p).size; w=w_mm*mm
    return Image(p, width=w, height=w*ih/iw)
def P(t, st=body): return Paragraph(t, st)
def B(t): return Paragraph("•&nbsp;&nbsp;"+t, bullet)

def tbl(rows, header, widths, aligns=None, hi=None):
    data=[header]+rows
    t=Table(data, colWidths=[w*mm for w in widths], repeatRows=1)
    sty=[("BACKGROUND",(0,0),(-1,0),ACC),("TEXTCOLOR",(0,0),(-1,0),colors.white),
        ("FONTNAME",(0,0),(-1,0),"Helvetica-Bold"),("FONTSIZE",(0,0),(-1,-1),8),
        ("FONTNAME",(0,1),(-1,-1),"Helvetica"),("TEXTCOLOR",(0,1),(-1,-1),INK),
        ("ROWBACKGROUNDS",(0,1),(-1,-1),[colors.white,PALE]),("GRID",(0,0),(-1,-1),0.4,LIGHT),
        ("VALIGN",(0,0),(-1,-1),"MIDDLE"),("TOPPADDING",(0,0),(-1,-1),4),
        ("BOTTOMPADDING",(0,0),(-1,-1),4),("LEFTPADDING",(0,0),(-1,-1),5)]
    if aligns:
        for c,a in aligns.items(): sty.append(("ALIGN",(c,0),(c,-1),a))
    if hi is not None:
        sty.append(("BACKGROUND",(0,hi),(-1,hi),GREENBG)); sty.append(("FONTNAME",(0,hi),(-1,hi),"Helvetica-Bold"))
    t.setStyle(TableStyle(sty)); return t

def on_page(canvas, doc):
    canvas.saveState(); w,h=A4
    canvas.setStrokeColor(LIGHT); canvas.setLineWidth(0.6)
    canvas.line(20*mm,h-15*mm,w-20*mm,h-15*mm)
    canvas.setFont("Helvetica",7.5); canvas.setFillColor(MUT)
    canvas.drawString(20*mm,h-13*mm,"Mainline Zephyr CAN-FD - SPI bridge - Arduino UNO Q (STM32U585 / QCM2290)")
    canvas.drawRightString(w-20*mm,h-13*mm,"Engineering report")
    canvas.line(20*mm,15*mm,w-20*mm,15*mm)
    canvas.drawString(20*mm,11*mm,"2026-05-28")
    canvas.drawRightString(w-20*mm,11*mm,"Page %d"%doc.page)
    canvas.restoreState()

def on_cover(canvas, doc):
    canvas.saveState(); w,h=A4
    canvas.setFillColor(ACC); canvas.rect(0,h-90*mm,w,90*mm,fill=1,stroke=0)
    canvas.setFillColor(colors.HexColor("#0ea5e9")); canvas.rect(0,h-93*mm,w,3*mm,fill=1,stroke=0)
    canvas.setFillColor(colors.white); canvas.setFont("Helvetica-Bold",25)
    canvas.drawString(20*mm,h-42*mm,"CAN-FD <-> SPI Bridge")
    canvas.setFont("Helvetica",14); canvas.drawString(20*mm,h-52*mm,"Mainline Zephyr on the Arduino UNO Q")
    canvas.setFont("Helvetica",11); canvas.drawString(20*mm,h-62*mm,"Architecture, implementation, testing & performance")
    canvas.setFont("Helvetica",9.5); canvas.drawString(20*mm,h-74*mm,"STM32U585 (Cortex-M33, Zephyr 4.4)   .   QCM2290 (Qualcomm Linux)")
    canvas.setFillColor(MUT); canvas.setFont("Helvetica",9)
    canvas.drawString(20*mm,22*mm,"Generated 2026-05-28   .   all figures measured on real hardware")
    canvas.restoreState()

frame=Frame(20*mm,18*mm,A4[0]-40*mm,A4[1]-36*mm,id="main")
cover_frame=Frame(20*mm,18*mm,A4[0]-40*mm,A4[1]-130*mm,id="cover")
doc=BaseDocTemplate(OUT,pagesize=A4,title="CAN-FD SPI Bridge Report",author="zephyr-mainline")
doc.addPageTemplates([PageTemplate(id="cover",frames=[cover_frame],onPage=on_cover),
                      PageTemplate(id="main",frames=[frame],onPage=on_page)])
story=[]

# SUMMARY
story.append(NextPageTemplate("main"))
story.append(P("Executive summary", h1))
story.append(P("This report documents a working, bidirectional <b>CAN-FD to SPI bridge</b> built on "
    "<b>mainline Zephyr 4.4</b> running on the Arduino UNO Q's STM32U585 microcontroller, paired "
    "with the board's QCM2290 applications processor running Qualcomm Linux. The MCU runs its "
    "CAN-FD controller in on-chip loopback and exchanges frames with the Linux side over SPI, with "
    "the STM32 as SPI slave and Linux as master via <font name='Courier'>/dev/spidev0.0</font>. A "
    "hardware ready (RDY) line provides lossless flow control.", body))
story.append(P("The solution was built incrementally and measured on real hardware at every step: "
    "from a classic-CAN baseline, CAN-FD (64-byte payloads) was added, then per-block CRC-16 "
    "integrity and sequence-gap detection, then an optimization round. The optimized bridge "
    "sustains <b>1,040 round-trip frames/s and 66.5 kB/s of CAN payload</b> &mdash; an "
    "<b>11.1&times; payload-throughput improvement</b> over the classic-CAN baseline &mdash; while "
    "adding CAN-FD, integrity checking, and flow control.", body))
story.append(hr())
story.append(P("Headline results", h2))
story.append(tbl(
    [["Round-trip frame rate","750 frames/s","1,040 frames/s","1.4x"],
     ["CAN payload throughput","6.0 kB/s","66.5 kB/s","11.1x"],
     ["Max payload per frame","8 bytes","64 bytes (CAN-FD)","8x"],
     ["Integrity checking","none","CRC-16 per block","added"],
     ["Flow control","none","RDY line, lossless","added"]],
    ["Metric","Baseline","Final (optimized)","Gain"],
    [52,38,50,22], aligns={1:"CENTER",2:"CENTER",3:"CENTER"}))
story.append(Spacer(1,4))
story.append(P("All numbers are medians of five timed runs on the physical board; methodology in "
    "the Benchmarking section.", small))

# ARCH
story.append(PageBreak()); story.append(P("1.  System architecture", h1))
story.append(P("The bridge spans two processors on a single board. The <b>QCM2290</b> (Qualcomm "
    "applications processor, Yocto-based Qualcomm Linux) is the SPI <b>master</b> and the system's "
    "northbound interface. The <b>STM32U585</b> (Cortex-M33) runs mainline Zephyr, owns the CAN-FD "
    "peripheral, and acts as SPI <b>slave</b>. They are wired by the board's SPI3 bus plus a "
    "dedicated ready/handshake line.", body))
story.append(img("fig_arch.png", 168))
story.append(P("Figure 1 &mdash; End-to-end architecture: build/flash pipeline, the SPI block "
    "protocol, CAN-FD loopback, and the three signal paths (MOSI inject, MISO return, RDY).", cap))
story.append(P("1.1  MCU side (STM32U585, Zephyr 4.4)", h2))
story.append(B("<b>FDCAN1</b> via Zephyr's CAN API in <font name='Courier'>CAN_MODE_FD | "
    "CAN_MODE_LOOPBACK</font> with bitrate switching (BRS). Enabled by a device-tree overlay "
    "(kernel clock PLL1_Q, pins PD0/PD1). Up to 64 data bytes/frame."))
story.append(B("<b>SPI3 as slave</b> on board-default pins (SCK PG9, MISO PG10, MOSI PB5, NSS "
    "PG12). The QCM master supplies clock + chip-select; the driver sets NSS to hardware-input."))
story.append(B("<b>RDY line</b> on PG13 (= QCM <font name='Courier'>gpiochip1:70</font>), driven "
    "HIGH whenever unread frames are staged or queued for the master &mdash; basis of lossless flow control."))
story.append(B("<b>CRC-16</b> (CCITT-FALSE) in firmware over every block, to stamp outbound and "
    "verify/reject corrupt inbound command blocks."))
story.append(B("Two threads: a CAN-RX thread (loopback frames into a ship queue, raises RDY) and "
    "the SPI thread (per transfer: parse-and-inject inbound, pack-and-stamp outbound)."))
story.append(P("1.2  Linux side (QCM2290, Qualcomm Linux)", h2))
story.append(B("<b>SPI master</b> via <font name='Courier'>/dev/spidev0.0</font> &mdash; a "
    "purpose-built DT node (<font name='Courier'>compatible = \"arduino,unoq-mcu\"</font>) on the "
    "GENI SE. Full-duplex transfers via <font name='Courier'>SPI_IOC_MESSAGE</font>."))
story.append(B("<b>RDY read</b> via libgpiod (<font name='Courier'>gpioget</font>) on "
    "<font name='Courier'>gpiochip1:70</font> so the master clocks transfers only when data is ready."))
story.append(B("<b>Host tooling</b> is pure Python stdlib (the device interpreter ships without "
    "<font name='Courier'>spidev</font> / <font name='Courier'>ctypes</font> / 3rd-party modules). "
    "Block CRC uses C-implemented <font name='Courier'>binascii.crc_hqx</font>, matching the firmware bit-for-bit."))
story.append(P("1.3  The SPI block protocol", h2))
story.append(P("SPI has no message framing and &mdash; critically &mdash; the STM32 slave transfer "
    "is <b>byte-count driven, not chip-select driven</b>: deasserting NSS does not end a transfer "
    "mid-buffer. The protocol uses a <b>fixed-size block</b> both ends agree on exactly, same layout "
    "both directions:", body))
story.append(Table([
    [Paragraph("<font name='Courier' size=8>[0] magic 0xA5  [1] version  [2] count (0..7)  [3] seq  ...  [-2..-1] CRC-16 (LE)</font>", small)],
    [Paragraph("<font name='Courier' size=8>record (72 B):  id (LE u32) | len (0..64) | flags | rsvd[2] | data[0..64]</font>", small)],
], colWidths=[168*mm], style=TableStyle([("BACKGROUND",(0,0),(-1,-1),PALEB),
    ("BOX",(0,0),(-1,-1),0.5,ACC),("TOPPADDING",(0,0),(-1,-1),5),("BOTTOMPADDING",(0,0),(-1,-1),5),
    ("LEFTPADDING",(0,0),(-1,-1),8),("INNERGRID",(0,0),(-1,-1),0.3,LIGHT)])))
story.append(Spacer(1,5))
story.append(B("<b>MOSI</b> (Linux->MCU): command block; MCU injects each record as a CAN-FD frame."))
story.append(B("<b>MISO</b> (MCU->Linux): up to 7 looped-back CAN frames per 512-byte block."))
story.append(B("<b>RDY</b> stays HIGH while a packed-but-unclocked block is staged <i>or</i> frames "
    "remain queued, so the master never stops one read early &mdash; this makes the link lossless under burst."))

# IMPL
story.append(PageBreak()); story.append(P("2.  Implementation & toolchain", h1))
story.append(P("The whole build-and-flash loop runs from a laptop over a single ADB connection (the "
    "device is not on the network). It was engineered to be fast and panic-free &mdash; an early "
    "attempt to run the flasher in a container kernel-panicked the 1.7&nbsp;GB device during image "
    "decompression, so the flasher runs natively.", body))
story.append(P("2.1  Build -> flash pipeline", h2))
story.append(B("<b>west-builder</b> &mdash; Docker-wrapped <font name='Courier'>west build</font> "
    "(Zephyr SDK 1.0.1, bind-mounted workspace) compiles any app for "
    "<font name='Courier'>arduino_uno_q</font>, emitting <font name='Courier'>zephyr.bin</font>."))
story.append(B("<b>ADB push</b> drops the binary on the device; a <b>systemd path-unit watcher</b> "
    "notices the change and runs the flasher automatically."))
story.append(B("<b>Native openocd</b> flashes the STM32 over SWD (bit-banged via "
    "<font name='Courier'>gpiochip1</font>), holding BOOT0 low across reset so the MCU boots the "
    "new image rather than the ROM bootloader."))
story.append(P("2.2  Verification without a serial console", h2))
story.append(P("The board's Zephyr console UART does not surface on the QCM serial path, so "
    "correctness was proven by <b>SWD-readable RAM counters</b>: the firmware exposes globals "
    "(<font name='Courier'>g_frames_injected, g_can_rx, g_frames_packed, g_rx_crc_err</font>, ...) "
    "read live with <font name='Courier'>openocd mdw</font>. This gives an independent, ground-truth "
    "cross-check of every Linux-side measurement.", body))

# WHAT WORKS
story.append(P("3.  What works", h1))
story.append(P("Every capability below was demonstrated on the physical board, not in simulation.", body))
story.append(tbl(
    [["Build / flash / boot pipeline","Working","west build -> adb -> watcher -> SWD flash -> run"],
     ["SPI-slave data path","Working","full-duplex echo verified byte-exact"],
     ["FDCAN on-chip loopback","Working","TX=RX=match counters equal via SWD"],
     ["CAN -> SPI -> Linux (RX)","Working","frames decoded on /dev/spidev0.0"],
     ["Linux -> MCU CAN inject (TX)","Working","MOSI command blocks -> can_send()"],
     ["RDY flow control (lossless)","Working","200 frames > queue depth, 0 lost"],
     ["CAN-FD 64-byte / BRS","Working","64-byte payloads round-trip byte-exact"],
     ["Block CRC-16 integrity","Working","5/5 corrupt rejected, 15/15 good pass"],
     ["Sequence-gap detection","Working","host tracks seq continuity per block"],
     ["Real CAN bus (transceiver)","Not tested","loopback only; no transceiver wired"]],
    ["Capability","Status","Evidence"], [56,24,82], aligns={1:"CENTER"}))

# TESTING
story.append(PageBreak()); story.append(P("4.  Testing methodology", h1))
story.append(P("Each feature was validated by a dedicated test checking an observable, falsifiable "
    "outcome &mdash; and, wherever possible, cross-checked against the MCU's own SWD counters so the "
    "Linux and firmware views must agree.", body))
story.append(P("4.1  Correctness tests", h2))
story.append(B("<b>SPI-slave echo</b>: slave preloads a known banner (A0..BF) and echoes each "
    "received block on the next transfer; master verified the banner, then that block N+1 returned "
    "exactly what block N sent."))
story.append(B("<b>FDCAN loopback</b>: firmware counted transmitted, received and content-matched "
    "frames; all three equal (via SWD), and the program counter confirmed execution from flash."))
story.append(B("<b>Bidirectional + RDY losslessness</b>: 200 frames injected through a ship queue of "
    "depth 64 (loss would occur without flow control). Result: 200 injected, 200 read back, "
    "<b>0 missing, 0 unexpected</b>, cross-checked by equal MCU counters."))
story.append(B("<b>CAN-FD payload</b>: 12 frames with full 64-byte payloads injected and read back "
    "byte-for-byte identical."))
story.append(B("<b>CRC integrity (negative test)</b>: five valid plus five deliberately "
    "CRC-corrupted blocks sent. MCU injected exactly the 15 valid frames and incremented its "
    "CRC-error counter exactly 5 times &mdash; corrupt blocks rejected, never acted on."))
story.append(P("4.2  Why these tests are trustworthy", h2))
story.append(B("<b>Dual-sided accounting</b>: host counts what it sent/received; MCU independently "
    "counts via SWD. A bug on either side shows up as a mismatch."))
story.append(B("<b>Falsifiable thresholds</b>: tests assert exact equality (0 missing / 0 extra / "
    "counters equal), not \"looks plausible\"."))
story.append(B("<b>Integrity verified on the wire</b>: the CRC check is exercised with "
    "intentionally corrupt input, proving it actually rejects bad data."))

# BENCH METHOD
story.append(P("5.  Benchmark methodology", h1))
story.append(P("Throughput was measured by a harness that drives the bridge as hard as possible and "
    "times a fixed amount of work, kept deliberately simple and repeatable so before/after numbers "
    "are comparable.", body))
story.append(B("<b>Metric</b>: sustained round-trip throughput &mdash; each transfer injects frames "
    "(MOSI) and reads looped-back frames (MISO). Reported as frames/s and CAN-payload kB/s."))
story.append(B("<b>Warm-up</b>: ~300 frames of untimed transfers first, to fill the pipeline."))
story.append(B("<b>Timed window</b>: back-to-back full-duplex transfers until a target frame count "
    "is read back, measured with <font name='Courier'>time.monotonic()</font>."))
story.append(B("<b>No artificial sleeps</b>, and in the throughput loop no per-transfer "
    "<font name='Courier'>gpioget</font> (a subprocess fork per transfer would dominate). Flow-control "
    "correctness is proven separately by the RDY-gated loss test."))
story.append(B("<b>Repetition</b>: five runs; the <b>median</b> is reported. Runs were tightly "
    "clustered (typically &lt;1% spread), indicating a stable measurement."))
story.append(B("<b>Integrity during benchmarking</b>: the harness verifies the CRC of every returned "
    "block and tracks sequence continuity. In all reported runs <font name='Courier'>crc_err = 0</font>."))
story.append(P("One honest caveat: the throughput loop saturates the link and samples returned "
    "blocks rather than draining every one under RDY gating, so its sequence counter shows occasional "
    "expected gaps (the host isn't reading every block the MCU produces). That is a property of the "
    "max-rate sampler, not data loss &mdash; losslessness is proven independently by the RDY-gated "
    "test in section 4.1.", small))

# PERF
story.append(PageBreak()); story.append(P("6.  Performance results", h1))
story.append(P("The bridge was benchmarked at four stages: the classic-CAN baseline, after adding "
    "CAN-FD, after adding CRC + sequence-gap detection, and after the optimization round.", body))
story.append(img("fig_payload.png", 150))
story.append(P("Figure 2 &mdash; CAN payload throughput by phase. CAN-FD delivers the largest single "
    "jump (8x data/frame); the optimization round more than doubles it again to 66.5 kB/s.", cap))
story.append(tbl(
    [["Baseline (classic CAN)","8 B","1 MHz","750","6.0","-"],
     ["CAN-FD (64 B / BRS)","64 B","1 MHz","671","43.0","+7.2x"],
     ["+ CRC-16 + seq-gap","64 B","1 MHz","438","28.0","+4.7x"],
     ["Optimized (final)","64 B","2 MHz","1,040","66.5","+11.1x"]],
    ["Phase","Payload","SPI","frames/s","kB/s","vs base"],
    [50,20,18,28,24,26], aligns={1:"CENTER",2:"CENTER",3:"CENTER",4:"CENTER",5:"CENTER"}, hi=4))
story.append(Spacer(1,3))
story.append(P("Table values are medians of five runs. \"vs base\" compares payload kB/s to the "
    "classic-CAN baseline.", small))
story.append(P("6.1  CAN-FD: payload over frame rate", h2))
story.append(P("Enabling CAN-FD slightly lowers raw frame rate (750->671 frames/s) because each "
    "frame rides in a larger SPI block, but it carries 8x the data, so effective payload throughput "
    "rises 7.2x. For any real CAN workload, payload throughput is the metric that matters.", body))
story.append(img("fig_dual.png", 150))
story.append(P("Figure 3 &mdash; Frame rate (blue) versus payload throughput (green).", cap))

# OPT
story.append(PageBreak()); story.append(P("7.  Optimization analysis", h1))
story.append(P("Adding CRC initially cost 35% of throughput &mdash; but profiling showed the cost "
    "was entirely on the host, in a pure-Python CRC loop, not the (cheap) firmware CRC. The "
    "optimization round addressed three bottlenecks in turn:", body))
story.append(img("fig_opt.png", 150))
story.append(P("Figure 4 &mdash; Frame-rate build-up across the three optimizations, from the "
    "un-optimized +CRC low point.", cap))
story.append(B("<b>Fast CRC</b> &mdash; replacing the pure-Python CRC-16 with C-implemented "
    "<font name='Courier'>binascii.crc_hqx</font> (verified to match the firmware bit-for-bit) "
    "recovered almost the entire CRC penalty: 438->666 frames/s (+52%)."))
story.append(B("<b>Higher SPI clock</b> &mdash; raising the bus from 1 to 2 MHz lifted throughput to "
    "845 frames/s (+27%). A clock sweep established the safe ceiling."))
story.append(B("<b>Larger blocks</b> &mdash; moving from 256-byte/3-frame to 512-byte/7-frame blocks "
    "amortizes per-transfer ioctl, CRC and slave-CPU overhead across more frames, reaching the final "
    "1,040 frames/s (+23%)."))
story.append(P("7.1  The SPI clock ceiling", h2))
story.append(P("The STM32 SPI-slave driver runs in interrupt mode (no DMA). A clock sweep showed the "
    "slave keeps up cleanly to ~3 MHz but collapses at 4 MHz and above &mdash; the ISR can no longer "
    "keep the SPI FIFO fed between per-block CPU work, and returned blocks become invalid. The CRC "
    "check correctly flags this rather than silently passing corrupt data. 2 MHz was selected as the "
    "reliable operating point.", body))
story.append(img("fig_clock.png", 150))
story.append(P("Figure 5 &mdash; SPI-slave reliability versus clock. Green = clean (CRC errors = 0); "
    "red = slave underruns, no valid blocks. A DMA slave path is the route to higher clocks.", cap))

# LIMITS
story.append(P("8.  Limitations & future work", h1))
story.append(B("<b>Loopback only</b>: all CAN traffic is on-chip loopback. A real bus needs a CAN "
    "transceiver on PD0/PD1 and a second node; the firmware path is unchanged."))
story.append(B("<b>SPI clock capped at ~3 MHz</b> by the interrupt-mode slave driver. Enabling the "
    "STM32 SPI DMA slave path is the most promising route to higher throughput."))
story.append(B("<b>Test ID coverage</b>: standard IDs and a range of payload sizes are covered; "
    "extended IDs and error/RTR frames are not yet."))
story.append(B("<b>Host tooling is reference-grade Python</b>: a production northbound service (e.g. "
    "a Go SPI daemon exposing SocketCAN or a network API) would replace the benchmark scripts."))
story.append(B("<b>No back-pressure on inject</b>: the Linux->MCU direction assumes the MCU TX "
    "mailbox keeps up; a busy real bus would need TX flow control too."))
story.append(hr())
story.append(P("Appendix &mdash; key source artifacts", h2))
story.append(P("Firmware: <font name='Courier'>apps/can_spi_bridge/</font> (Zephyr app + overlay). "
    "Host tools: <font name='Courier'>host-tools/</font> (bench.py, can_spi_bridge.py, "
    "can_spi_fd_test.py, crc_test.py). Build/flash: <font name='Courier'>west-builder/</font>, "
    "<font name='Courier'>openocd-native/</font>, <font name='Courier'>watcher/</font>. Raw "
    "benchmark log: <font name='Courier'>bench-results/results.md</font>.", small))

doc.build(story)
print("PDF written:", OUT)
