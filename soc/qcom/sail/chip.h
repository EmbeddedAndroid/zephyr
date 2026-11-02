/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_CHIP_H_
#define ZEPHYR_SOC_QCOM_SAIL_CHIP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SAIL hardware version register; the chip ID is in bits 31:16. */
#define SAIL_HW_VER UINT32_C(0xf0300028)

#define SAIL_CHIP_ID(hw_ver) ((uint16_t)((hw_ver) >> 16))

/* Properties of a SoC carrying the safety island, fixed by the silicon. */
struct sail_chip {
	uint16_t id;
	const char *name;
	/* The fuses hold SAIL PLL5 and PLL6 trims to apply over the defaults. */
	bool pll_fuse_trim;
	/* L value of the CPU PLL (PLL1) as the boot ROM leaves it running. */
	uint8_t cpu_pll1_boot_l;
	/* The SAIL pads the main domain waits on must be configured first. */
	bool md_pads;
};

extern const struct sail_chip sail_chip_monaco;
extern const struct sail_chip sail_chip_lemans;

/* Return the chip for a SAIL_HW_VER value, or NULL if it is not supported. */
const struct sail_chip *sail_chip_find(uint32_t hw_ver);

#endif
