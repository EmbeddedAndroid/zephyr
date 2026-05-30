#!/usr/bin/env python3
"""Generate the performance + architecture figures for the bridge report.
All values are MEASURED on hardware (see bench-results/results.md)."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import os

OUT = os.path.dirname(os.path.abspath(__file__))

INK="#1b2733"; ACC="#2563eb"; ACC2="#0ea5e9"; GOOD="#16a34a"; WARN="#d97706"
MUT="#64748b"; LIGHT="#e2e8f0"; RED="#dc2626"
plt.rcParams.update({"font.family":"DejaVu Sans","font.size":10,
    "axes.edgecolor":MUT,"axes.labelcolor":INK,"text.color":INK,
    "xtick.color":INK,"ytick.color":INK})

# MEASURED phase data: (label, frames/s, payload kB/s)
phases = [
    ("Baseline\nclassic CAN", 750, 6.0),
    ("CAN-FD\n64B/BRS", 671, 43.0),
    ("+CRC-16\n+seq-gap", 438, 28.0),
    ("Optimized\nfinal", 1040, 66.5),
]
labels=[p[0] for p in phases]; fps=[p[1] for p in phases]; kbs=[p[2] for p in phases]
colors=[MUT, ACC2, WARN, GOOD]

# Fig 1: payload throughput (headline)
fig, ax = plt.subplots(figsize=(7.0, 3.4))
bars = ax.bar(labels, kbs, color=colors, width=0.62, zorder=3)
ax.set_ylabel("Payload throughput (kB/s)")
ax.set_title("Round-trip CAN payload throughput by phase", fontweight="bold")
ax.grid(axis="y", color=LIGHT, zorder=0); ax.set_axisbelow(True)
for b,v in zip(bars,kbs):
    ax.text(b.get_x()+b.get_width()/2, v+1.2, f"{v:.1f}", ha="center", va="bottom",
            fontweight="bold")
ax.set_ylim(0, max(kbs)*1.20)
for s in ("top","right"): ax.spines[s].set_visible(False)
ax.annotate("11.1x vs baseline", xy=(3,66.5), xytext=(1.4,60),
            fontsize=9, color=GOOD, fontweight="bold",
            arrowprops=dict(arrowstyle="->", color=GOOD))
fig.tight_layout(); fig.savefig(f"{OUT}/fig_payload.png", dpi=200); plt.close(fig)

# Fig 2: frames/s vs payload dual
fig, ax1 = plt.subplots(figsize=(7.0, 3.4))
x=range(len(labels))
ax1.bar([i-0.2 for i in x], fps, width=0.4, color=ACC, zorder=3)
ax1.set_ylabel("Frames/s", color=ACC); ax1.tick_params(axis="y", labelcolor=ACC)
ax1.set_xticks(list(x)); ax1.set_xticklabels(labels)
ax2=ax1.twinx()
ax2.bar([i+0.2 for i in x], kbs, width=0.4, color=GOOD, zorder=3)
ax2.set_ylabel("Payload kB/s", color=GOOD); ax2.tick_params(axis="y", labelcolor=GOOD)
ax1.set_title("Frame rate vs payload throughput", fontweight="bold")
ax1.grid(axis="y", color=LIGHT, zorder=0); ax1.set_axisbelow(True)
ax1.spines["top"].set_visible(False); ax2.spines["top"].set_visible(False)
fig.tight_layout(); fig.savefig(f"{OUT}/fig_dual.png", dpi=200); plt.close(fig)

# Fig 3: optimization build-up (frames/s), MEASURED
opt=[("+CRC slow\n1MHz",438,WARN),("fast CRC\n1MHz",666,ACC2),
     ("2 MHz\n256B",845,ACC),("512B/7rec\n2MHz",1040,GOOD)]
ol=[o[0] for o in opt]; ov=[o[1] for o in opt]; oc=[o[2] for o in opt]
fig, ax = plt.subplots(figsize=(7.0, 3.2))
bars=ax.bar(ol, ov, color=oc, width=0.6, zorder=3)
ax.set_ylabel("Frames/s"); ax.set_title("Optimization round: frames/s build-up", fontweight="bold")
ax.grid(axis="y", color=LIGHT, zorder=0); ax.set_axisbelow(True)
for b,v in zip(bars,ov):
    ax.text(b.get_x()+b.get_width()/2, v+12, str(v), ha="center", va="bottom", fontweight="bold")
ax.set_ylim(0, max(ov)*1.16)
for s in ("top","right"): ax.spines[s].set_visible(False)
fig.tight_layout(); fig.savefig(f"{OUT}/fig_opt.png", dpi=200); plt.close(fig)

# Fig 4: SPI clock ceiling (measured: reliable to 3MHz, collapse at 4+)
clk=[1,2,3,4,6,8]; valid=[1,1,1,0,0,0]
fig, ax = plt.subplots(figsize=(7.0, 2.7))
cols=[GOOD if v else RED for v in valid]
ax.bar([str(c) for c in clk], [1]*len(clk), color=cols, width=0.6, zorder=3)
ax.set_yticks([]); ax.set_xlabel("SPI clock (MHz)")
ax.set_title("SPI-slave reliability vs clock (green = clean, red = collapses)",
             fontweight="bold", fontsize=10)
ax.text(1,0.5,"reliable\ncrc_err=0", ha="center", va="center", color="white",
        fontweight="bold", fontsize=9)
ax.text(4,0.5,"slave FIFO underruns\nvalid blocks = 0", ha="center", va="center",
        color="white", fontweight="bold", fontsize=9)
for s in ("top","right","left"): ax.spines[s].set_visible(False)
fig.tight_layout(); fig.savefig(f"{OUT}/fig_clock.png", dpi=200); plt.close(fig)

# Fig 5: architecture
fig, ax = plt.subplots(figsize=(7.4, 4.5))
ax.set_xlim(0,10); ax.set_ylim(0,10); ax.axis("off")
def box(x,y,w,h,title,lines,fc,ec):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle="round,pad=0.04,rounding_size=0.12",
                 fc=fc,ec=ec,lw=1.6,zorder=2))
    ax.text(x+w/2,y+h-0.32,title,ha="center",va="top",fontweight="bold",fontsize=9.5,zorder=3)
    ax.text(x+w/2,y+h-0.72,"\n".join(lines),ha="center",va="top",fontsize=7.6,
            zorder=3,linespacing=1.5)
def arrow(x1,y1,x2,y2,label,col,off=0.0):
    ax.add_patch(FancyArrowPatch((x1,y1),(x2,y2),arrowstyle="-|>",mutation_scale=14,
                 lw=1.7,color=col,zorder=1))
    ax.text((x1+x2)/2,(y1+y2)/2+off,label,ha="center",va="center",fontsize=7.2,
            color=col,fontweight="bold",bbox=dict(boxstyle="round,pad=0.15",fc="white",ec="none"))
box(0.3,6.4,4.2,3.0,"QCM2290  -  Linux (qcom-distro)",
    ["host-tools/*.py  (pure stdlib)","SPI master via /dev/spidev0.0",
     "  ioctl SPI_IOC_MESSAGE, full duplex","RDY read via libgpiod gpioget",
     "  gpiochip1:70","binascii.crc_hqx  block CRC-16"],"#eef4ff",ACC)
box(5.5,6.4,4.2,3.0,"STM32U585  -  Zephyr 4.4 (mainline)",
    ["apps/can_spi_bridge  firmware","SPI3 SLAVE (SCK PG9 / MISO PG10",
     "  / MOSI PB5 / NSS PG12)","FDCAN1  CAN_MODE_FD|LOOPBACK",
     "RDY drive: PG13 (HIGH=data)","CRC-16 verify/stamp per block"],"#eefbf3",GOOD)
arrow(4.5,8.2,5.5,8.2,"MOSI: inject CAN frames",ACC,0.28)
arrow(5.5,7.5,4.5,7.5,"MISO: looped-back frames",GOOD,-0.28)
arrow(5.5,6.8,4.5,6.8,"RDY (flow control)",WARN,-0.26)
box(0.3,3.4,9.4,2.4,"512-byte SPI block  (fixed size, both directions)",
    ["[0] magic 0xA5  [1] ver  [2] count (0..7)  [3] seq    ...    [-2..-1] CRC-16",
     "record (72 B):  id (LE u32) | len | flags | rsvd[2] | data[0..64]","",
     "MCU slave transfer is BYTE-COUNT driven (not CS) -> block size matches both ends.",
     "RDY high while a packed-but-unclocked block is staged OR frames queued -> lossless."],
    "#fffaf0",WARN)
box(5.5,0.4,4.2,2.4,"CAN-FD loopback (on-chip)",
    ["injected frame -> can_send()","  (FDF | BRS, up to 64 B)",
     "-> internal loopback -> RX filter","-> ship queue -> SPI MISO",
     "no transceiver / no bus wiring"],"#eefbf3",GOOD)
box(0.3,0.4,4.2,2.4,"Build / flash pipeline",
    ["west-builder (docker) -> zephyr.bin","adb push -> incoming.bin",
     "systemd .path watcher -> flash","native openocd over SWD",
     "  (gpiochip1, BOOT0 hold)"],"#eef4ff",ACC)
arrow(2.4,2.8,2.4,3.4,"",MUT); arrow(7.6,2.8,7.6,3.4,"",MUT)
ax.set_title("CAN-FD <-> SPI bridge  -  system architecture", fontweight="bold",
             fontsize=12, pad=8)
fig.tight_layout(); fig.savefig(f"{OUT}/fig_arch.png", dpi=200, bbox_inches="tight"); plt.close(fig)
print("charts written")
