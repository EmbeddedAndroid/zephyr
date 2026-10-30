/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_POLICY_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_POLICY_H_

#include "pmic_boot.h"

enum monaco_pmic_board_profile {
	MONACO_PMIC_BOARD_UNKNOWN,
	MONACO_PMIC_BOARD_VENTUNO_Q,
};

enum monaco_pmic_policy_error {
	MONACO_PMIC_POLICY_PROFILE = -1200,
	MONACO_PMIC_POLICY_IDENTITY = -1201,
	MONACO_PMIC_POLICY_INCOMPLETE = -1202,
};

enum monaco_pmic_policy_stage {
	MONACO_PMIC_POLICY_VALIDATE,
	MONACO_PMIC_POLICY_SKU,
	MONACO_PMIC_POLICY_SOC,
	MONACO_PMIC_POLICY_FSM,
	MONACO_PMIC_POLICY_PROBE,
	MONACO_PMIC_POLICY_SAMPLED,
};

struct monaco_pmic_policy_io {
	void *context;
	/* Ordered Device read; returns 0 or a negative error. */
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	/*
	 * Returns 0 only if the address ACKed, else a negative error. Must return
	 * within timeout_us, including controller cleanup.
	 */
	int (*probe)(void *context, uint8_t address7, uint32_t timeout_us);
};

/* Filled only by monaco_pmic_policy_sample(). */
struct monaco_pmic_policy_snapshot {
	enum monaco_pmic_board_profile profile;
	enum monaco_pmic_policy_stage stage;
	uint32_t raw_sku, raw_soc_identity, raw_reset_fsm;
	uint32_t last_address;
	unsigned int reads_attempted, reads_completed;
	unsigned int probes_attempted, probes_completed;
	int last_callback_status, status;
	bool sampled;
};

struct monaco_pmic_policy_selection {
	struct monaco_pmic_boot_inputs boot;
	uint16_t sku, chip_variant;
	uint32_t chip_version;
	struct monaco_pmic_version pmic_version;
};

/**
 * Read the SKU, SoC identity and reset FSM, then probe the PMICs at 0x28,
 * 0x29 and 0x12 in that order. Only the VENTUNO Q profile and the seven
 * known Monaco SKUs are accepted. The I2C controller must be initialized.
 * A failed call clears any earlier snapshot. Use the snapshot in the same
 * boot, before any controller reset.
 *
 * @retval 0 on success, or a negative MONACO_PMIC_POLICY_ or callback error.
 */
int monaco_pmic_policy_sample(enum monaco_pmic_board_profile profile,
			      const struct monaco_pmic_policy_io *io, uint32_t probe_timeout_us,
			      struct monaco_pmic_policy_snapshot *snapshot);

/**
 * Build the PMIC boot inputs from a sample and an identified PMIC (device ID
 * 0x82); no hardware access. supply_policy_validated is copied unchanged;
 * monaco_pmic_boot() refuses inputs where it is false. Any error clears
 * selection.
 *
 * @retval 0 on success.
 * @retval MONACO_PMIC_BOOT_SKIPPED reset FSM >= 12; selection is still filled.
 * @retval negative MONACO_PMIC_ or MONACO_PMIC_POLICY_ error.
 */
int monaco_pmic_policy_select(const struct monaco_pmic_policy_snapshot *snapshot,
			      const struct monaco_pmic *pmic, bool supply_policy_validated,
			      struct monaco_pmic_policy_selection *selection);

#endif
