/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "post_pmic_clock.h"

#include <zephyr/sys/util.h>
#include "cpu_pll.h"

#define CPU_CMD       UINT32_C(0xf1124000)
#define ATB_CMD       UINT32_C(0xf1114054)
#define ASILB_CMD     UINT32_C(0xf111001c)
#define DMA_CMD       UINT32_C(0xf1111058)

static int inherited(struct monaco_clock *clock, struct monaco_post_pmic_clock_status *status)
{
	static const struct {
		uint32_t command, fields;
	} initial[] = {
		{CPU_CMD, 0x201},
		{ATB_CMD, 0x104},
		{ASILB_CMD, 0x105},
		{DMA_CMD, 0x102},
	};
	uint32_t value;

	status->stage = MONACO_POST_PMIC_CLOCK_INHERITED;
	for (unsigned int i = 0; i < ARRAY_SIZE(initial); i++) {
		status->address = initial[i].command;
		if (clock->io.read32(clock->io.context, initial[i].command, &value)) {
			return MONACO_CLOCK_IO;
		}
		if (value & 1U) {
			return MONACO_CLOCK_STATE;
		}
		status->address += 4;
		if (clock->io.read32(clock->io.context, initial[i].command + 4, &value)) {
			return MONACO_CLOCK_IO;
		}
		if ((value & 0x371fU) != initial[i].fields) {
			return MONACO_CLOCK_STATE;
		}
	}
	status->address = 0xf1100000;
	return monaco_cpu_pll0_ready(clock);
}

static bool regulator_completed(const struct monaco_pmic_regulator_policy *policy,
				const struct monaco_pmic_regulator_result *rail,
				unsigned int position)
{
	enum monaco_pmic_regulator_group group = position < 4 ? MONACO_PMIC_LDO : MONACO_PMIC_SMPS;
	unsigned int index = position < 4 ? position : position - 4;
	bool needs_enable;

	if (policy->group != group || policy->index != index ||
	    policy->minimum_uv > policy->maximum_uv ||
	    policy->required_mode != (group == MONACO_PMIC_LDO ? 2U : 0U) ||
	    rail->stage != MONACO_PMIC_REGULATOR_COMPLETE || !rail->voltage_valid ||
	    !rail->mode_valid || !rail->enable_valid || rail->voltage_uv < policy->minimum_uv ||
	    rail->voltage_uv > policy->maximum_uv ||
	    (group == MONACO_PMIC_LDO ? rail->observed_mode != 2U : rail->observed_mode > 1U)) {
		return false;
	}
	needs_enable = policy->enable_required && !rail->observed_enabled;
	if (!needs_enable) {
		return !rail->enable_skipped && !rail->enable_sequence_complete;
	}
	if (policy->write_permitted) {
		return rail->enable_sequence_complete && !rail->enable_skipped;
	}
	return rail->enable_skipped && !rail->enable_sequence_complete;
}

int monaco_post_pmic_clocks(struct monaco_clock *clock, const struct monaco_pmic *pmic,
			    const struct monaco_pmic_boot_result *boot,
			    const struct monaco_pmic_regulator_policy *policies,
			    size_t policy_count, struct monaco_post_pmic_clock_status *status)
{
	int rc;

	if (!status) {
		return MONACO_CLOCK_INVALID;
	}
	*status = (struct monaco_post_pmic_clock_status){.stage = MONACO_POST_PMIC_CLOCK_INPUT};
	if (!clock || !clock->io.read32 || !clock->io.write32 || !clock->timer ||
	    !clock->timer->initialized || !clock->max_poll_reads || !pmic || !boot || !policies ||
	    policy_count != MONACO_PMIC_BOOT_REGULATORS || pmic->state != MONACO_PMIC_CRC_READY ||
	    pmic->version.device != 0x82 || boot->status ||
	    boot->stage != MONACO_PMIC_BOOT_COMPLETE ||
	    boot->regulators_completed != MONACO_PMIC_BOOT_REGULATORS ||
	    boot->last_operation != MONACO_PMIC_BOOT_DELAY || boot->last_delay_us != 3000 ||
	    boot->last_callback_status || boot->delay_us_completed < 5000) {
		return MONACO_CLOCK_INVALID;
	}
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; i++) {
		if (!regulator_completed(&policies[i], &boot->regulators[i], i)) {
			return MONACO_CLOCK_INVALID;
		}
	}
	rc = inherited(clock, status);
	if (rc != 0) {
		return rc;
	}
	status->stage = MONACO_POST_PMIC_CLOCK_CPU_SOURCE;
	status->address = 0xf1101000;
	rc = monaco_cpu_pll1_raise(clock);
	if (rc != 0) {
		return rc;
	}
	status->stage = MONACO_POST_PMIC_CLOCK_CPU_DOMAIN;
	status->address = CPU_CMD;
	rc = monaco_clock_rcg_update(clock, CPU_CMD, 2, 2);
	if (rc != 0) {
		return rc;
	}
	status->completed_domains++;
	status->stage = MONACO_POST_PMIC_CLOCK_ATB;
	status->address = ATB_CMD;
	rc = monaco_clock_rcg_update(clock, ATB_CMD, 1, 4);
	if (rc != 0) {
		return rc;
	}
	status->completed_domains++;
	status->stage = MONACO_POST_PMIC_CLOCK_ASILB;
	status->address = ASILB_CMD;
	rc = monaco_clock_rcg_update(clock, ASILB_CMD, 1, 5);
	if (rc != 0) {
		return rc;
	}
	status->completed_domains++;
	status->stage = MONACO_POST_PMIC_CLOCK_DMA_SOURCE;
	status->address = 0xf1104000;
	rc = monaco_cpu_pll4_enable(clock);
	if (rc != 0) {
		return rc;
	}
	status->stage = MONACO_POST_PMIC_CLOCK_DMA_DOMAIN;
	status->address = DMA_CMD;
	rc = monaco_clock_rcg_update(clock, DMA_CMD, 6, 2);
	if (rc != 0) {
		return rc;
	}
	status->completed_domains++;
	status->stage = MONACO_POST_PMIC_CLOCK_COMPLETE;
	status->address = 0;
	return 0;
}
