#!/usr/bin/env python3
"""N2K report figures. All values MEASURED (bench-results/n2k-results.md)."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import os
OUT = os.path.dirname(os.path.abspath(__file__))
INK="#1b2733"; ACC="#2563eb"; ACC2="#0ea5e9"; GOOD="#16a34a"; WARN="#d97706"
MUT="#64748b"; LIGHT="#e2e8f0"; NAVY="#0f3d6b"; RED="#dc2626"
plt.rcParams.update({"font.family":"DejaVu Sans","font.size":10,
    "axes.edgecolor":MUT,"axes.labelcolor":INK,"text.color":INK,
    "xtick.color":INK,"ytick.color":INK})

# Fig 1: throughput vs injection pace (MEASURED sweep), showing the loss wall
pace=[2.0,1.0,0.6,0.5,0.4,0.3,0.2]
fps =[411,698,970,1074,1204,1175,756]
lossless=[1,1,1,1,1,0,0]   # 0.3ms is loss onset (585/600), 0.2 heavy loss
fig, ax = plt.subplots(figsize=(7.0,3.6))
xs=range(len(pace))
# split into lossless / lossy segments visually
gcols=[GOOD if l else RED for l in lossless]
ax.plot(xs, fps, "-", color=MUT, lw=1.4, zorder=2)
for i in xs:
    ax.plot(i, fps[i], "o", color=(GOOD if lossless[i] else RED), markersize=8, zorder=3)
ax.axhline(1953, color=ACC2, ls="--", lw=1.6, label="250 kbit/s bus ceiling (~1,953 fps)")
ax.axvspan(4.5, 6.5, color="#fee2e2", zorder=0)   # loss region
ax.set_xticks(list(xs)); ax.set_xticklabels(["%.1f"%p for p in pace])
ax.set_xlabel("Injection pace (ms/frame)  ->  faster")
ax.set_ylabel("Frames/s (round-trip)")
ax.set_title("N2K throughput vs injection pace (green=lossless, red=loss)", fontweight="bold")
ax.grid(color=LIGHT, zorder=0); ax.set_axisbelow(True); ax.set_ylim(0, 2100)
ax.legend(fontsize=8, loc="upper center")
ax.annotate("best lossless\n1,204 fps @0.4ms", xy=(4,1204), xytext=(1.3,1500),
            fontsize=8, color=GOOD, fontweight="bold",
            arrowprops=dict(arrowstyle="->", color=GOOD))
ax.annotate("loss wall\n(~0.3 ms)", xy=(5,1175), xytext=(5.0,500),
            fontsize=8, color=RED, fontweight="bold",
            arrowprops=dict(arrowstyle="->", color=RED))
for s in ("top","right"): ax.spines[s].set_visible(False)
fig.tight_layout(); fig.savefig(f"{OUT}/n2k_throughput.png", dpi=200); plt.close(fig)

# Fig 2: FD variant vs N2K variant comparison (config, not a race)
fig, ax = plt.subplots(figsize=(7.0,3.0))
cats=["Bitrate","Max payload\nper frame","Frame format","ID width"]
fig.clf()
ax=fig.add_subplot(111); ax.axis("off")
rows=[
 ["", "CAN-FD variant", "NMEA 2000 variant"],
 ["Standard", "CAN FD + BRS", "Classic CAN 2.0B"],
 ["Bitrate", "500k arb / 2M data", "250 kbit/s"],
 ["Payload/frame", "up to 64 bytes", "up to 8 bytes"],
 ["ID width", "11-bit (tested)", "29-bit extended"],
 ["Limiter", "SPI link (~2 MHz)", "CAN bus (250 kbit/s)"],
 ["Use case", "high-rate internal link", "marine N2K backbone"],
]
tbl=ax.table(cellText=rows, loc="center", cellLoc="left")
tbl.auto_set_font_size(False); tbl.set_fontsize(8.5); tbl.scale(1,1.55)
for (r,c),cell in tbl.get_celld().items():
    cell.set_edgecolor(LIGHT)
    if r==0:
        cell.set_facecolor(NAVY); cell.set_text_props(color="white", fontweight="bold")
    elif c==0:
        cell.set_facecolor("#eef4ff"); cell.set_text_props(fontweight="bold", color=INK)
    else:
        cell.set_facecolor("white" if r%2 else "#f6f9fc")
        if c==2: cell.set_text_props(color=GOOD)
ax.set_title("CAN-FD variant vs NMEA 2000 variant", fontweight="bold", fontsize=11, pad=2)
fig.tight_layout(); fig.savefig(f"{OUT}/n2k_compare.png", dpi=200, bbox_inches="tight"); plt.close(fig)

# Fig 3: architecture (N2K-flavored)
fig, ax = plt.subplots(figsize=(7.4,4.6))
ax.set_xlim(0,10); ax.set_ylim(0,10); ax.axis("off")
def box(x,y,w,h,title,lines,fc,ec):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.04,rounding_size=0.12",
                 fc=fc,ec=ec,lw=1.6,zorder=2))
    ax.text(x+w/2,y+h-0.30,title,ha="center",va="top",fontweight="bold",fontsize=9.3,zorder=3)
    ax.text(x+w/2,y+h-0.68,"\n".join(lines),ha="center",va="top",fontsize=7.5,zorder=3,linespacing=1.5)
def arrow(x1,y1,x2,y2,label,col,off=0.0):
    ax.add_patch(FancyArrowPatch((x1,y1),(x2,y2),arrowstyle="-|>",mutation_scale=14,lw=1.7,color=col,zorder=1))
    ax.text((x1+x2)/2,(y1+y2)/2+off,label,ha="center",va="center",fontsize=7.1,color=col,
            fontweight="bold",bbox=dict(boxstyle="round,pad=0.15",fc="white",ec="none"))
box(0.3,6.5,4.2,3.0,"QCM2290  -  Linux (qcom-distro)",
    ["host tools (pure stdlib)","SPI master /dev/spidev0.0",
     "  SPI_IOC_MESSAGE full duplex","RDY read: libgpiod gpioget",
     "  gpiochip1:70","CRC-16 (binascii.crc_hqx)"],"#eef4ff",ACC)
box(5.5,6.5,4.2,3.0,"STM32U585  -  Zephyr 4.4",
    ["can_spi_bridge_n2k firmware","SPI3 SLAVE (PG9/PG10/PB5/PG12)",
     "FDCAN1 CLASSIC @ 250 kbit/s","  29-bit extended IDs",
     "RDY drive PG13; CRC-16/block","loopback now / NORMAL for bus"],"#eefbf3",GOOD)
arrow(4.5,8.3,5.5,8.3,"MOSI: inject N2K frames",ACC,0.27)
arrow(5.5,7.6,4.5,7.6,"MISO: received frames",GOOD,-0.27)
arrow(5.5,6.9,4.5,6.9,"RDY (flow control)",WARN,-0.25)
box(0.3,3.5,9.4,2.3,"256-byte SPI block  (fixed, both directions, CRC-16 protected)",
    ["[0] magic 0xA5  [1] ver 0x03  [2] count (0..15)  [3] seq   ...   [-2..-1] CRC-16",
     "record (16 B):  id (LE u32, 29-bit) | len (0..8) | flags=IDE | rsvd[2] | data[0..8]",
     "",
     "up to 15 classic frames per block; RDY-gated -> lossless; CRC rejects corrupt blocks."],
    "#fffaf0",WARN)
box(5.5,0.4,4.2,2.4,"NMEA 2000 bus (classic CAN)",
    ["250 kbit/s, 29-bit extended","CAN transceiver on PD0/PD1",
     "  (TJA1051 / MCP2562, 5V)","120 ohm termination + bus power",
     "** transceiver NOT on bench **","(loopback-verified only)"],"#fef2f2","#dc2626")
box(0.3,0.4,4.2,2.4,"Build / flash pipeline",
    ["west-builder (docker) -> .bin","adb push -> watcher -> flash",
     "native openocd over SWD","BENCH_LOOPBACK switch:",
     "  1 = loopback (no HW)","  0 = real bus (NORMAL)"],"#eef4ff",ACC)
arrow(2.4,2.8,2.4,3.5,"",MUT); arrow(7.6,2.8,7.6,3.5,"",MUT)
ax.set_title("NMEA 2000 CAN <-> SPI bridge  -  system architecture", fontweight="bold", fontsize=12, pad=8)
fig.tight_layout(); fig.savefig(f"{OUT}/n2k_arch.png", dpi=200, bbox_inches="tight"); plt.close(fig)
print("n2k charts written")
