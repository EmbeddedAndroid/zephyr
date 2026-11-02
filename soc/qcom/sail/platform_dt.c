/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include "platform.h"
#include "pmic_boot.h"

#define DT_DRV_COMPAT qcom_sail_platform

#define PMIC(n) DT_CHILD(DT_DRV_INST(n), pmic)

#define RAIL(node)                                                                                 \
	{                                                                                          \
		.group = DT_ENUM_IDX(node, qcom_group) == 0 ? MONACO_PMIC_LDO : MONACO_PMIC_SMPS,  \
		.index = DT_PROP(node, qcom_index),                                                \
		.write_permitted = DT_PROP(node, qcom_write_permitted),                            \
		.enable_required = DT_PROP(node, qcom_enable_required),                            \
		.required_mode = DT_PROP(node, qcom_mode),                                         \
		.minimum_uv = DT_PROP(node, regulator_min_microvolt),                              \
		.maximum_uv = DT_PROP(node, regulator_max_microvolt),                              \
	},

#define RAILS(n)                                                                                   \
	IF_ENABLED(DT_NODE_EXISTS(PMIC(n)),                                                        \
		   (static const struct monaco_pmic_regulator_policy rails_##n[] = {               \
			    DT_FOREACH_CHILD(PMIC(n), RAIL)};                                      \
		    BUILD_ASSERT(ARRAY_SIZE(rails_##n) == MONACO_PMIC_BOOT_REGULATORS,             \
				 "the SAIL PMIC profile covers nine rails");))

#define ENTRY(n)                                                                                   \
	{                                                                                          \
		.chip_id = DT_INST_PROP(n, qcom_chip_id),                                          \
		.pmic_regulators = COND_CODE_1(DT_NODE_EXISTS(PMIC(n)), (rails_##n), (NULL)),      \
		.pmic_regulator_count =                                                            \
			COND_CODE_1(DT_NODE_EXISTS(PMIC(n)), (ARRAY_SIZE(rails_##n)), (0)),        \
	},

DT_INST_FOREACH_STATUS_OKAY(RAILS)

static const struct sail_platform entries[] = {DT_INST_FOREACH_STATUS_OKAY(ENTRY)};

BUILD_ASSERT(ARRAY_SIZE(entries) > 0, "the board must describe a qcom,sail-platform");

const struct sail_platforms sail_platforms = {entries, ARRAY_SIZE(entries)};
