/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sail_check.h"
#include "sail_test_platform.h"
#include <stdio.h>
#include <zephyr/sys/util.h>

static void chips(void)
{
	const struct {
		uint32_t hw_ver;
		const struct sail_chip *chip;
	} cases[] = {
		{0x601a0100, &sail_chip_monaco}, {0x601a0000, &sail_chip_monaco},
		{0x601affff, &sail_chip_monaco}, {0x60190200, &sail_chip_lemans},
		{0x60190000, &sail_chip_lemans}, {0x60180000, NULL},
		{0x601b0100, NULL},              {0x0000601a, NULL},
		{0x00006019, NULL},              {0x00000000, NULL},
		{0xffffffff, NULL},              {0x1a600100, NULL},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		sail_check(sail_chip_find(cases[i].hw_ver) == cases[i].chip);
	}
	sail_check(sail_chip_monaco.id == 0x601a && sail_chip_monaco.pll_fuse_trim &&
		   sail_chip_monaco.cpu_pll1_boot_l == 83 && !sail_chip_monaco.md_pads);
	sail_check(sail_chip_lemans.id == 0x6019 && !sail_chip_lemans.pll_fuse_trim &&
		   sail_chip_lemans.cpu_pll1_boot_l == 80 && sail_chip_lemans.md_pads);
}

static void platforms(void)
{
	static const struct sail_platform twice[] = {
		{0x6019, NULL, 0},
		{0x601a, sail_test_ventuno_q_rails, MONACO_PMIC_BOOT_REGULATORS},
		{0x6019, NULL, 0},
	};
	const struct sail_platforms duplicate = {twice, ARRAY_SIZE(twice)};
	const struct sail_platforms empty = {twice, 0};
	const struct sail_platforms missing = {NULL, 2};

	sail_check(sail_platform_find(&sail_test_platforms, 0x601a) ==
		   &sail_test_platforms.entries[0]);
	sail_check(sail_platform_find(&sail_test_platforms, 0x6019) ==
		   &sail_test_platforms.entries[1]);
	sail_check(sail_platform_find(&sail_test_platforms, 0x6018) == NULL);
	sail_check(sail_platform_find(&duplicate, 0x6019) == NULL);
	sail_check(sail_platform_find(&duplicate, 0x601a) == &twice[1]);
	sail_check(sail_platform_find(&empty, 0x6019) == NULL);
	sail_check(sail_platform_find(&missing, 0x6019) == NULL);
	sail_check(sail_platform_find(NULL, 0x601a) == NULL);
}

int sail_model_test(void)
{
	chips();
	platforms();
	puts("platform: chip IDs from SAIL_HW_VER, one platform per chip, unknown chips, "
	     "missing and duplicate platforms refused");
	return 0;
}
