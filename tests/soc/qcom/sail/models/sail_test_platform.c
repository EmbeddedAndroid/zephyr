/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sail_test_platform.h"
#include <zephyr/sys/util.h>

const struct monaco_pmic_regulator_policy sail_test_ventuno_q_rails[] = {
	{MONACO_PMIC_LDO, 0, true, true, 2, 600000, 3300000},
	{MONACO_PMIC_LDO, 1, true, false, 2, 600000, 3300000},
	{MONACO_PMIC_LDO, 2, false, false, 2, 600000, 3300000},
	{MONACO_PMIC_LDO, 3, true, true, 2, 1200000, 3300000},
	{MONACO_PMIC_SMPS, 0, false, false, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 1, false, true, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 2, true, true, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 3, true, true, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 4, true, true, 0, 300000, 3340000},
};

static const struct sail_platform entries[] = {
	{0x601a, sail_test_ventuno_q_rails, ARRAY_SIZE(sail_test_ventuno_q_rails)},
	{0x6019, NULL, 0},
};

const struct sail_platforms sail_test_platforms = {entries, ARRAY_SIZE(entries)};
