/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_sail_cc

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/qcom-sail-cc.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>

#define CBCR_CLK_ENABLE BIT(0)
#define CBCR_CLK_OFF    BIT(31)

#define RCG_CMD        0x00U
#define RCG_CFG        0x04U
#define RCG_M          0x08U
#define RCG_N          0x0cU
#define RCG_D          0x10U
#define RCG_CMD_UPDATE BIT(0)
#define RCG_CFG_DIV    GENMASK(4, 0)
#define RCG_CFG_SRC    GENMASK(10, 8)
#define RCG_CFG_MODE   GENMASK(13, 12)
#define RCG_MODE_DUAL_EDGE 2U
#define RCG_MND_MASK   0xffffU

#define DFSR_DFS_EN    BIT(0)

#define XO_HZ          19200000U
#define SRC_XO         0U
#define SRC_PLL0_MAIN  1U

#define POLL_LOOPS     100000U

struct sail_cc_clock {
	uint16_t cbcr;
	uint16_t rcg;
	uint16_t dfsr;
};

struct sail_cc_config {
	mem_addr_t base;
	uint32_t pll0_hz;
};

struct sail_cc_data {
	uint32_t rate[SAIL_CC_QUPV3_WRAP0_S4_CLK + 1];
};

static const struct sail_cc_clock clocks[] = {
	[SAIL_CC_QUPV3_WRAP0_M_AHB_CLK] = {0x5004, 0, 0},
	[SAIL_CC_QUPV3_WRAP0_S_AHB_CLK] = {0x5008, 0, 0},
	[SAIL_CC_QUPV3_WRAP0_CORE_CLK] = {0x500c, 0, 0},
	[SAIL_CC_QUPV3_WRAP0_CORE_2X_CLK] = {0x5018, 0, 0},
	[SAIL_CC_QUPV3_WRAP0_S0_CLK] = {0x5038, 0x5040, 0x5054},
	[SAIL_CC_QUPV3_WRAP0_S1_CLK] = {0x516c, 0x5174, 0x5188},
	[SAIL_CC_QUPV3_WRAP0_S2_CLK] = {0x52a0, 0x52a8, 0x52bc},
	[SAIL_CC_QUPV3_WRAP0_S3_CLK] = {0x53d4, 0x53dc, 0x53f0},
	[SAIL_CC_QUPV3_WRAP0_S4_CLK] = {0x5508, 0x5510, 0x5524},
};

static const struct sail_cc_clock *lookup(clock_control_subsys_t sys)
{
	uintptr_t id = (uintptr_t)sys;

	return id < ARRAY_SIZE(clocks) ? &clocks[id] : NULL;
}

static bool poll_clear(mem_addr_t reg, uint32_t mask)
{
	for (uint32_t i = 0U; i < POLL_LOOPS; i++) {
		if ((sys_read32(reg) & mask) == 0U) {
			return true;
		}
	}
	return false;
}

static int sail_cc_on(const struct device *dev, clock_control_subsys_t sys)
{
	const struct sail_cc_config *cfg = dev->config;
	const struct sail_cc_clock *clk = lookup(sys);
	mem_addr_t cbcr;

	if (clk == NULL) {
		return -EINVAL;
	}
	cbcr = cfg->base + clk->cbcr;
	sys_set_bits(cbcr, CBCR_CLK_ENABLE);
	return poll_clear(cbcr, CBCR_CLK_OFF) ? 0 : -ETIMEDOUT;
}

static int sail_cc_off(const struct device *dev, clock_control_subsys_t sys)
{
	const struct sail_cc_config *cfg = dev->config;
	const struct sail_cc_clock *clk = lookup(sys);

	if (clk == NULL) {
		return -EINVAL;
	}
	sys_clear_bits(cfg->base + clk->cbcr, CBCR_CLK_ENABLE);
	return 0;
}

static uint32_t gcd32(uint32_t a, uint32_t b)
{
	while (b != 0U) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

/* Exact M/N for rate = src * m / n with the 16-bit MND counter, or false. */
static bool mnd_for(uint32_t src, uint32_t rate, uint32_t *m, uint32_t *n)
{
	uint32_t g;

	if (rate == 0U || rate > src) {
		return false;
	}
	g = gcd32(src, rate);
	*m = rate / g;
	*n = src / g;
	return *n <= RCG_MND_MASK;
}

static int sail_cc_set_rate(const struct device *dev, clock_control_subsys_t sys,
			    clock_control_subsys_rate_t rate_arg)
{
	const struct sail_cc_config *cfg = dev->config;
	struct sail_cc_data *data = dev->data;
	const struct sail_cc_clock *clk = lookup(sys);
	uint32_t rate = (uint32_t)(uintptr_t)rate_arg;
	uint32_t src, m, n, d, value;
	mem_addr_t rcg;

	if (clk == NULL || clk->rcg == 0U) {
		return -ENOTSUP;
	}
	if (mnd_for(XO_HZ, rate, &m, &n)) {
		src = SRC_XO;
	} else if (mnd_for(cfg->pll0_hz, rate, &m, &n)) {
		src = SRC_PLL0_MAIN;
	} else {
		return -ERANGE;
	}

	/* With DFS on, the root ignores CFG and runs from the performance table. */
	if ((sys_read32(cfg->base + clk->dfsr) & DFSR_DFS_EN) != 0U) {
		sys_clear_bits(cfg->base + clk->dfsr, DFSR_DFS_EN);
	}

	rcg = cfg->base + clk->rcg;
	if (m == n) {
		value = FIELD_PREP(RCG_CFG_SRC, src);
	} else {
		/* 50% duty: 2d = n, clamped to [m, 2(n - m)]. */
		d = CLAMP(n, m, 2U * (n - m));
		sys_write32(m, rcg + RCG_M);
		sys_write32(~(n - m) & RCG_MND_MASK, rcg + RCG_N);
		sys_write32(~d & RCG_MND_MASK, rcg + RCG_D);
		value = FIELD_PREP(RCG_CFG_SRC, src) | FIELD_PREP(RCG_CFG_MODE, RCG_MODE_DUAL_EDGE);
	}
	sys_write32((sys_read32(rcg + RCG_CFG) & ~(RCG_CFG_DIV | RCG_CFG_SRC | RCG_CFG_MODE)) |
		    value, rcg + RCG_CFG);
	sys_set_bits(rcg + RCG_CMD, RCG_CMD_UPDATE);
	if (!poll_clear(rcg + RCG_CMD, RCG_CMD_UPDATE)) {
		return -ETIMEDOUT;
	}
	data->rate[(uintptr_t)sys] = rate;
	return 0;
}

static int sail_cc_get_rate(const struct device *dev, clock_control_subsys_t sys, uint32_t *rate)
{
	struct sail_cc_data *data = dev->data;
	const struct sail_cc_clock *clk = lookup(sys);

	if (clk == NULL) {
		return -EINVAL;
	}
	if (data->rate[(uintptr_t)sys] == 0U) {
		return -EAGAIN;
	}
	*rate = data->rate[(uintptr_t)sys];
	return 0;
}

static DEVICE_API(clock_control, sail_cc_api) = {
	.on = sail_cc_on,
	.off = sail_cc_off,
	.get_rate = sail_cc_get_rate,
	.set_rate = sail_cc_set_rate,
};

#define SAIL_CC_INIT(n)                                                                            \
	static const struct sail_cc_config sail_cc_config_##n = {                                  \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.pll0_hz = DT_INST_PROP(n, pll0_out_main_frequency),                               \
	};                                                                                         \
	static struct sail_cc_data sail_cc_data_##n;                                               \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &sail_cc_data_##n, &sail_cc_config_##n,               \
			      PRE_KERNEL_1, CONFIG_CLOCK_CONTROL_INIT_PRIORITY, &sail_cc_api);

DT_INST_FOREACH_STATUS_OKAY(SAIL_CC_INIT)
