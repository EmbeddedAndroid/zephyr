/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chip.h"
#include <zephyr/sys/util.h>

const struct sail_chip sail_chip_monaco = {
	.id = 0x601a,
	.name = "Monaco",
	.pll_fuse_trim = true,
	.cpu_pll1_boot_l = 83,
	.md_pads = false,
};

/* No SAIL PLL trims are fused, and the boot ROM leaves PLL1 at 1536 MHz. */
const struct sail_chip sail_chip_lemans = {
	.id = 0x6019,
	.name = "Lemans",
	.pll_fuse_trim = false,
	.cpu_pll1_boot_l = 80,
	.md_pads = true,
};

static const struct sail_chip *const chips[] = {
	&sail_chip_monaco,
	&sail_chip_lemans,
};

const struct sail_chip *sail_chip_find(uint32_t hw_ver)
{
	for (size_t i = 0; i < ARRAY_SIZE(chips); i++) {
		if (chips[i]->id == SAIL_CHIP_ID(hw_ver)) {
			return chips[i];
		}
	}
	return NULL;
}
