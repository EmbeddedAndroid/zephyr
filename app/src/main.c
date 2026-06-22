/*
 * ZEPHYR-SAIL: minimal non-supervising replacement for the Qualcomm
 * SA8797P "SAIL" safety-island firmware.
 *
 * Job:
 *   1. Run on the SAIL Cortex-R52 boot core (loaded by the SAIL PBL at
 *      0x08021000).
 *   2. Satisfy XBL_SC's blocking SAIL boot handshake so the Mission
 *      Domain (Oryon/APSS, Linux) is released and boots.
 *   3. Idle forever WITHOUT supervising or resetting the MD.
 *
 * It deliberately does NOT:
 *   - arm any SSM/FUSA watchdog,
 *   - drive PSAIL_ERR_N / MD PS_HOLD,
 *   - run the ISD reset state machine.
 * That removes the stock SAIL's fixed ~15.5 s MD reset entirely.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/arch/cpu.h>
#include <stdint.h>

/* ----------------------------------------------------------------------
 * XBL <-> SAIL handshake registers (TCSR shadow regs, physical addrs).
 *
 * From core.xbl SailLib.c / BootSailMdHWIO.h (the running XBL_SC build):
 *   SAIL_TO_MD_ACK_REG = TCSR_SAIL2MAIN_GP_NONSEC_SHADOW_STATUS4 = 0x01F7B014
 * XBL polls this until it equals BOOT_SAIL_PASS2_COMPLETE.
 *
 * The XBL source enum gives BOOT_SAIL_PASS2_COMPLETE = 0xA0300700.
 * The decompiled stock SAIL (sailhyp.elf) writes an ascending cookie
 * sequence ...0x500 (stage), 0x600 (final) to the ack register as its
 * last pass-2 step. To be robust against either constant we write the
 * whole ascending sequence ending on 0xA0300700.
 * ---------------------------------------------------------------------- */
#define TCSR_SAIL_TO_MD_ACK_REG      0x01F7B014u  /* SAIL2MAIN SHADOW STATUS4 */
#define TCSR_SAIL_TO_MD_MESSAGE_REG  0x01F7B028u  /* SAIL2MAIN SHADOW STATUS9 */

#define COOKIE_PASS2_STAGE_500       0xA0300500u
#define COOKIE_PASS2_STAGE_600       0xA0300600u
#define COOKIE_PASS2_COMPLETE_700    0xA0300700u  /* XBL BOOT_SAIL_PASS2_COMPLETE */

/* APSS_INTU IPC - non-secure doorbell to the MD (optional nudge). */
#define APSS_INTU_IPC_BASE           0x17824000u
#define APSS_INTU_NS_IPC_REG         (APSS_INTU_IPC_BASE + 0x8u)
#define APSS_INTU_NS_IPC_BIT         0x800000u   /* SMSS bit */

static inline void mmio_w32(uint32_t addr, uint32_t val)
{
	*(volatile uint32_t *)(uintptr_t)addr = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t mmio_r32(uint32_t addr)
{
	return *(volatile uint32_t *)(uintptr_t)addr;
}

/* ----------------------------------------------------------------------
 * SAIL console UART: sailss QUPv3 SE2. R52-local base from Ghidra decompile
 * of stock sailhyp.elf (DAT_080922e8 = 0xf8800000 QUPv3 ID base; SE region
 * at +0x80000; SE2 = +2*0x4000). IPcat: console = u_sailss..u_qupv3_wrapper_0
 * SE2. GENI register offsets per TF-A qti_uart_console.S. Minimal polled TX
 * (assumes the SAIL PBL left SE2 clocked/UART-proto, like XBL does for the MD).
 * ---------------------------------------------------------------------- */
#define SAIL_UART_SE2        0xf8888000u
#define GENI_STATUS_OFF      0x040u
#define GENI_M_CMD_ACTIVE    0x1u
#define UART_TX_TRANS_LEN    0x270u
#define GENI_M_CMD0_OFF      0x600u
#define GENI_TX_FIFO_OFF     0x700u
#define GENI_M_CMD_TX        0x08000000u

static const uint32_t sail_geni_cfg[69] = {0x00000090,0x00000000,0x00000090,0x00000000,0x00038028,0x00084080,0x00000343,0x00010000,0x00000000,0x00001a00,0x00000100,0x00000000,0x00000000,0x00000000,0x00808008,0x001c0020,0x00000000,0x00020000,0x00000000,0x00000201,0x0001fc01,0x00036222,0x09c01ffc,0x00100120,0x02c00000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x00000000,0x00000000,0x00000409,0x00000003,0x00000002,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x0007f8fe,0x000ffefe,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000002,0x00000001,0x0007f807,0x000ffefe,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00c00000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000055};

/* Replicates stock SAIL FUN_08044dfc SE2 GENI-UART config (proto FW image +
 * register defaults) decompiled from sailhyp.elf. Base = SE2 0xf8888000. */
/* SAILSS_CLKCTL QUPV3_WRAP0 clock (from IPcat SAILSS_CC freq plan id 14607):
 * S2 serial clk = 7.3728 MHz from PLL0(600MHz,SRC_SEL=1): M=192 N=15625 div=1 dual-edge.
 * CORE_2X = 100 MHz from PLL0 div=6. Then enable the wrapper CBCRs. */
#define CC 0xf0e25000u
static void sail_rcg(uint32_t cmd, uint32_t cfg, uint32_t m, uint32_t n, uint32_t d)
{
	if (n) { mmio_w32(cmd+0x8,m); mmio_w32(cmd+0xc,n); mmio_w32(cmd+0x10,d); }
	mmio_w32(cmd+0x4,cfg);
	mmio_w32(cmd, mmio_r32(cmd)|1u);          /* UPDATE */
	for (uint32_t to=100000u; (mmio_r32(cmd)&1u)&&--to; ) ;
}
static void sail_clk_init(void)
{
	mmio_w32(0x01F7B01Cu, 0xAAAA0001u);
	/* S2 RCG @ CC+0x3e4: cfg=DUAL_EDGE(0x2000)|SRC1(0x100)|DIV1(0x1)=0x2101, M=0xC0, N=~(n-m)=0xC3B6, D=~n=0xC2F6 */
	mmio_w32(CC+0x3e8, 0x2101u);
	mmio_w32(0x01F7B028u, 0xCF000000u | (mmio_r32(CC+0x3e8) & 0xFFFFu)); /* CF002101 if CLKCTL writable */
	mmio_w32(0x01F7B024u, 0xAAAA0002u);
	sail_rcg(CC+0x3e4, 0x2101u, 0xC0u, 0xC3B6u, 0xC2F6u);
	/* CORE_2X RCG @ CC+0x30: cfg=SRC1|DIV6(0xB)=0x10B, no MND */
	sail_rcg(CC+0x30, 0x10Bu, 0,0,0);
	/* enable CBCRs: M_AHB(+4) S_AHB(+8) CORE(+0xc) CORE_2X(+0x20) S2(+0x3d4) */
	mmio_w32(CC+0x004, mmio_r32(CC+0x004)|1u);
	mmio_w32(CC+0x008, mmio_r32(CC+0x008)|1u);
	mmio_w32(CC+0x00c, mmio_r32(CC+0x00c)|1u);
	mmio_w32(CC+0x020, mmio_r32(CC+0x020)|1u);
	mmio_w32(CC+0x3d4, mmio_r32(CC+0x3d4)|1u);
	mmio_w32(0x01F7B004u, mmio_r32(CC+0x3e4)); /* S2 CMD_RCGR: bit31=ROOT_OFF */
	/* diag: report clock state to MD via SAIL2MAIN shadow STATUS5-9 */
	mmio_w32(0x01F7B018u, mmio_r32(CC+0x3e4));   /* S2 CMD_RCGR (bit31=ROOT_OFF) */
	mmio_w32(0x01F7B01Cu, mmio_r32(CC+0x3d4));   /* S2 CBCR (bit31=CLK_OFF) */
	mmio_w32(0x01F7B020u, mmio_r32(CC+0x3e8));   /* S2 CFG_RCGR readback */
	mmio_w32(0x01F7B024u, mmio_r32(CC+0x020));   /* CORE_2X CBCR */
}
static void sail_geni_init(void)
{
	sail_clk_init();
	mmio_w32(SAIL_UART_SE2 + 0x024u, 0x7Fu);   /* OUTPUT_CTRL */
	mmio_w32(SAIL_UART_SE2 + 0x048u, 0x9u);    /* SER_M_CLK_CFG: div4, en (7.3728M/4/16=115200) */
	mmio_w32(SAIL_UART_SE2 + 0x1000u, 0x301u);
	mmio_w32(SAIL_UART_SE2 + 0x1004u, 0x301u);
	mmio_w32(SAIL_UART_SE2 + 0x0u, 9u);
	mmio_w32(SAIL_UART_SE2 + 0x4u, 9u);
	for (int i = 0; i < 19; i++)
		mmio_w32(SAIL_UART_SE2 + 0x100u + i*4u, sail_geni_cfg[i]);
	for (int i = 19; i < 69; i++)
		mmio_w32(SAIL_UART_SE2 + 0x200u + (i-19)*4u, sail_geni_cfg[i]);
}

static int sail_geni_putc(int c)
{
	uint32_t to = 2000000u;
	if (c == '\n')
		sail_geni_putc('\r');
	while ((mmio_r32(SAIL_UART_SE2 + GENI_STATUS_OFF) & GENI_M_CMD_ACTIVE) && --to)
		;
	mmio_w32(SAIL_UART_SE2 + UART_TX_TRANS_LEN, 1u);
	mmio_w32(SAIL_UART_SE2 + GENI_M_CMD0_OFF, GENI_M_CMD_TX);
	mmio_w32(SAIL_UART_SE2 + GENI_TX_FIFO_OFF, (uint32_t)(uint8_t)c);
	return c;
}

static void sail_puts(const char *s)
{
	while (*s)
		sail_geni_putc(*s++);
}

/* Write the PASS2_COMPLETE handshake so XBL_SC releases the MD. */
static void sail_signal_pass2_complete(void)
{
	/* Clear any stale message reg. */
	mmio_w32(TCSR_SAIL_TO_MD_MESSAGE_REG, 0x0u);

	/* Ascending ack cookie sequence -> SAIL_TO_MD_ACK_REG. */
	mmio_w32(TCSR_SAIL_TO_MD_ACK_REG, COOKIE_PASS2_STAGE_500);
	mmio_w32(TCSR_SAIL_TO_MD_ACK_REG, COOKIE_PASS2_STAGE_600);
	mmio_w32(TCSR_SAIL_TO_MD_ACK_REG, COOKIE_PASS2_COMPLETE_700);

	/* Optional non-secure IPC doorbell to the MD. Harmless if unused. */
	mmio_w32(APSS_INTU_NS_IPC_REG, APSS_INTU_NS_IPC_BIT);
}

static void disarm_cti(uint32_t base)
{
	mmio_w32(base + 0xFB0u, 0xC5ACCE55u);  /* CTILAR unlock */
	mmio_w32(base + 0x000u, 0x0u);          /* CTICONTROL=0 disable */
	mmio_w32(base + 0x140u, 0x0u);          /* CTIGATE=0 block channels */
}

int main(void)
{
	/* Do the handshake as early as possible so the MD is released
	 * with minimal added latency.
	 */
	mmio_w32(0x01F7B018u, 0xAAAA0000u);
	sail_signal_pass2_complete();
	/* #2: SAIL(R52) holds the proc-halt CTIs disarmed so the DBGEN-gated ~9s
	 * APSS halt never fires with no JTAG. Loop forever (handshake already done). */
	for (;;) {
		mmio_w32(0xc42f0000u, 0xa1000876u);  /* BRIDGE TEST: set RDE spare bit1 via SAIL->MD AOSS view */
		disarm_cti(0xc9a21000u);  /* AOSS AOP debug CTI */
		disarm_cti(0xc6c21000u);  /* AOP debug CTI */
		disarm_cti(0xece21000u);  /* CPU_RVSS_CTI */
		disarm_cti(0xede21000u);  /* SEQ_RVSS_CTI */
	}

	sail_geni_init();
	sail_puts("\r\n[ZEPHYR-SAIL] alive on R52: PASS2_COMPLETE sent, MD released.\r\n");
	sail_puts("[ZEPHYR-SAIL] non-supervising stub; console = sailss QUPv3 SE2 @ 0xf8888000.\r\n");

	printk("ZEPHYR-SAIL alive: PASS2_COMPLETE=0x%08x written to 0x%08x\n",
	       COOKIE_PASS2_COMPLETE_700, TCSR_SAIL_TO_MD_ACK_REG);
	printk("ZEPHYR-SAIL: non-supervising stub - MD released, idling forever\n");

	/* Idle forever. Never supervise, never reset the MD. */
	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
