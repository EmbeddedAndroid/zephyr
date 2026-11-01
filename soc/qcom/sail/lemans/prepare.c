/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "prepare.h"
#include <zephyr/sys/util.h>

#define SAIL_HW_VER     UINT32_C(0xf0300028)
#define LEMANS_HW_ID    UINT32_C(0x6019)
#define TLMM_GPIO_CFG(n) (UINT32_C(0xf0100000) + UINT32_C(0x1000) * (n))
#define PSAIL_ERR_N_PAD 63U
#define SLP_EN_PAD      66U

/* GPIO_CFG fields: pull [1:0], function [5:2], drive [8:6], output enable [9]. */
#define CFG_PULL_UP  UINT32_C(3)
#define CFG_FUNC(f)  ((uint32_t)(f) << 2)
#define CFG_OE       BIT(9)
#define CFG_FIELDS   UINT32_C(0x3ff)

static const struct {
	uint32_t address;
	uint32_t value;
} pads[] = {
	/*
	 * The pad leaves reset on its PSAIL_ERR_N function with a pull-down,
	 * which holds the main domain off; a pulled-up GPIO input releases it.
	 */
	{TLMM_GPIO_CFG(PSAIL_ERR_N_PAD), CFG_PULL_UP},
	{TLMM_GPIO_CFG(SLP_EN_PAD), CFG_FUNC(1) | CFG_OE},
};

bool lemans_prepare_write_allowed(uint32_t address)
{
	for (size_t i = 0; i < ARRAY_SIZE(pads); i++) {
		if (address == pads[i].address) {
			return true;
		}
	}
	return false;
}

int lemans_prepare_run(const struct monaco_timer_io *io)
{
	uint32_t value;

	if (io == NULL || io->read32 == NULL || io->write32 == NULL) {
		return LEMANS_PREPARE_INVALID;
	}
	if (io->read32(io->context, SAIL_HW_VER, &value) != 0) {
		return LEMANS_PREPARE_IO;
	}
	if ((value >> 16) != LEMANS_HW_ID) {
		return LEMANS_PREPARE_IDENTITY;
	}
	for (size_t i = 0; i < ARRAY_SIZE(pads); i++) {
		if (io->write32(io->context, pads[i].address, pads[i].value) != 0 ||
		    io->read32(io->context, pads[i].address, &value) != 0) {
			return LEMANS_PREPARE_IO;
		}
		if ((value & CFG_FIELDS) != pads[i].value) {
			return LEMANS_PREPARE_PAD;
		}
	}
	return 0;
}
