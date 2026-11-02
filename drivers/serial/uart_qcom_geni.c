/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Qualcomm GENI serial engine in UART mode, FIFO mode with polled transfers.
 * When the serial engine has no protocol firmware, the UART firmware is loaded
 * from an image built in with CONFIG_UART_QCOM_GENI_SE_FW.
 */

#define DT_DRV_COMPAT qcom_geni_uart

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>

/* Serial engine registers. */
#define SE_GENI_INIT_CFG_REVISION   0x000U
#define SE_GENI_S_INIT_CFG_REVISION 0x004U
#define GENI_FORCE_DEFAULT_REG      0x020U
#define GENI_OUTPUT_CTRL            0x024U
#define SE_GENI_CGC_CTRL            0x028U
#define SE_GENI_STATUS              0x040U
#define GENI_SER_M_CLK_CFG          0x048U
#define GENI_SER_S_CLK_CFG          0x04cU
#define GENI_FW_REVISION_RO         0x068U
#define SE_GENI_CLK_SEL             0x07cU
#define SE_GENI_CFG_REG0            0x100U
#define SE_UART_LOOPBACK_CFG        0x22cU
#define SE_GENI_BYTE_GRAN           0x254U
#define SE_GENI_DMA_MODE_EN         0x258U
#define SE_UART_TX_TRANS_CFG        0x25cU
#define SE_GENI_TX_PACKING_CFG0     0x260U
#define SE_GENI_TX_PACKING_CFG1     0x264U
#define SE_UART_TX_WORD_LEN         0x268U
#define SE_UART_TX_STOP_BIT_LEN     0x26cU
#define SE_UART_TX_TRANS_LEN        0x270U
#define SE_UART_RX_TRANS_CFG        0x280U
#define SE_GENI_RX_PACKING_CFG0     0x284U
#define SE_GENI_RX_PACKING_CFG1     0x288U
#define SE_UART_RX_WORD_LEN         0x28cU
#define SE_UART_RX_STALE_CNT        0x294U
#define SE_UART_TX_PARITY_CFG       0x2a4U
#define SE_UART_RX_PARITY_CFG       0x2a8U
#define SE_GENI_M_CMD0              0x600U
#define SE_GENI_M_IRQ_STATUS        0x610U
#define SE_GENI_M_IRQ_EN            0x614U
#define SE_GENI_M_IRQ_CLEAR         0x618U
#define SE_GENI_S_CMD0              0x630U
#define SE_GENI_S_CMD_CTRL_REG      0x634U
#define SE_GENI_S_IRQ_STATUS        0x640U
#define SE_GENI_S_IRQ_EN            0x644U
#define SE_GENI_S_IRQ_CLEAR         0x648U
#define SE_GENI_TX_FIFOn            0x700U
#define SE_GENI_RX_FIFOn            0x780U
#define SE_GENI_TX_FIFO_STATUS      0x800U
#define SE_GENI_RX_FIFO_STATUS      0x804U
#define SE_GENI_TX_WATERMARK_REG    0x80cU
#define SE_GENI_RX_WATERMARK_REG    0x810U
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814U
#define SE_DMA_TX_IRQ_CLR           0xc44U
#define SE_DMA_TX_IRQ_EN_SET        0xc4cU
#define SE_DMA_RX_IRQ_CLR           0xd44U
#define SE_DMA_RX_IRQ_EN_SET        0xd4cU
#define SE_GSI_EVENT_EN             0xe18U
#define SE_IRQ_EN                   0xe1cU
#define SE_HW_PARAM_1               0xe28U
#define SE_HW_PARAM_2               0xe2cU
#define SE_DMA_GENERAL_CFG          0xe30U
#define SE_GENI_FW_REVISION         0x1000U
#define SE_GENI_S_FW_REVISION       0x1004U
#define SE_GENI_CFG_RAMN            0x1010U
#define SE_GENI_CLK_CTRL            0x2000U
#define SE_DMA_IF_EN                0x2004U
#define SE_FIFO_IF_DISABLE          0x2008U

/* Wrapper common registers. */
#define QUPV3_HW_VER_REG            0x004U
#define QUPV3_SE_AHB_M_CFG          0x118U
#define QUPV3_COMMON_CFG            0x120U
#define QUPV3_COMMON_CGC_CTRL       0x21cU

#define FW_REV_PROTOCOL             GENMASK(15, 8)
#define FW_REV_VERSION              GENMASK(7, 0)
#define GENI_PROTO_UART             2U
#define GENI_PROTO_NONE             0U
#define GENI_PROTO_INVALID          0xffU

#define M_GENI_CMD_ACTIVE           BIT(0)
#define S_GENI_CMD_ACTIVE           BIT(12)
#define M_OPCODE_SHIFT              27U
#define S_OPCODE_SHIFT              27U
#define UART_START_TX               1U
#define UART_START_READ             1U
#define S_GENI_CMD_CANCEL           BIT(2)

#define M_CMD_DONE                  BIT(0)
#define M_CMD_CANCEL                BIT(4)
#define M_CMD_ABORT                 BIT(5)
#define M_TX_FIFO_WATERMARK         BIT(30)
#define S_RX_FIFO_WATERMARK         BIT(26)
#define S_RX_FIFO_LAST              BIT(27)
#define M_COMMON_IRQ_EN             (GENMASK(6, 1) | BIT(22) | BIT(23) | BIT(24) | BIT(25) |   \
				     BIT(28) | BIT(29))
#define S_FW_LOAD_IRQ_EN            (BIT(1) | BIT(2) | BIT(4) | BIT(5) | GENMASK(12, 9) |        \
				     BIT(24) | BIT(25))
#define S_COMMON_IRQ_EN             (GENMASK(5, 1) | GENMASK(13, 9) | BIT(24) | BIT(25))

#define RX_FIFO_WC                  GENMASK(24, 0)
#define FIFO_DEPTH                  GENMASK(21, 16)
#define PROG_RAM_DEPTH              GENMASK(10, 0)

#define SER_CLK_EN                  BIT(0)
#define CLK_DIV                     GENMASK(15, 4)
#define DEFAULT_CGC_EN              GENMASK(6, 0)
#define DEFAULT_IO_OUTPUT_CTRL      GENMASK(6, 0)
#define PROG_RAM_HCLK_OFF           BIT(8)
#define PROG_RAM_SCLK_OFF           BIT(9)
#define DMA_CGC_ON                  GENMASK(3, 0)
#define SE_IRQ_FIFO_EN              (BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define UART_CTS_MASK               BIT(1)
#define QUP_SE_VERSION_2_5          0x20050000U

#define BITS_PER_CHAR               8U
#define STALE_TIMEOUT_BITS          (10U * 16U)
#define POLL_LOOPS                  200000U

/* ELF and serial engine firmware image layout, as loaded by Linux. */
#define SE_FW_MAGIC                 0x57464553U
#define ELF_PT_LOAD                 1U
#define MI_PBT_PAGE_MODE            BIT(20)
#define MI_PBT_SEGMENT_TYPE         GENMASK(26, 24)
#define MI_PBT_ACCESS_TYPE          GENMASK(23, 21)
#define MI_PBT_HASH_SEGMENT         2U
#define MI_PBT_NOTUSED_SEGMENT      3U
#define MI_PBT_SHARED_SEGMENT       4U

struct geni_uart_config {
	mem_addr_t base;
	mem_addr_t wrapper;
	const struct device *clock_dev;
	clock_control_subsys_t se_clock;
	const clock_control_subsys_t *wrapper_clocks;
	uint8_t num_wrapper_clocks;
	uint32_t se_clock_hz;
	uint32_t baud;
	const struct pinctrl_dev_config *pcfg;
};

struct geni_uart_data {
	struct k_spinlock lock;
	uint32_t pending_rx;
	bool fw_loaded;
};

#ifdef CONFIG_UART_QCOM_GENI_SE_FW
static const uint8_t se_fw_image[] __aligned(4) = {
#include "qcom_geni_se_fw.inc"
};
#endif

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct geni_uart_config *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t val)
{
	const struct geni_uart_config *cfg = dev->config;

	sys_write32(val, cfg->base + off);
}

static inline void setbits(const struct device *dev, uint32_t off, uint32_t bits)
{
	wr(dev, off, rd(dev, off) | bits);
}

static inline void clrbits(const struct device *dev, uint32_t off, uint32_t bits)
{
	wr(dev, off, rd(dev, off) & ~bits);
}

static bool poll_bits(const struct device *dev, uint32_t off, uint32_t mask, bool set)
{
	for (uint32_t i = 0U; i < POLL_LOOPS; i++) {
		if (((rd(dev, off) & mask) != 0U) == set) {
			return true;
		}
	}
	return false;
}

static uint32_t read_proto(const struct device *dev)
{
	return FIELD_GET(FW_REV_PROTOCOL, rd(dev, GENI_FW_REVISION_RO));
}

#ifdef CONFIG_UART_QCOM_GENI_SE_FW
struct se_fw_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t core_version;
	uint16_t serial_protocol;
	uint16_t fw_version;
	uint16_t cfg_version;
	uint16_t fw_size_in_items;
	uint16_t fw_offset;
	uint16_t cfg_size_in_items;
	uint16_t cfg_idx_offset;
	uint16_t cfg_val_offset;
};

static uint32_t le32_at(const uint8_t *p, size_t off)
{
	return sys_get_le32(p + off);
}

static uint16_t le16_at(const uint8_t *p, size_t off)
{
	return sys_get_le16(p + off);
}

/* Find the UART protocol segment, with the same checks as Linux. */
static const uint8_t *find_uart_fw(const struct device *dev, uint32_t *words)
{
	const uint8_t *img = se_fw_image;
	size_t size = sizeof(se_fw_image);
	uint32_t phoff, phnum, phentsize, ram_depth;

	if (size < 0x34U || le32_at(img, 0) != 0x464c457fU) {
		return NULL;
	}
	phoff = le32_at(img, 0x1c);
	phentsize = le16_at(img, 0x2a);
	phnum = le16_at(img, 0x2c);
	if (phentsize < 0x20U || phnum < 2U || phoff + (size_t)phnum * phentsize > size) {
		return NULL;
	}
	ram_depth = FIELD_GET(PROG_RAM_DEPTH, rd(dev, SE_HW_PARAM_2));

	for (uint32_t i = 0U; i < phnum; i++) {
		const uint8_t *ph = img + phoff + (size_t)i * phentsize;
		uint32_t type = le32_at(ph, 0x00);
		uint32_t offset = le32_at(ph, 0x04);
		uint32_t filesz = le32_at(ph, 0x10);
		uint32_t memsz = le32_at(ph, 0x14);
		uint32_t flags = le32_at(ph, 0x18);
		const uint8_t *seg;
		uint32_t fw_size, fw_end, idx_end, val_end;

		if (offset > size || filesz > size - offset) {
			return NULL;
		}
		if (type != ELF_PT_LOAD || memsz == 0U || (flags & MI_PBT_PAGE_MODE) != 0U ||
		    FIELD_GET(MI_PBT_SEGMENT_TYPE, flags) == MI_PBT_HASH_SEGMENT ||
		    FIELD_GET(MI_PBT_ACCESS_TYPE, flags) == MI_PBT_NOTUSED_SEGMENT ||
		    FIELD_GET(MI_PBT_ACCESS_TYPE, flags) == MI_PBT_SHARED_SEGMENT ||
		    filesz < sizeof(struct se_fw_hdr)) {
			continue;
		}
		seg = img + offset;
		if (le32_at(seg, offsetof(struct se_fw_hdr, magic)) != SE_FW_MAGIC ||
		    le32_at(seg, offsetof(struct se_fw_hdr, version)) != 1U ||
		    le16_at(seg, offsetof(struct se_fw_hdr, serial_protocol)) != GENI_PROTO_UART) {
			continue;
		}
		fw_size = le16_at(seg, offsetof(struct se_fw_hdr, fw_size_in_items));
		fw_size += fw_size & 1U;
		fw_end = le16_at(seg, offsetof(struct se_fw_hdr, fw_offset)) + fw_size * 4U;
		idx_end = le16_at(seg, offsetof(struct se_fw_hdr, cfg_idx_offset)) +
			  le16_at(seg, offsetof(struct se_fw_hdr, cfg_size_in_items));
		val_end = le16_at(seg, offsetof(struct se_fw_hdr, cfg_val_offset)) +
			  le16_at(seg, offsetof(struct se_fw_hdr, cfg_size_in_items)) * 4U;
		if (fw_size >= ram_depth || fw_end > filesz || idx_end > filesz ||
		    val_end > filesz) {
			continue;
		}
		*words = fw_size;
		return seg;
	}
	return NULL;
}

static int load_uart_fw(const struct device *dev)
{
	const struct geni_uart_config *cfg = dev->config;
	const uint8_t *hdr;
	uint32_t words, cfg_items, cfg_version, depth;
	const uint8_t *fw, *idx, *val;

	hdr = find_uart_fw(dev, &words);
	if (hdr == NULL) {
		return -ENOENT;
	}
	fw = hdr + le16_at(hdr, offsetof(struct se_fw_hdr, fw_offset));
	idx = hdr + le16_at(hdr, offsetof(struct se_fw_hdr, cfg_idx_offset));
	val = hdr + le16_at(hdr, offsetof(struct se_fw_hdr, cfg_val_offset));
	cfg_items = le16_at(hdr, offsetof(struct se_fw_hdr, cfg_size_in_items));
	cfg_version = le16_at(hdr, offsetof(struct se_fw_hdr, cfg_version));

	sys_write32(sys_read32(cfg->wrapper + QUPV3_COMMON_CFG) | BIT(0),
		    cfg->wrapper + QUPV3_COMMON_CFG);
	sys_write32(sys_read32(cfg->wrapper + QUPV3_SE_AHB_M_CFG) | BIT(0),
		    cfg->wrapper + QUPV3_SE_AHB_M_CFG);
	sys_write32(sys_read32(cfg->wrapper + QUPV3_COMMON_CGC_CTRL) | BIT(0),
		    cfg->wrapper + QUPV3_COMMON_CGC_CTRL);

	wr(dev, GENI_OUTPUT_CTRL, 0U);
	setbits(dev, SE_GENI_CGC_CTRL, PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF);
	wr(dev, SE_GENI_CLK_CTRL, 0U);
	clrbits(dev, SE_GENI_CGC_CTRL, PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF);
	setbits(dev, SE_DMA_GENERAL_CFG, DMA_CGC_ON);
	wr(dev, SE_GENI_CGC_CTRL, DEFAULT_CGC_EN);

	wr(dev, SE_GENI_INIT_CFG_REVISION, cfg_version);
	wr(dev, SE_GENI_S_INIT_CFG_REVISION, cfg_version);
	for (uint32_t i = 0U; i < cfg_items; i++) {
		wr(dev, SE_GENI_CFG_REG0 + (uint32_t)idx[i] * 4U, le32_at(val, (size_t)i * 4U));
	}

	depth = FIELD_GET(FIFO_DEPTH, rd(dev, SE_HW_PARAM_1));
	wr(dev, SE_GENI_RX_RFR_WATERMARK_REG, depth - 2U);
	setbits(dev, GENI_OUTPUT_CTRL, DEFAULT_IO_OUTPUT_CTRL);

	/* FIFO mode. */
	clrbits(dev, SE_GENI_DMA_MODE_EN, BIT(0));
	wr(dev, SE_IRQ_EN, SE_IRQ_FIFO_EN);
	wr(dev, SE_GSI_EVENT_EN, 0U);
	wr(dev, SE_GENI_M_IRQ_EN, M_COMMON_IRQ_EN);
	wr(dev, SE_GENI_S_IRQ_EN, S_FW_LOAD_IRQ_EN);
	wr(dev, SE_DMA_TX_IRQ_EN_SET, BIT(0) | BIT(1) | BIT(3));
	wr(dev, SE_DMA_RX_IRQ_EN_SET, BIT(0) | BIT(1) | BIT(3) | BIT(4));

	wr(dev, SE_GENI_FW_REVISION,
	   FIELD_PREP(FW_REV_PROTOCOL, GENI_PROTO_UART) |
		   FIELD_PREP(FW_REV_VERSION,
			      le16_at(hdr, offsetof(struct se_fw_hdr, fw_version))));
	wr(dev, SE_GENI_S_FW_REVISION, rd(dev, SE_GENI_FW_REVISION));

	for (uint32_t i = 0U; i < words; i++) {
		wr(dev, SE_GENI_CFG_RAMN + i * 4U, le32_at(fw, (size_t)i * 4U));
	}

	wr(dev, GENI_FORCE_DEFAULT_REG, 1U);
	setbits(dev, SE_GENI_CGC_CTRL, PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF);
	setbits(dev, SE_GENI_CLK_CTRL, BIT(0));
	clrbits(dev, SE_GENI_CGC_CTRL, PROG_RAM_SCLK_OFF | PROG_RAM_HCLK_OFF);
	setbits(dev, SE_DMA_IF_EN, BIT(0));
	clrbits(dev, SE_FIFO_IF_DISABLE, BIT(0));
	return 0;
}
#endif

static int geni_uart_setup(const struct device *dev)
{
	const struct geni_uart_config *cfg = dev->config;
	uint32_t sampling, div, depth, ver;

	ver = sys_read32(cfg->wrapper + QUPV3_HW_VER_REG);
	sampling = ver >= QUP_SE_VERSION_2_5 ? 16U : 32U;
	div = DIV_ROUND_UP(cfg->se_clock_hz, cfg->baud * sampling);
	if (div == 0U || div > FIELD_GET(CLK_DIV, CLK_DIV)) {
		return -EINVAL;
	}

	/* Packing: one 8-bit character per FIFO word, TX and RX. */
	wr(dev, SE_GENI_TX_PACKING_CFG0, (7U << 1) | 1U);
	wr(dev, SE_GENI_TX_PACKING_CFG1, 0U);
	wr(dev, SE_GENI_RX_PACKING_CFG0, (7U << 1) | 1U);
	wr(dev, SE_GENI_RX_PACKING_CFG1, 0U);
	wr(dev, SE_GENI_BYTE_GRAN, 0U);

	/* geni_se_init(). */
	wr(dev, SE_GSI_EVENT_EN, 0U);
	wr(dev, SE_GENI_M_IRQ_CLEAR, UINT32_MAX);
	wr(dev, SE_GENI_S_IRQ_CLEAR, UINT32_MAX);
	wr(dev, SE_DMA_TX_IRQ_CLR, UINT32_MAX);
	wr(dev, SE_DMA_RX_IRQ_CLR, UINT32_MAX);
	setbits(dev, SE_GENI_CGC_CTRL, DEFAULT_CGC_EN);
	setbits(dev, SE_DMA_GENERAL_CFG, DMA_CGC_ON);
	wr(dev, GENI_OUTPUT_CTRL, DEFAULT_IO_OUTPUT_CTRL);
	wr(dev, GENI_FORCE_DEFAULT_REG, 1U);
	setbits(dev, SE_IRQ_EN, SE_IRQ_FIFO_EN);
	clrbits(dev, SE_GENI_DMA_MODE_EN, BIT(0));
	wr(dev, SE_GSI_EVENT_EN, 0U);

	depth = FIELD_GET(FIFO_DEPTH, rd(dev, SE_HW_PARAM_1));
	wr(dev, SE_GENI_RX_WATERMARK_REG, 1U);
	wr(dev, SE_GENI_RX_RFR_WATERMARK_REG, depth > 2U ? depth - 2U : 1U);
	setbits(dev, SE_GENI_M_IRQ_EN, M_COMMON_IRQ_EN | M_CMD_DONE | M_TX_FIFO_WATERMARK);
	setbits(dev, SE_GENI_S_IRQ_EN, S_COMMON_IRQ_EN | S_RX_FIFO_WATERMARK | S_RX_FIFO_LAST);
	wr(dev, SE_GENI_TX_WATERMARK_REG, 2U);

	/* 8N1, no flow control. */
	wr(dev, SE_UART_RX_STALE_CNT, STALE_TIMEOUT_BITS);
	wr(dev, SE_UART_LOOPBACK_CFG, 0U);
	wr(dev, SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
	wr(dev, SE_UART_TX_PARITY_CFG, 0U);
	wr(dev, SE_UART_RX_TRANS_CFG, 0U);
	wr(dev, SE_UART_RX_PARITY_CFG, 0U);
	wr(dev, SE_UART_TX_WORD_LEN, BITS_PER_CHAR);
	wr(dev, SE_UART_RX_WORD_LEN, BITS_PER_CHAR);
	wr(dev, SE_UART_TX_STOP_BIT_LEN, 0U);

	wr(dev, SE_GENI_CLK_SEL, 0U);
	wr(dev, GENI_SER_M_CLK_CFG, SER_CLK_EN | FIELD_PREP(CLK_DIV, div));
	wr(dev, GENI_SER_S_CLK_CFG, SER_CLK_EN | FIELD_PREP(CLK_DIV, div));

	wr(dev, SE_GENI_S_CMD0, UART_START_READ << S_OPCODE_SHIFT);
	return 0;
}

static int geni_uart_poll_in(const struct device *dev, unsigned char *c)
{
	struct geni_uart_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	int ret = -1;

	if (data->pending_rx == 0U) {
		data->pending_rx = FIELD_GET(RX_FIFO_WC, rd(dev, SE_GENI_RX_FIFO_STATUS));
	}
	if (data->pending_rx != 0U) {
		*c = (unsigned char)(rd(dev, SE_GENI_RX_FIFOn) & 0xffU);
		data->pending_rx--;
		ret = 0;
	} else {
		wr(dev, SE_GENI_S_IRQ_CLEAR, S_RX_FIFO_WATERMARK | S_RX_FIFO_LAST);
	}
	/* Restart the read if the secondary sequencer stopped (an error or a cancel). */
	if ((rd(dev, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) == 0U) {
		wr(dev, SE_GENI_S_CMD0, UART_START_READ << S_OPCODE_SHIFT);
	}
	k_spin_unlock(&data->lock, key);
	return ret;
}

static void geni_uart_poll_out(const struct device *dev, unsigned char c)
{
	struct geni_uart_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	(void)poll_bits(dev, SE_GENI_STATUS, M_GENI_CMD_ACTIVE, false);
	wr(dev, SE_GENI_M_IRQ_CLEAR, M_CMD_DONE | M_TX_FIFO_WATERMARK);
	wr(dev, SE_UART_TX_TRANS_LEN, 1U);
	wr(dev, SE_GENI_M_CMD0, UART_START_TX << M_OPCODE_SHIFT);
	wr(dev, SE_GENI_TX_FIFOn, c);
	(void)poll_bits(dev, SE_GENI_M_IRQ_STATUS, M_CMD_DONE, true);
	wr(dev, SE_GENI_M_IRQ_CLEAR, M_CMD_DONE | M_TX_FIFO_WATERMARK);
	k_spin_unlock(&data->lock, key);
}

static int geni_uart_init(const struct device *dev)
{
	const struct geni_uart_config *cfg = dev->config;
	uint32_t proto;
	int ret;

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}
	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}
	for (uint8_t i = 0U; i < cfg->num_wrapper_clocks; i++) {
		ret = clock_control_on(cfg->clock_dev, cfg->wrapper_clocks[i]);
		if (ret != 0) {
			return ret;
		}
	}
	ret = clock_control_set_rate(cfg->clock_dev, cfg->se_clock,
				     (clock_control_subsys_rate_t)(uintptr_t)cfg->se_clock_hz);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_on(cfg->clock_dev, cfg->se_clock);
	if (ret != 0) {
		return ret;
	}

	proto = read_proto(dev);
	if (proto == GENI_PROTO_NONE || proto == GENI_PROTO_INVALID) {
#ifdef CONFIG_UART_QCOM_GENI_SE_FW
		struct geni_uart_data *data = dev->data;

		ret = load_uart_fw(dev);
		if (ret != 0) {
			return ret;
		}
		data->fw_loaded = true;
		proto = read_proto(dev);
#else
		return -ENODEV;
#endif
	}
	if (proto != GENI_PROTO_UART) {
		return -ENODEV;
	}
	return geni_uart_setup(dev);
}

static DEVICE_API(uart, geni_uart_api) = {
	.poll_in = geni_uart_poll_in,
	.poll_out = geni_uart_poll_out,
};

#define GENI_WRAPPER_CLOCK(node_id, prop, idx)                                                     \
	(clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL_BY_IDX(node_id, idx, id),

#define GENI_UART_INIT(n)                                                                          \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	static const clock_control_subsys_t geni_uart_wrapper_clocks_##n[] = {                     \
		DT_FOREACH_PROP_ELEM(DT_INST_PARENT(n), clocks, GENI_WRAPPER_CLOCK)};              \
	static const struct geni_uart_config geni_uart_config_##n = {                              \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.wrapper = DT_REG_ADDR(DT_INST_PARENT(n)),                                         \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.se_clock = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL(n, id),        \
		.wrapper_clocks = geni_uart_wrapper_clocks_##n,                                    \
		.num_wrapper_clocks = ARRAY_SIZE(geni_uart_wrapper_clocks_##n),                    \
		.se_clock_hz = DT_INST_PROP(n, clock_frequency),                                   \
		.baud = DT_INST_PROP(n, current_speed),                                            \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
	};                                                                                         \
	static struct geni_uart_data geni_uart_data_##n;                                           \
	DEVICE_DT_INST_DEFINE(n, geni_uart_init, NULL, &geni_uart_data_##n,                        \
			      &geni_uart_config_##n, PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY,    \
			      &geni_uart_api);

DT_INST_FOREACH_STATUS_OKAY(GENI_UART_INIT)
