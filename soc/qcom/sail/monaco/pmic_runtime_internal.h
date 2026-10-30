/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_RUNTIME_INTERNAL_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_RUNTIME_INTERNAL_H_
#include "pmic_reset.h"

/* Return true when the PMIC state allows a runtime transaction. No side effects. */
bool monaco_pmic_runtime_ready(const struct monaco_pmic *pmic,
			       const struct monaco_pmic_policy_snapshot *snapshot,
			       const struct monaco_pmic_boot_result *boot);
#endif
