/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_sail_tlmm

#include <errno.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>

#define TLMM_BASE      DT_INST_REG_ADDR(0)
#define TLMM_PADS      (DT_INST_REG_SIZE(0) / TLMM_PAD_STRIDE)
#define TLMM_PAD_STRIDE 0x1000U

#define GPIO_CFG_PULL     GENMASK(1, 0)
#define GPIO_CFG_FUNC     GENMASK(5, 2)
#define GPIO_CFG_DRIVE    GENMASK(8, 6)

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0U; i < pin_cnt; i++) {
		const pinctrl_soc_pin_t *pin = &pins[i];
		mem_addr_t cfg = TLMM_BASE + (mem_addr_t)pin->pad * TLMM_PAD_STRIDE;
		uint32_t value;

		if (pin->pad >= TLMM_PADS || pin->func > 15U) {
			return -EINVAL;
		}
		value = sys_read32(cfg) & ~(GPIO_CFG_PULL | GPIO_CFG_FUNC | GPIO_CFG_DRIVE);
		value |= FIELD_PREP(GPIO_CFG_PULL, pin->pull);
		value |= FIELD_PREP(GPIO_CFG_FUNC, pin->func);
		value |= FIELD_PREP(GPIO_CFG_DRIVE, (pin->drive_ma / 2U) - 1U);
		sys_write32(value, cfg);
	}

	return 0;
}
