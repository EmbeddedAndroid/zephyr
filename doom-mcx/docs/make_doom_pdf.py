#!/usr/bin/env python3
"""Generate the DOOM-on-STM32U585 debugging field report PDF (reportlab)."""
import re
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_LEFT, TA_CENTER, TA_JUSTIFY
from reportlab.platypus import (
    BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, Table, TableStyle,
    Preformatted, NextPageTemplate, PageBreak, KeepTogether, HRFlowable,
)

OUT = "/home/tyler/Dev/claude/zephyr-upstream/doom-mcx/docs/doom-on-stm32u585.pdf"

# ---- palette ----------------------------------------------------------------
INK      = colors.HexColor("#1a1a1a")
ACCENT   = colors.HexColor("#8b1a1a")   # doom red
ACCENT2  = colors.HexColor("#b3541e")   # ember
SLATE    = colors.HexColor("#3b3f46")
RULE     = colors.HexColor("#c9ccd1")
CODEBG   = colors.HexColor("#f4f2ef")
CODEBORD = colors.HexColor("#d9d4cc")
CALLBG   = colors.HexColor("#fbf3ea")
CALLBORD = colors.HexColor("#e0b98c")
WARNBG   = colors.HexColor("#fdeeee")
WARNBORD = colors.HexColor("#e0a3a3")
TBLHEAD  = colors.HexColor("#2b2f36")
TBLALT   = colors.HexColor("#f6f5f3")

def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))

styles = getSampleStyleSheet()
BODY = ParagraphStyle("body", parent=styles["BodyText"], fontName="Helvetica",
                      fontSize=9.5, leading=14, textColor=INK, alignment=TA_JUSTIFY,
                      spaceAfter=6)
BODYL = ParagraphStyle("bodyl", parent=BODY, alignment=TA_LEFT)
H1 = ParagraphStyle("h1", parent=styles["Heading1"], fontName="Helvetica-Bold",
                    fontSize=15, leading=18, textColor=ACCENT, spaceBefore=16,
                    spaceAfter=7)
H2 = ParagraphStyle("h2", parent=styles["Heading2"], fontName="Helvetica-Bold",
                    fontSize=11.5, leading=14, textColor=SLATE, spaceBefore=10,
                    spaceAfter=4)
CODE = ParagraphStyle("code", fontName="Courier", fontSize=8.1, leading=10.4,
                      textColor=colors.HexColor("#23201c"))
CAP = ParagraphStyle("cap", parent=BODY, fontSize=8, textColor=SLATE,
                     alignment=TA_LEFT, spaceBefore=2, spaceAfter=10)
LISTSTYLE = ParagraphStyle("li", parent=BODY, leftIndent=12, bulletIndent=2,
                           spaceAfter=3, alignment=TA_LEFT)
TITLE = ParagraphStyle("title", fontName="Helvetica-Bold", fontSize=30,
                       leading=34, textColor=INK, alignment=TA_LEFT)
SUBT = ParagraphStyle("subt", fontName="Helvetica", fontSize=13, leading=18,
                      textColor=ACCENT, alignment=TA_LEFT)
META = ParagraphStyle("meta", fontName="Helvetica", fontSize=9.5, leading=14,
                      textColor=SLATE, alignment=TA_LEFT)
CALLTITLE = ParagraphStyle("ctitle", fontName="Helvetica-Bold", fontSize=9.5,
                           leading=12, textColor=ACCENT2, spaceAfter=2)
CALLBODY = ParagraphStyle("cbody", parent=BODY, alignment=TA_LEFT, spaceAfter=0,
                          fontSize=9)

story = []

def P(t, style=BODY):
    story.append(Paragraph(t, style))

def H(t, lvl=1):
    story.append(Paragraph(esc(t), H1 if lvl == 1 else H2))

def gap(h=4):
    story.append(Spacer(1, h))

def bullets(items):
    for it in items:
        story.append(Paragraph(it, LISTSTYLE, bulletText=u"•"))

def code(text, caption=None):
    text = text.strip("\n")
    pre = Preformatted(esc(text), CODE)
    t = Table([[pre]], colWidths=[166*mm])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), CODEBG),
        ("BOX", (0, 0), (-1, -1), 0.6, CODEBORD),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("RIGHTPADDING", (0, 0), (-1, -1), 8),
        ("TOPPADDING", (0, 0), (-1, -1), 6),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
        ("LINEBEFORE", (0, 0), (0, -1), 2.4, ACCENT2),
    ]))
    story.append(KeepTogether([t] + ([Paragraph(esc(caption), CAP)] if caption else [Spacer(1, 8)])))

def callout(title, body, warn=False):
    bg = WARNBG if warn else CALLBG
    bd = WARNBORD if warn else CALLBORD
    inner = [Paragraph(esc(title), CALLTITLE)]
    for para in body:
        inner.append(Paragraph(para, CALLBODY))
    t = Table([[inner]], colWidths=[166*mm])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), bg),
        ("BOX", (0, 0), (-1, -1), 0.7, bd),
        ("LEFTPADDING", (0, 0), (-1, -1), 10),
        ("RIGHTPADDING", (0, 0), (-1, -1), 10),
        ("TOPPADDING", (0, 0), (-1, -1), 7),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
        ("LINEBEFORE", (0, 0), (0, -1), 3, bd),
    ]))
    story.append(Spacer(1, 2))
    story.append(KeepTogether(t))
    story.append(Spacer(1, 9))

def datatable(rows, head=True, widths=None):
    ncol = len(rows[0])
    widths = widths or [166*mm/ncol]*ncol
    tdata = [[Paragraph(esc(c), ParagraphStyle("tc", parent=BODY, fontSize=8.4,
              leading=11, alignment=TA_LEFT, textColor=(colors.white if (head and r==0) else INK),
              fontName=("Helvetica-Bold" if (head and r==0) else "Helvetica"), spaceAfter=0))
             for c in row] for r, row in enumerate(rows)]
    t = Table(tdata, colWidths=widths, repeatRows=1 if head else 0)
    sty = [
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 6),
        ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
        ("BOX", (0, 0), (-1, -1), 0.5, RULE),
    ]
    if head:
        sty.append(("BACKGROUND", (0, 0), (-1, 0), TBLHEAD))
        for r in range(2, len(rows), 2):
            sty.append(("BACKGROUND", (0, r), (-1, r), TBLALT))
    t.setStyle(TableStyle(sty))
    story.append(t)
    gap(10)

# ============================ TITLE PAGE =====================================
story.append(Spacer(1, 40))
story.append(Paragraph("Getting DOOM to Run on an<br/>STM32U585", TITLE))
story.append(Spacer(1, 10))
story.append(HRFlowable(width="100%", thickness=2, color=ACCENT, spaceAfter=10))
story.append(Paragraph("A debugging field report", SUBT))
story.append(Spacer(1, 6))
story.append(Paragraph(
    "Porting NXP&rsquo;s Doom-MCX (prBoom) to Zephyr on the Arduino UNO Q: the "
    "constraints, the crashes, and the tools that found them &mdash; native_sim "
    "+ Valgrind for the fast lane, OpenOCD/SWD and GDB for the metal.", META))
story.append(Spacer(1, 60))
story.append(HRFlowable(width="36%", thickness=0.8, color=RULE, spaceAfter=8, hAlign="LEFT"))
story.append(Paragraph(
    "Target&nbsp;&nbsp;STM32U585 (Cortex-M33 @160&nbsp;MHz, 768&nbsp;KB SRAM, 2&nbsp;MB flash)<br/>"
    "Display&nbsp;&nbsp;13&times;8 charlieplex LED matrix (104 px, 8 grey levels)<br/>"
    "Stack&nbsp;&nbsp;Zephyr 4.4.x, picolibc, arm-zephyr-eabi (Zephyr SDK 0.16)<br/>"
    "Audience&nbsp;&nbsp;embedded engineers who like the gory details", META))
story.append(PageBreak())

# ============================ 1. TARGET ======================================
H("1. The target, and why it is tight")
P("DOOM (1993) was written for a 486 with 4&nbsp;MB of RAM and a hard disk holding "
  "a ~4&nbsp;MB WAD. The job here was to make it run on a microcontroller: the "
  "STM32U585 on an Arduino UNO Q, rendering into a 13&times;8 LED matrix and taking "
  "input over the board&rsquo;s SPI bridge. The U585 is a generous MCU, but it is "
  "still an MCU, and three of its numbers define the entire effort:")
datatable([
    ["Resource", "STM32U585", "Consequence for DOOM"],
    ["CPU", "Cortex-M33 @ 160 MHz, FPU, MPU, TrustZone (TZEN=0)", "Fast enough for software 3D at a few fps; FPU unused by the fixed-point engine but handy for the display path."],
    ["SRAM", "768 KB, 0x20000000 - 0x200C0000", "Comfortable for the zone heap + framebuffers once they are sized sanely."],
    ["Flash", "2 MB internal, dual-bank, NO external flash", "The hard wall. The game code AND the WAD must both live in 2 MB."],
], widths=[24*mm, 70*mm, 72*mm])
P("There is no external QSPI/SDRAM on this board, so every classic &lsquo;put the "
  "WAD on a memory-mapped flash&rsquo; trick is off the table. That single fact "
  "drove the port&rsquo;s first major decision and its first class of bugs.")

H("2. Choosing a base port")
P("Two candidates. <b>floppes/stm32doom</b> is a beautiful Chocolate-Doom port, but "
  "it assumes ~8&nbsp;MB of external SDRAM and a USB-stick WAD &mdash; neither exists "
  "on the UNO Q. <b>NXP Doom-MCX</b> is a prBoom derivative already proven on the "
  "FRDM-MCXN947 (also a Cortex-M33, 512&nbsp;KB SRAM), structured as a Zephyr "
  "application with the SDL hooks replaced by four platform functions "
  "(I_FinishUpdate / I_SetPallete / I_ProcessKeyEvents / I_InitGraphics). It bakes "
  "the WAD into flash as a C array and converts the indexed frame to the panel "
  "format line-by-line with no second framebuffer. That is the right shape for an "
  "MCU, so Doom-MCX (GPLv2, prBoom lineage) was the base.")
callout("Why the base choice is also a debugging decision", [
    "Doom-MCX targets Zephyr 4.1.0 and is normally exercised on native_sim with "
    "glibc. We built it on Zephyr 4.4.x with arm picolibc. A port across both a "
    "<b>three-minor-version Zephyr gap</b> and a <b>libc swap</b> means the first "
    "bugs are environmental, not logical &mdash; and they must be cleared before any "
    "DOOM-specific bug is even visible. Establishing a known-good baseline (does it "
    "build and boot the title screen at all?) was step zero."])

# ============================ 3. FLASH GATE ==================================
H("3. The flash gate: 2 MB versus a 4 MB game")
P("The full DOOM1.WAD baked as a C array is roughly 3.84&nbsp;MB of binary &mdash; "
  "it does not fit, full stop. The link failed by ~1.98&nbsp;MB, which also gave a "
  "clean measurement: the engine itself is ~228&nbsp;KB, leaving a WAD budget of "
  "about 1.82&nbsp;MB. The fix was <b>Squashware</b> (fragglet/squashware), a "
  "shrunken but complete shareware WAD. The v1.3 full edition is 1,703,241 bytes "
  "&mdash; it fits, with all nine E1Mx maps intact.")
bullets([
    "<b>Bake the WAD:</b> convert the 1.62&nbsp;MB WAD to a C array "
    "(<font face='Courier'>const unsigned char doom_iwad[1703241]</font>) and "
    "select it via <font face='Courier'>#include \"iwad/squashware.c\"</font> in "
    "doom_iwad.c.",
    "<b>Drop MCUboot for the DOOM image:</b> an A/B layout would halve the usable "
    "flash. DOOM ships as a single image flashed straight to 0x08000000. (The OTA "
    "demo on this board is a separate mode &mdash; mcuboot + a signed app in slot0 "
    "&mdash; that you switch to by reflashing.)",
])
P("Final footprint: <b>FLASH 1,948,000 B (92.9%)</b>, <b>RAM 50.6%</b>. The flash "
  "headroom is thin but real; RAM is comfortable once the framebuffers are sized "
  "correctly (see &sect;10).")

# ============================ 4. TOOLCHAIN ==================================
H("4. Toolchain landmines: Zephyr 4.4 + picolibc")
P("Three environmental failures stood between &lsquo;clones&rsquo; and "
  "&lsquo;boots&rsquo;. None are DOOM bugs; all are the kind of thing that eats an "
  "afternoon if you do not recognise the signature.")
H("4.1  A symbol named gamma", 2)
P("picolibc declares <font face='Courier'>gamma()</font> (the log-gamma function). "
  "prBoom&rsquo;s menu code has an <font face='Courier'>enum</font> member literally "
  "named <font face='Courier'>gamma</font>. The collision is a build break in "
  "m_menu.c. Fix: rename the enumerator to "
  "<font face='Courier'>gamma_item</font> (declaration and use).")
H("4.2  POSIX AEP drags in a struct picolibc does not have", 2)
P("Enabling the full <font face='Courier'>CONFIG_POSIX_API</font> pulls in the "
  "realtime AEP, including <font face='Courier'>posix/options/timer.c</font>, which "
  "references <font face='Courier'>sigev_notify_function</font> / "
  "<font face='Courier'>sigev_notify_attributes</font> &mdash; fields picolibc&rsquo;s "
  "<font face='Courier'>struct sigevent</font> lacks on Zephyr 4.4. DOOM needs only "
  "a handful of libc calls, so the fix is to enable just the libc extension and the "
  "two subsystems it uses, and leave timers/signals off:")
code(
"# DOOM needs free/printf/stat/write, NOT the realtime AEP.\n"
"CONFIG_POSIX_C_LIB_EXT=y\n"
"CONFIG_POSIX_DEVICE_IO=y\n"
"CONFIG_POSIX_FILE_SYSTEM=y\n"
"# CONFIG_POSIX_API is not set\n"
"# CONFIG_POSIX_TIMERS is not set\n"
"# CONFIG_POSIX_SIGNALS is not set")
H("4.3  gettimeofday() does not exist", 2)
P("The engine&rsquo;s tic clock, <font face='Courier'>I_GetTime()</font>, calls "
  "<font face='Courier'>gettimeofday()</font>; picolibc without the AEP does not "
  "provide it. A four-line shim backed by the kernel uptime is enough:")
code(
"int gettimeofday(struct timeval *tv, void *tz) {\n"
"    int64_t ms = k_uptime_get();\n"
"    tv->tv_sec  = ms / 1000;\n"
"    tv->tv_usec = (ms % 1000) * 1000;\n"
"    return 0;\n"
"}")
callout("Keep this shim honest", [
    "I_GetTime() converts to tics as "
    "<font face='Courier'>tv_sec*TICRATE + tv_usec*TICRATE/1e6</font>. If your "
    "gettimeofday() returns a constant, DOOM never advances a tic, the main loop "
    "never calls I_FinishUpdate, and the render thread blocks forever after frame 1. "
    "A frozen screen with a live CPU often means a dead clock, not a dead renderer "
    "&mdash; verify the tic actually increments before you suspect anything else."])

# ============================ 5. TWO SURFACES ===============================
H("5. Two debug surfaces: native_sim versus the metal")
P("The single most useful decision in this port was to keep <b>two</b> build "
  "targets alive at all times and to know which one to reach for:")
datatable([
    ["", "native_sim (x86)", "arduino_uno_q (STM32U585)"],
    ["Cycle time", "seconds: cmake + ninja + run", "~2 min SWD flash per change"],
    ["Memory tooling", "Valgrind, ASan, gdb, core dumps", "SWD peeks, fault registers only"],
    ["Faithful to HW?", "No: x86, SDL display, 4 MB heap", "Yes: the real panel + timing"],
    ["Best for", "logic bugs: map loader, struct casts, OOB", "HW bugs: display, SPI, alignment, timing"],
], widths=[26*mm, 70*mm, 70*mm])
P("The discipline: <b>reproduce on native_sim first.</b> If a bug shows up there, "
  "you get Valgrind and a sub-ten-second loop. Only bugs that genuinely depend on "
  "the silicon (the charlieplex path, SPI slave timing, unaligned access) earn a "
  "flash cycle. Conversely, do not over-trust native_sim: it uses an SDL display at "
  "640&times;360, so it cannot validate the 13&times;8 downscale &mdash; a frame that "
  "looked &lsquo;stuck&rsquo; on native_sim turned out to be a sim-only artifact "
  "that ran fine on the matrix.")

# ============================ 6. THE CRASH ==================================
H("6. The real bug: a WAD the loader cannot read")
P("With the environment clean, the game booted to the title screen, then crashed "
  "hard the moment a level loaded. On hardware it was a fault deep in texture/BSP "
  "setup; the breakthrough was reproducing it on native_sim in seconds:")
code(
"P_GroupLines: Subsector a part of no sector!\n"
"P_GroupLines: Subsector a part of no sector!\n"
"... (repeated) ...\n"
"Segmentation fault (core dumped)        # exit 139")
P("That message is diagnostic gold. Every subsector failing to find its sector "
  "means the seg / sidedef / sector linkage is garbage &mdash; which points straight "
  "at the map loader. The root cause: Doom-MCX&rsquo;s loaders "
  "<b>direct-cast raw on-disk WAD lumps onto the engine&rsquo;s in-memory "
  "structs</b>, instead of converting between the two layouts. That only works if "
  "the WAD has been rewritten ahead of time into the engine&rsquo;s memory format.")
H("6.1  On-disk is not in-memory", 2)
P("The on-disk map lumps are compact arrays of indices and 16-bit coordinates. The "
  "engine&rsquo;s runtime structs are wider and hold <i>resolved</i> pointers and "
  "fixed-point values. Casting one onto the other reads adjacent fields as garbage:")
datatable([
    ["Lump", "On-disk record", "In-memory record", "Conversion"],
    ["VERTEXES", "mapvertex_t: int16 x, y (4 B)", "vertex_t: fixed_t x, y (8 B)",
     "x = SHORT(x) << FRACBITS"],
    ["SEGS", "mapseg_t (12 B): v1,v2,angle,linedef,side,offset (u16)",
     "seg_t: resolved v1/v2, angle_t, fixed_t offset, side/line/sector nums",
     "look up vertices; angle<<16; offset<<FRACBITS; sidenum = ldef->sidenum[side]"],
    ["SIDEDEFS", "30 B: int16 texoff,rowoff; char[8]x3 tex names; int16 sector",
     "side_t: numeric texture ids + sector*",
     "R_CheckTextureNumForName(name); -1 -> 0"],
], widths=[24*mm, 56*mm, 50*mm, 36*mm])
P("The smoking gun was in the original, working <font face='Courier'>doom1.c</font> "
  "shipped with Doom-MCX: its E1M1 <b>SEGS lump has size 0</b>, and the SSECTORS / "
  "NODES are resized. In other words, NXP runs a <i>build-time WAD preprocessor</i> "
  "that rewrites the BSP and vertex lumps into the engine&rsquo;s in-memory layout. "
  "That tool is not in the repository. A stock WAD &mdash; Squashware or otherwise "
  "&mdash; uses the standard on-disk format and therefore crashes the cast-based "
  "loader.")
H("6.2  The fix: convert on load (stock prBoom behaviour)", 2)
P("Rather than reverse-engineer NXP&rsquo;s preprocessor, the loaders were rewritten "
  "to convert on load &mdash; allocate the in-memory array, then resolve each field "
  "&mdash; exactly as upstream prBoom does. Representative of the set "
  "(P_LoadVertexes / P_LoadLineDefs / P_LoadSegs / P_LoadSideDefs):")
code(
"static void P_LoadSegs(int lump) {\n"
"    const mapseg_t *ml = W_CacheLumpNum(lump);\n"
"    int n = W_LumpLength(lump) / sizeof(mapseg_t);\n"
"    seg_t *segs = Z_Malloc(n * sizeof(seg_t), PU_LEVEL, 0);\n"
"    _g->segs = segs;\n"
"    for (int i = 0; i < n; i++) {\n"
"        const mapseg_t *m = &ml[i];\n"
"        seg_t *li = &segs[i];\n"
"        li->v1 = _g->vertexes[(unsigned short)SHORT(m->v1)];\n"
"        li->v2 = _g->vertexes[(unsigned short)SHORT(m->v2)];\n"
"        li->angle  = (angle_t)SHORT(m->angle)  << 16;\n"
"        li->offset = (fixed_t)SHORT(m->offset) << FRACBITS;\n"
"        int linenum = (unsigned short)SHORT(m->linedef);\n"
"        const line_t *ldef = &_g->lines[linenum];\n"
"        int side = SHORT(m->side);\n"
"        li->sidenum = ldef->sidenum[side];\n"
"        li->frontsectornum = (li->sidenum != NO_INDEX)\n"
"            ? _g->sides[li->sidenum].sector - _g->sectors : NO_INDEX;\n"
"        /* backsector via ldef->sidenum[side^1] when ML_TWOSIDED ... */\n"
"    }\n"
"}",
"After the rewrite the counts came out sane (verts=402, sectors=82, sides=273, "
"lines=431, subs=197) with zero P_GroupLines warnings, on both native_sim and HW.")
P("Two adjacent fixes were needed for the stripped Squashware WAD: the status bar "
  "and HUD draw routines were gated out for the matrix build (Squashware strips the "
  "STGANUM* digit graphics, so ST_Drawer dereferenced NULL patches), and the title "
  "screen was replaced with a direct <font face='Courier'>G_DeferedInitNew()</font> "
  "into E1M1.")

# ============================ 7. RED HERRINGS ===============================
H("7. The red herrings (and how each was ruled out)")
P("Before the WAD-loader root cause was found, three other hypotheses looked "
  "plausible. Each was investigated, and although none was the cause, two were real "
  "latent bugs worth keeping. This is the honest part of any field report.")
bullets([
    "<b>Stack overflow.</b> The renderer recurses (R_RenderBSPNode) and level setup "
    "uses large stack locals. The 8&nbsp;KB main stack genuinely was too small, so "
    "it was raised to 32&nbsp;KB and the MPU + HW stack protection were enabled. "
    "Real improvement &mdash; but the crash persisted, now faulting cleanly instead "
    "of corrupting RAM silently.",
    "<b>Unaligned access.</b> The WAD&rsquo;s infotableofs is odd "
    "(<font face='Courier'>0x19c029</font>), so packed multi-byte reads could "
    "LDRD-fault. Adding <font face='Courier'>-mno-unaligned-access</font> was correct "
    "hardening, but the crash address merely moved &mdash; symptom, not cause.",
    "<b>Display downscale stride.</b> The matrix downscale indexed the frame with "
    "<font face='Courier'>CONFIG_DOOM_X_RES</font> (the 565 width) instead of "
    "<font face='Courier'>SCREENWIDTH = X_RES/2</font> (the engine packs two columns "
    "per output pixel). This overran the backbuffer &mdash; a genuine bug, fixed "
    "&mdash; but downstream of the load crash.",
])
callout("The lesson the red herrings teach", [
    "Every one of these &lsquo;wrong&rsquo; theories produced a memory-safety "
    "improvement, and on bare metal that is not wasted work: tightening the MPU and "
    "the alignment behaviour <b>changed the crash from silent corruption into a "
    "precise fault with a valid BFAR</b> (next section). Sometimes you fix the "
    "instrumentation before you fix the bug."])

# ============================ 8. FAULT ON METAL =============================
H("8. Reading a bus fault on bare metal")
P("On hardware there is no Valgrind. The Cortex-M33 fault status registers are the "
  "equivalent, and they are precise if you read them correctly. The fault here was "
  "caught with a hardware breakpoint on the fault path, then decoded from CFSR / "
  "BFAR:")
code(
"# OpenOCD running on the device (linuxgpiod SWD bitbang):\n"
"> mrw 0xE000ED28      ; CFSR  (Configurable Fault Status)\n"
"  0x00008200\n"
"> mrw 0xE000ED38      ; BFAR  (Bus Fault Address)\n"
"  0x200C2048")
P("Decoding CFSR = 0x00008200:")
datatable([
    ["Field", "Bits", "Value", "Meaning"],
    ["UFSR", "[31:16]", "0x0000", "no usage fault (so NOT div-by-zero / undef instr)"],
    ["BFSR", "[15:8]", "0x82", "BFARVALID (0x80) + PRECISERR (0x02): precise data bus error, BFAR valid"],
    ["MMFSR", "[7:0]", "0x00", "no MPU region fault"],
], widths=[20*mm, 18*mm, 18*mm, 110*mm])
P("So it is a <b>precise data bus fault</b> and BFAR is trustworthy. BFAR = "
  "0x200C2048. SRAM ends at 0x200C0000 (0x20000000 + 768&nbsp;KB), so the faulting "
  "access is 0x2048 bytes <b>past the top of RAM</b> &mdash; a wild pointer read, "
  "consistent with the cast-based loader walking off a mis-sized array. The fault "
  "register decode and the native_sim Valgrind report pointed at the same code from "
  "two completely different directions, which is how you know you have the root "
  "cause and not a coincidence.")
callout("Catching the fault, not its aftermath", [
    "A naive halt lands you in z_arm_fatal_error with the original context already "
    "unwound. Set a hardware breakpoint on the fault entry "
    "(<font face='Courier'>bp z_arm_usage_fault</font> / the BusFault vector) so you "
    "stop with the <b>exception stack frame</b> intact, then read the stacked PC / "
    "LR from the ESF to get the faulting instruction. CFSR/BFAR are latched, so they "
    "survive to the handler &mdash; but the call stack does not."])

H("8.1  GDB for layout, not just for stepping", 2)
P("You rarely single-step a 35-fps game over SWD. GDB&rsquo;s more valuable role "
  "here was static: extracting struct offsets from the ELF so raw memory reads could "
  "be interpreted. There is no need to guess where a field lives &mdash; ask the "
  "DWARF:")
code(
"arm-zephyr-eabi-gdb -q -batch build/zephyr/zephyr.elf \\\n"
"  -ex 'printf \"gametic=%d\\n\",  (int)&((globals_t*)0)->gametic'  \\\n"
"  -ex 'printf \"player=%d\\n\",   (int)&((globals_t*)0)->player'   \\\n"
"  -ex 'printf \"mo=%d\\n\",       (int)&((player_t*)0)->mo'        \\\n"
"  -ex 'printf \"angle=%d\\n\",    (int)&((mobj_t*)0)->angle'\n"
"# gametic=484  player=232  mo=0  angle=32")

H("8.2  SWD memory peek: the printf you did not compile in", 2)
P("The most-used technique across the whole port was non-intrusive "
  "halt / read / resume over SWD. <font face='Courier'>nm</font> gives the address "
  "of the <font face='Courier'>_g</font> pointer; dereference it, add the GDB "
  "offsets, and you can watch live engine state without a single added "
  "<font face='Courier'>printf</font> or a reflash:")
code(
"> halt\n"
"> mrw 0x20000830          ; _g  (pointer to globals)\n"
"  0x20017DB4\n"
"> mdw 0x20017DB4+484 1    ; gametic\n"
"  0x000008F4               ; = 2292\n"
"> resume\n"
"  ... wait 2.5 s, halt again ...\n"
"> mdw 0x20017DB4+484 1\n"
"  0x0000097C               ; +136 in 2.5 s ~= 35 tics/s  -> the loop is LIVE")
P("This one trick answered the &lsquo;frozen or running?&rsquo; question "
  "definitively. The matrix looked static, but gametic was advancing at exactly "
  "35&nbsp;tics/s and the main loop blocks on the display semaphore every frame "
  "&mdash; so the display thread <i>had</i> to be completing each frame. The image "
  "was static only because the player was standing still. To prove the render path "
  "end-to-end, we wrote the player&rsquo;s view angle directly and watched the "
  "framebuffer change:")
code(
"> mww 0x20033BB8 0xC0000000   ; player.mo->angle = 180 deg (BAM)\n"
"> resume                       ; the matrix image rotates 180 deg\n"
"# framebuffer at *cplx_fb went from a dark wall to a bright room.")

# ============================ 9. VALGRIND ==================================
H("9. native_sim + Valgrind: the fast lane")
P("Because native_sim is an ordinary x86 ELF, the entire host toolbox applies. The "
  "level-load crash that took hardware breakpoints and CFSR decoding to corner on "
  "the MCU was, on native_sim, a one-line Valgrind run that named the offending "
  "function and the invalid read width directly. The workflow:")
code(
"# Host build (NOT the docker builder: it lacks SDL2 and is non-root).\n"
"python3 -m venv /tmp/zvenv && . /tmp/zvenv/bin/activate\n"
"pip install -r zephyr/scripts/requirements-base.txt\n"
"export ZEPHYR_BASE=$PWD/zephyr ZEPHYR_TOOLCHAIN_VARIANT=host\n"
"cmake -GNinja -B build_ns -S doom-mcx -DBOARD=native_sim/native/64\n"
"ninja -C build_ns\n"
"\n"
"# Reproduce + localise in one shot:\n"
"SDL_VIDEODRIVER=dummy valgrind --error-exitcode=1 \\\n"
"    build_ns/zephyr/zephyr.exe")
callout("Why x86 repro is worth the dual-target maintenance", [
    "A 2-minute flash makes bisection painful: ten iterations is half an hour of "
    "watching a progress bar. The same ten iterations on native_sim is under two "
    "minutes total, with Valgrind annotating each one. Spend the up-front effort to "
    "keep the simulator build green &mdash; it pays for itself the first time you "
    "have a memory bug. Just remember its blind spots (display, timing, alignment) "
    "and finish on hardware."])

# ============================ 10. DISPLAY ==================================
H("10. Making 104 pixels legible")
P("Getting DOOM to render was not the same as getting it to be <i>readable</i> on a "
  "13&times;8 grid. Two problems, both fixed in the downscale path.")
H("10.1  Aspect ratio", 2)
P("The engine renders at "
  "<font face='Courier'>SCREENWIDTH = X_RES/2</font> by "
  "<font face='Courier'>SCREENHEIGHT = Y_RES</font>. With X_RES=400, Y_RES=320 that "
  "is a <b>200&times;320 portrait</b> frame &mdash; taller than wide &mdash; box-"
  "downscaled into a 13&times;8 <b>landscape</b> (1.625:1) matrix. The vertical axis "
  "was crushed ~40:1 against ~15:1 horizontal, smearing the scene into noise. "
  "Dropping Y_RES to 128 gives a landscape 200&times;128 frame (~1.56:1, close to "
  "13:8) with the correct DOOM proportions &mdash; and frees ~75&nbsp;KB of RAM. "
  "The width was deliberately left at 200: an earlier 160&times;128 attempt "
  "(SCREENWIDTH=80) tripped small-width assumptions in the column renderer.")
H("10.2  Eight grey levels, used", 2)
P("DOOM scenes are dark; a naive linear map to 0..7 left the panel using only "
  "0..2. The fix is a per-frame auto-exposure: keep full 0..255 luma per cell, then "
  "stretch this frame&rsquo;s [min,max] across the range and apply a brightening "
  "gamma via a threshold table. Verified over SWD that the framebuffer now spans the "
  "full 0..7.")
code(
"/* per cell: avg luma 0..255 -> stretch -> gamma threshold -> 0..7 */\n"
"int range = mx - mn; if (range < 24) range = 24;   /* don't amplify flat frames */\n"
"static const uint8_t thr[7] = {10,32,62,100,145,197,240};   /* gamma < 1 */\n"
"int s = ((avg[i] - mn) * 255) / range; if (s > 255) s = 255;\n"
"uint8_t lvl = 0; for (int k = 0; k < 7; k++) if (s >= thr[k]) lvl = k + 1;\n"
"cplx_fb[i] = lvl;")

# ============================ 11. INPUT ====================================
H("11. The input path: SPI slave, RDY, CRC, and an adb gotcha")
P("Input reuses the board&rsquo;s SoC&harr;STM32 SPI link. The STM32 runs an "
  "<b>SPI3 slave</b> (2&nbsp;MHz, mode 0, MSB, 8-bit) and the QCS Linux side is the "
  "<font face='Courier'>/dev/spidev</font> master. A 64-byte block protocol carries "
  "key state, with a hardware RDY line (PG13 == SoC gpiochip1:70) for flow control "
  "and a CRC-16/CCITT-FALSE per block:")
code(
"[0] 0xA5  [1] ver  [2] type  [3] seq  [4] len  [5..] payload  [62..63] crc16-le\n"
"TYPE_KEYS = 0x20, payload[0] = bitmask:\n"
"  bit0 fwd  bit1 back  bit2 left  bit3 right\n"
"  bit4 fire bit5 use   bit6 strafeL bit7 strafeR")
P("The slave parks in <font face='Courier'>spi_transceive()</font>, raises RDY when "
  "armed, drops it while parsing. A poll on the engine&rsquo;s main thread "
  "edge-detects the bitmask and posts DOOM key events. Two robustness details "
  "earned their keep:")
bullets([
    "<b>Watchdog.</b> If the host goes quiet (crash, unplug), the last bitmask would "
    "otherwise stick &mdash; the player runs into a wall forever. A 300&nbsp;ms "
    "timeout clears all keys. Verified by SWD: after the host exits mid-&lsquo;turn "
    "right&rsquo;, the angle stops changing within the window.",
    "<b>The transport bug.</b> The first design captured keys on the laptop and "
    "piped them to the device via <font face='Courier'>adb exec-out</font>. Nothing "
    "moved. SWD showed blocks arriving with zero CRC errors but a zero key mask: "
    "<font face='Courier'>adb exec-out</font> <b>does not forward stdin</b>. "
    "<font face='Courier'>adb shell -t</font> allocates a PTY and does, so the "
    "controller was moved onto the device and launched over an adb shell.",
])
callout("Two more gotchas from the host side", [
    "The stock device python is a <b>minimal build with no termios/tty module</b>, "
    "so raw mode is set with <font face='Courier'>stty -echo -icanon min 1 time 0</font> "
    "around the controller rather than from Python. And "
    "<font face='Courier'>adb shell &lt;cmd&gt;</font> runs <b>without a PTY</b> "
    "unless you pass <font face='Courier'>-t</font> &mdash; without it your local "
    "terminal stays cooked, keys echo locally and only flush on Enter."])

# ============================ 12. FLASHING =================================
H("12. Flashing without wedging the link")
P("A 1.94&nbsp;MB image over a bit-banged SWD link is slow, and two sharp edges "
  "cost real time before they were understood:")
bullets([
    "<b>Do not verify over SWD.</b> OpenOCD&rsquo;s <font face='Courier'>verify_image</font> "
    "reads the whole 1.94&nbsp;MB back at ~15&nbsp;KiB/s &mdash; minutes on top of "
    "the write, and it hung outright for ~12 on one run. The write itself is "
    "reliable; drop the verify. A custom <font face='Courier'>flash-fast.sh</font> "
    "does write + reset only.",
    "<b>reset halt, not halt.</b> A bare <font face='Courier'>halt</font> on a busy "
    "target gives &lsquo;target was in unknown state when halt was requested&rsquo; "
    "and the flash algorithm can time out. <font face='Courier'>reset halt</font> "
    "gives the flash loader a clean machine.",
    "<b>Never poll over adb during an SWD flash.</b> Concurrent adb access while "
    "OpenOCD drives SWD wedges the link and forces a physical reset. Start the flash "
    "in the background and leave the device alone until it finishes.",
])

# ============================ 13. PLAYBOOK =================================
H("13. Playbook: what to carry to the next port")
bullets([
    "<b>Two targets, always.</b> A host simulator for logic + Valgrind, the metal "
    "for everything hardware. Reproduce on the simulator first; finish on hardware.",
    "<b>Clear the environment before you trust a bug.</b> Across a libc or Zephyr "
    "version gap, the first failures are toolchain (symbol collisions, missing libc "
    "calls), not your code.",
    "<b>A frozen screen with a live CPU is usually a dead clock or a one-shot "
    "handshake,</b> not a dead renderer. Prove the tic advances first.",
    "<b>Decode the fault registers, do not guess.</b> CFSR tells you the class; "
    "BFAR/MMFAR the address; the ESF the instruction. Tighten the MPU so corruption "
    "becomes a precise fault.",
    "<b>SWD halt/read/resume is a free printf.</b> nm + GDB offsets + a pointer "
    "deref let you watch any global live, no reflash, no instrumentation.",
    "<b>Red herrings that harden memory safety are not wasted</b> &mdash; they often "
    "turn an undiagnosable corruption into a catchable fault.",
    "<b>Know your flash link&rsquo;s limits.</b> Skip the SWD read-back verify; use "
    "reset halt; never share the debug link with another tool mid-operation.",
])
gap(6)
story.append(HRFlowable(width="100%", thickness=0.8, color=RULE, spaceAfter=8))
P("<i>The result: DOOM running from internal flash on an STM32U585, E1M1 rendering "
  "to a 13&times;8 LED matrix at the engine&rsquo;s native tic rate, driven by WASD "
  "over the on-board SPI bridge &mdash; 92.9% of flash, 50.6% of RAM, and a fistful "
  "of fault registers later.</i>", BODYL)

# ---- page furniture ---------------------------------------------------------
def furniture(canvas, doc):
    canvas.saveState()
    w, h = A4
    if doc.page == 1:
        canvas.setFillColor(ACCENT)
        canvas.rect(0, h-10*mm, w, 10*mm, stroke=0, fill=1)
        canvas.setFillColor(colors.HexColor("#111111"))
        canvas.rect(0, 0, w, 6*mm, stroke=0, fill=1)
    else:
        canvas.setStrokeColor(RULE)
        canvas.setLineWidth(0.5)
        canvas.line(18*mm, h-15*mm, w-18*mm, h-15*mm)
        canvas.setFont("Helvetica", 7.5)
        canvas.setFillColor(SLATE)
        canvas.drawString(18*mm, h-13*mm, "DOOM on STM32U585 — a debugging field report")
        canvas.drawRightString(w-18*mm, h-13*mm, "")
        canvas.setStrokeColor(RULE)
        canvas.line(18*mm, 13*mm, w-18*mm, 13*mm)
        canvas.setFont("Helvetica", 8)
        canvas.drawCentredString(w/2, 9*mm, str(doc.page))
    canvas.restoreState()

doc = BaseDocTemplate(OUT, pagesize=A4, leftMargin=18*mm, rightMargin=18*mm,
                      topMargin=20*mm, bottomMargin=18*mm, title="Getting DOOM to Run on an STM32U585",
                      author="Field report")
frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="main")
doc.addPageTemplates([PageTemplate(id="all", frames=[frame], onPage=furniture)])
doc.build(story)
print("wrote", OUT)
