/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_boot.h"

struct boot {
	struct monaco_pmic pmic;
	struct monaco_pmic_io transport;
	struct monaco_pmic_boot_result *result;
};

static int trace_transfer(void *context, uint8_t address7, const uint8_t *tx, size_t tx_size,
			  uint8_t *rx, size_t rx_size, uint32_t timeout_us)
{
	struct boot *boot = context;
	struct monaco_pmic_boot_result *r = boot->result;
	int rc;

	r->last_operation = rx_size ? MONACO_PMIC_BOOT_READ : MONACO_PMIC_BOOT_WRITE;
	r->last_address7 = address7;
	r->last_register = tx[0];
	r->last_delay_us = 0;
	r->operations_attempted++;
	rc = boot->transport.transfer(boot->transport.context, address7, tx, tx_size, rx, rx_size,
				      timeout_us);
	r->last_callback_status = rc;
	if (rc == 0) {
		r->operations_completed++;
	}
	return rc;
}

static int trace_delay(void *context, uint32_t us)
{
	struct boot *boot = context;
	struct monaco_pmic_boot_result *r = boot->result;
	int rc;

	r->last_operation = MONACO_PMIC_BOOT_DELAY;
	r->last_address7 = 0;
	r->last_register = 0;
	r->last_delay_us = us;
	r->operations_attempted++;
	rc = boot->transport.delay_us(boot->transport.context, us);
	r->last_callback_status = rc;
	if (rc == 0) {
		r->operations_completed++;
		r->delay_us_completed += us;
	}
	return rc;
}

static int delay(struct boot *boot, uint32_t us)
{
	int rc = trace_delay(boot, us);

	return rc > 0 ? MONACO_PMIC_CALLBACK : rc;
}

static int read_reg(struct boot *boot, unsigned int slave, uint8_t reg, uint8_t *value)
{
	return monaco_pmic_read(&boot->pmic, slave, reg, value);
}

static int write_reg(struct boot *boot, unsigned int slave, uint8_t reg, uint8_t value)
{
	return monaco_pmic_write(&boot->pmic, slave, reg, value);
}

static int update(struct boot *boot, uint8_t reg, uint8_t mask, uint8_t value)
{
	return monaco_pmic_update_bits(&boot->pmic, 0, reg, mask, value);
}

static int writeback(struct boot *boot, unsigned int slave, uint8_t reg)
{
	int rc;
	uint8_t value;

	rc = read_reg(boot, slave, reg, &value);
	if (rc != 0) {
		return rc;
	}
	return write_reg(boot, slave, reg, value);
}

static int collect_status(struct boot *boot)
{
	int rc;
	struct monaco_pmic_boot_result *r = boot->result;
	uint8_t top, child;

	rc = read_reg(boot, 0, 0x5a, &top);
	if (rc != 0) {
		return rc;
	}
	r->status_initial = top;
	r->status_initial_valid = true;
	if (top & 0x80U) {
		rc = read_reg(boot, 0, 0x69, &child);
		if (rc != 0) {
			return rc;
		}
		if (child & 0x80U) {
			rc = writeback(boot, 2, 0x08);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x40U) {
			rc = writeback(boot, 0, 0x6c);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x20U) {
			rc = writeback(boot, 0, 0x6b);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x10U) {
			rc = writeback(boot, 0, 0x6a);
			if (rc != 0) {
				return rc;
			}
		}
		rc = writeback(boot, 0, 0x69);
		if (rc != 0) {
			return rc;
		}
	}
	for (unsigned int bit = 0; bit < 4; ++bit) {
		if (top & (0x40U >> bit)) {
			rc = writeback(boot, 0, (uint8_t)(0x68U - bit));
			if (rc != 0) {
				return rc;
			}
		}
	}
	if (top & 0x04U) {
		rc = read_reg(boot, 0, 0x63, &child);
		if (rc != 0) {
			return rc;
		}
		if (child & 0x08U) {
			rc = writeback(boot, 0, 0x64);
			if (rc != 0) {
				return rc;
			}
		}
		rc = writeback(boot, 0, 0x63);
		if (rc != 0) {
			return rc;
		}
	}
	if (top & 0x02U) {
		rc = read_reg(boot, 0, 0x5f, &child);
		if (rc != 0) {
			return rc;
		}
		if (child & 0x10U) {
			rc = writeback(boot, 0, 0x62);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x02U) {
			rc = writeback(boot, 0, 0x61);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x01U) {
			rc = writeback(boot, 0, 0x60);
			if (rc != 0) {
				return rc;
			}
		}
	}
	if (top & 0x01U) {
		rc = read_reg(boot, 0, 0x5b, &child);
		if (rc != 0) {
			return rc;
		}
		if (child & 0x04U) {
			rc = writeback(boot, 0, 0x5e);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x02U) {
			rc = writeback(boot, 0, 0x5d);
			if (rc != 0) {
				return rc;
			}
		}
		if (child & 0x01U) {
			rc = writeback(boot, 0, 0x5c);
			if (rc != 0) {
				return rc;
			}
		}
	}
	rc = read_reg(boot, 0, 0x5a, &r->status_residual);
	if (rc != 0) {
		return rc;
	}
	r->status_residual_valid = true;
	return 0;
}

static bool bist_accepted(const struct boot *boot, bool first, bool second)
{
	return first && (second || (boot->pmic.version.firmware == 1 &&
				    boot->pmic.version.firmware_revision == 0));
}

static int bist_flags(struct boot *boot, bool *first, bool *second)
{
	int rc;
	uint8_t value;

	rc = read_reg(boot, 0, 0x66, &value);
	if (rc != 0) {
		return rc;
	}
	*first = (value & 1U) != 0;
	rc = read_reg(boot, 0, 0x81, &value);
	if (rc != 0) {
		return rc;
	}
	*second = (value & 8U) != 0;
	return 0;
}

static int bist_pair(struct boot *boot, uint8_t base, unsigned int index, bool enable)
{
	int rc;
	uint8_t first = (index & 1U) ? 0x20 : 0x02;
	uint8_t second = (index & 1U) ? 0x10 : 0x01;
	uint8_t reg = (uint8_t)(base + index / 2U);

	rc = update(boot, reg, first, enable ? first : 0);
	if (rc != 0) {
		return rc;
	}
	return update(boot, reg, second, enable ? second : 0);
}

static int runtime_bist(struct boot *boot)
{
	int rc;
	struct monaco_pmic_boot_result *r = boot->result;
	uint8_t value;

	r->runtime_bist_attempted = true;
	r->stage = MONACO_PMIC_BOOT_BIST_ISOLATE_LDO;
	for (unsigned int i = 0; i < 4; ++i) {
		r->rail_index = i;
		rc = read_reg(boot, 0, (uint8_t)(0x1dU + i), &value);
		if (rc != 0) {
			return rc;
		}
		if (value & 1U) {
			rc = update(boot, (uint8_t)(0x1dU + i), 0x10, 0);
			if (rc != 0) {
				return rc;
			}
		}
	}
	r->stage = MONACO_PMIC_BOOT_BIST_GATE;
	rc = update(boot, 0x4e, 0x02, 0x02);
	if (rc != 0) {
		return rc;
	}
	rc = update(boot, 0x4e, 0x01, 0x01);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_BIST_SMPS;
	for (unsigned int i = 0; i < 5; ++i) {
		r->rail_index = i;
		rc = bist_pair(boot, 0x49, i, true);
		if (rc != 0) {
			return rc;
		}
	}
	r->stage = MONACO_PMIC_BOOT_BIST_LDO;
	for (unsigned int i = 0; i < 4; ++i) {
		r->rail_index = i;
		rc = bist_pair(boot, 0x4c, i, true);
		if (rc != 0) {
			return rc;
		}
	}
	r->stage = MONACO_PMIC_BOOT_BIST_RUN;
	rc = write_reg(boot, 0, 0x85, 0x04);
	if (rc != 0) {
		return rc;
	}
	rc = delay(boot, 1800);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_BIST_VERIFY;
	rc = bist_flags(boot, &r->runtime_bist_flag, &r->runtime_bist_second_flag);
	if (rc != 0) {
		return rc;
	}
	r->runtime_bist_flags_valid = true;
	if (!bist_accepted(boot, r->runtime_bist_flag, r->runtime_bist_second_flag)) {
		return MONACO_PMIC_BOOT_BIST_FAILED;
	}
	r->runtime_bist_passed = true;
	r->stage = MONACO_PMIC_BOOT_BIST_UNGATE;
	rc = update(boot, 0x4e, 0x02, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(boot, 0x4e, 0x01, 0);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_BIST_RESTORE_SMPS;
	for (unsigned int i = 0; i < 5; ++i) {
		r->rail_index = i;
		rc = bist_pair(boot, 0x49, i, false);
		if (rc != 0) {
			return rc;
		}
	}
	r->stage = MONACO_PMIC_BOOT_BIST_RESTORE_LDO;
	for (unsigned int i = 0; i < 4; ++i) {
		r->rail_index = i;
		rc = read_reg(boot, 0, (uint8_t)(0x1dU + i), &value);
		if (rc != 0) {
			return rc;
		}
		if (value & 1U) {
			rc = update(boot, (uint8_t)(0x1dU + i), 0x10, 0x10);
			if (rc != 0) {
				return rc;
			}
		}
	}
	r->stage = MONACO_PMIC_BOOT_BIST_SETTLE_LDO;
	for (unsigned int i = 0; i < 4; ++i) {
		r->rail_index = i;
		rc = read_reg(boot, 0, (uint8_t)(0x1dU + i), &value);
		if (rc != 0) {
			return rc;
		}
		rc = delay(boot, (value & 2U) ? 3500U : 1000U);
		if (rc != 0) {
			return rc;
		}
		rc = bist_pair(boot, 0x4c, i, false);
		if (rc != 0) {
			return rc;
		}
	}
	r->runtime_bist_cleanup_complete = true;
	return 0;
}

static int execute_pretarget(struct boot *boot, uint32_t platform_type,
			     enum monaco_pmic_bist_policy bist_policy)
{
	int rc;
	struct monaco_pmic_boot_result *r = boot->result;

	r->stage = MONACO_PMIC_BOOT_SUPPORT;
	rc = monaco_pmic_enable_crc(&boot->pmic);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_PLATFORM;
	if (platform_type == 0x0f) {
		rc = update(boot, 0x0d, 0x07, 0x05);
		if (rc != 0) {
			return rc;
		}
	}
	r->stage = MONACO_PMIC_BOOT_PARAMETERS;
	/* Code 15 is 600000 uV, in 5000 uV steps. */
	rc = write_reg(boot, 0, 0xcd, (820000U - 600000U) / 5000U + 15U);
	if (rc != 0) {
		return rc;
	}
	rc = write_reg(boot, 0, 0xce, (820000U - 600000U) / 5000U + 15U);
	if (rc != 0) {
		return rc;
	}
	rc = write_reg(boot, 0, 0xcf, (890000U - 600000U) / 5000U + 15U);
	if (rc != 0) {
		return rc;
	}
	rc = write_reg(boot, 0, 0xd0, (890000U - 600000U) / 5000U + 15U);
	if (rc != 0) {
		return rc;
	}
	rc = update(boot, 0xca, 0x0f, 0x03);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_FLAGS;
	rc = bist_flags(boot, &r->boot_bist_flag, &r->boot_bist_second_flag);
	if (rc != 0) {
		return rc;
	}
	r->boot_bist_flags_valid = true;
	r->stage = MONACO_PMIC_BOOT_STATUS;
	rc = collect_status(boot);
	if (rc != 0) {
		return rc;
	}
	r->boot_bist_accepted = bist_accepted(boot, r->boot_bist_flag, r->boot_bist_second_flag);
	if (!r->boot_bist_accepted) {
		r->runtime_bist_required = true;
		if (bist_policy == MONACO_PMIC_REQUIRE_BOOT_BIST) {
			r->stage = MONACO_PMIC_BOOT_BIST_REQUIRED;
			return MONACO_PMIC_BOOT_RUNTIME_BIST_REQUIRED;
		}
		rc = runtime_bist(boot);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int execute_tail(struct boot *boot, const struct monaco_pmic_regulator_policy *regulators)
{
	int rc;
	struct monaco_pmic_boot_result *r = boot->result;

	r->stage = MONACO_PMIC_BOOT_REGULATOR;
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; ++i) {
		r->rail_index = i;
		rc = monaco_pmic_regulator_init(&boot->pmic, &regulators[i], &r->regulators[i]);
		if (rc != 0) {
			return rc;
		}
		r->regulators_completed++;
	}
	r->stage = MONACO_PMIC_BOOT_POSTTARGET;
	/* Selects reset policy type 6; this does not trigger a reset or power off. */
	rc = write_reg(boot, 0, 0xc9, 0x00);
	if (rc != 0) {
		return rc;
	}
	r->posttarget_writes_completed++;
	rc = write_reg(boot, 0, 0xcb, 0x0a);
	if (rc != 0) {
		return rc;
	}
	r->posttarget_writes_completed++;
	rc = write_reg(boot, 0, 0xcc, 0x00);
	if (rc != 0) {
		return rc;
	}
	r->posttarget_writes_completed++;
	rc = write_reg(boot, 0, 0x4f, 0xff);
	if (rc != 0) {
		return rc;
	}
	r->posttarget_writes_completed++;
	rc = write_reg(boot, 0, 0x50, 0xff);
	if (rc != 0) {
		return rc;
	}
	r->posttarget_writes_completed++;
	rc = write_reg(boot, 0, 0x51, 0x3f);
	if (rc != 0) {
		return rc;
	}
	r->posttarget_writes_completed++;
	return 0;
}

static int execute(struct boot *boot, const struct monaco_pmic_boot_inputs *inputs)
{
	int rc;
	struct monaco_pmic_boot_result *r = boot->result;

	rc = execute_pretarget(boot, inputs->platform_type, MONACO_PMIC_ALLOW_RUNTIME_BIST);
	if (rc != 0) {
		return rc;
	}
	rc = execute_tail(boot, inputs->regulators);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_SETTLE;
	rc = delay(boot, 3000);
	if (rc != 0) {
		return rc;
	}
	r->stage = MONACO_PMIC_BOOT_COMPLETE;
	return 0;
}

static int validate_regulators(const struct monaco_pmic_regulator_policy *policies, size_t count)
{
	if (!policies || count != MONACO_PMIC_BOOT_REGULATORS) {
		return MONACO_PMIC_INVALID;
	}
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; ++i) {
		const struct monaco_pmic_regulator_policy *p = &policies[i];
		bool ldo = i < 4;

		if (p->group != (ldo ? MONACO_PMIC_LDO : MONACO_PMIC_SMPS) ||
		    p->index != (ldo ? i : i - 4U) || p->minimum_uv > p->maximum_uv ||
		    p->required_mode != (ldo ? 2U : 0U)) {
			return MONACO_PMIC_INVALID;
		}
	}
	return 0;
}

static int validate(const struct monaco_pmic *pmic, const struct monaco_pmic_boot_inputs *inputs)
{
	if (!pmic || !pmic->io.transfer || !pmic->io.delay_us || !pmic->transfer_timeout_us ||
	    !inputs) {
		return MONACO_PMIC_INVALID;
	}
	if (inputs->topology != MONACO_PMIC_TOPOLOGY_INDEPENDENT ||
	    inputs->present_slaves != 0x07) {
		return MONACO_PMIC_BOOT_TOPOLOGY;
	}
	if (!inputs->reset_fsm_valid || !inputs->platform_type_valid ||
	    !inputs->supply_policy_validated || !inputs->regulators ||
	    inputs->regulator_count != MONACO_PMIC_BOOT_REGULATORS) {
		return MONACO_PMIC_INVALID;
	}
	if (validate_regulators(inputs->regulators, inputs->regulator_count)) {
		return MONACO_PMIC_INVALID;
	}
	if (pmic->state != MONACO_PMIC_IDENTIFIED || pmic->version.device != 0x82) {
		return MONACO_PMIC_STATE;
	}
	return 0;
}

int monaco_pmic_boot(struct monaco_pmic *pmic, const struct monaco_pmic_boot_inputs *inputs,
		     struct monaco_pmic_boot_result *result)
{
	struct boot boot;
	int rc;

	if (!result) {
		return MONACO_PMIC_INVALID;
	}
	*result = (struct monaco_pmic_boot_result){0};
	rc = validate(pmic, inputs);
	if (rc != 0) {
		result->status = rc;
		return rc;
	}
	if (inputs->reset_fsm >= 12) {
		result->stage = MONACO_PMIC_BOOT_SKIP_COLD;
		result->status = MONACO_PMIC_BOOT_SKIPPED;
		return MONACO_PMIC_BOOT_SKIPPED;
	}
	boot = (struct boot){.pmic = *pmic, .transport = pmic->io, .result = result};
	boot.pmic.io = (struct monaco_pmic_io){&boot, trace_transfer, trace_delay};
	rc = execute(&boot, inputs);
	/* Copy back only the protocol state; the caller's I/O callbacks are unchanged. */
	pmic->state = boot.pmic.state;
	result->status = rc;
	return rc;
}

int monaco_pmic_pretarget(struct monaco_pmic *pmic,
			  const struct monaco_pmic_pretarget_inputs *inputs,
			  enum monaco_pmic_bist_policy bist_policy,
			  struct monaco_pmic_boot_result *result)
{
	struct boot boot;
	int rc = 0;

	if (!result) {
		return MONACO_PMIC_INVALID;
	}
	*result = (struct monaco_pmic_boot_result){0};
	if (!pmic || !pmic->io.transfer || !pmic->io.delay_us || !pmic->transfer_timeout_us ||
	    !inputs) {
		rc = MONACO_PMIC_INVALID;
	} else if (inputs->topology != MONACO_PMIC_TOPOLOGY_INDEPENDENT ||
		   inputs->present_slaves != 7) {
		rc = MONACO_PMIC_BOOT_TOPOLOGY;
	} else if (!inputs->reset_fsm_valid || !inputs->platform_type_valid ||
		   !inputs->parameter_status_policy_validated ||
		   (bist_policy != MONACO_PMIC_REQUIRE_BOOT_BIST &&
		    bist_policy != MONACO_PMIC_ALLOW_RUNTIME_BIST) ||
		   (bist_policy == MONACO_PMIC_ALLOW_RUNTIME_BIST &&
		    !inputs->runtime_bist_policy_validated)) {
		rc = MONACO_PMIC_INVALID;
	} else if (pmic->state != MONACO_PMIC_IDENTIFIED || pmic->version.device != 0x82) {
		rc = MONACO_PMIC_STATE;
	} else if (inputs->reset_fsm >= 12) {
		result->stage = MONACO_PMIC_BOOT_SKIP_COLD;
		rc = MONACO_PMIC_BOOT_SKIPPED;
	}
	if (rc != 0) {
		result->status = rc;
		return rc;
	}
	boot = (struct boot){.pmic = *pmic, .transport = pmic->io, .result = result};
	boot.pmic.io = (struct monaco_pmic_io){&boot, trace_transfer, trace_delay};
	rc = execute_pretarget(&boot, inputs->platform_type, bist_policy);
	pmic->state = boot.pmic.state;
	if (rc == 0) {
		result->stage = MONACO_PMIC_BOOT_PRETARGET_COMPLETE;
	}
	result->status = rc;
	return rc;
}

int monaco_pmic_boot_tail(struct monaco_pmic *pmic, const struct monaco_pmic_tail_inputs *inputs,
			  struct monaco_pmic_boot_result *result)
{
	struct boot boot;
	int rc;

	if (!result) {
		return MONACO_PMIC_INVALID;
	}
	if (!pmic || !pmic->io.transfer || !pmic->io.delay_us || !pmic->transfer_timeout_us ||
	    !inputs || !inputs->regulator_posttarget_policy_validated ||
	    validate_regulators(inputs->regulators, inputs->regulator_count)) {
		result->status = MONACO_PMIC_INVALID;
		return result->status;
	}
	if (pmic->state != MONACO_PMIC_CRC_READY || pmic->version.device != 0x82 ||
	    result->stage != MONACO_PMIC_BOOT_PRETARGET_COMPLETE || result->status ||
	    !result->boot_bist_flags_valid ||
	    (!result->boot_bist_accepted &&
	     (!result->runtime_bist_passed || !result->runtime_bist_cleanup_complete)) ||
	    result->regulators_completed || result->posttarget_writes_completed) {
		result->status = MONACO_PMIC_STATE;
		return result->status;
	}
	boot = (struct boot){.pmic = *pmic, .transport = pmic->io, .result = result};
	boot.pmic.io = (struct monaco_pmic_io){&boot, trace_transfer, trace_delay};
	rc = execute_tail(&boot, inputs->regulators);
	pmic->state = boot.pmic.state;
	if (rc == 0) {
		result->stage = MONACO_PMIC_BOOT_TAIL_COMPLETE;
		result->tail_owner = pmic;
		result->tail_result = result;
		result->tail_context = *pmic;
	}
	result->status = rc;
	return rc;
}

static int tail_ready(const struct monaco_pmic *pmic, const struct monaco_pmic_boot_result *result)
{
	const struct monaco_pmic *saved = &result->tail_context;

	if (result->tail_owner != pmic || result->tail_result != result ||
	    result->stage != MONACO_PMIC_BOOT_TAIL_COMPLETE || result->status ||
	    pmic->state != MONACO_PMIC_CRC_READY || saved->state != MONACO_PMIC_CRC_READY ||
	    pmic->version.device != 0x82 || pmic->io.context != saved->io.context ||
	    pmic->io.transfer != saved->io.transfer || pmic->io.delay_us != saved->io.delay_us ||
	    pmic->transfer_timeout_us != saved->transfer_timeout_us ||
	    pmic->version.device != saved->version.device ||
	    pmic->version.revision != saved->version.revision ||
	    pmic->version.firmware != saved->version.firmware ||
	    pmic->version.firmware_revision != saved->version.firmware_revision ||
	    !result->boot_bist_flags_valid ||
	    (!result->boot_bist_accepted &&
	     (!result->runtime_bist_passed || !result->runtime_bist_cleanup_complete)) ||
	    result->regulators_completed != MONACO_PMIC_BOOT_REGULATORS ||
	    result->posttarget_writes_completed != 6 ||
	    result->operations_attempted != result->operations_completed ||
	    result->last_callback_status || result->settle_time_reads_attempted ||
	    result->settle_time_reads_completed || result->settle_elapsed_valid ||
	    result->settle_elapsed_us) {
		return MONACO_PMIC_STATE;
	}
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; ++i) {
		const struct monaco_pmic_regulator_result *rail = &result->regulators[i];

		if (rail->stage != MONACO_PMIC_REGULATOR_COMPLETE || !rail->voltage_valid ||
		    !rail->mode_valid || !rail->enable_valid) {
			return MONACO_PMIC_STATE;
		}
	}
	return 0;
}

static int settle_time(const struct monaco_pmic_boot_time *time,
		       struct monaco_pmic_boot_result *result, uint64_t *value)
{
	int rc;

	result->settle_time_reads_attempted++;
	rc = time->time_us(time->context, value);
	result->last_callback_status = rc;
	if (rc == 0) {
		result->settle_time_reads_completed++;
	}
	return rc > 0 ? MONACO_PMIC_CALLBACK : rc;
}

int monaco_pmic_boot_finish(struct monaco_pmic *pmic, const struct monaco_pmic_boot_time *time,
			    struct monaco_pmic_boot_result *result)
{
	struct boot boot;
	uint64_t start = 0, end = 0;
	int rc;

	if (!result) {
		return MONACO_PMIC_INVALID;
	}
	/* Single use: a repeat call keeps the first call's result untouched. */
	if (result->stage != MONACO_PMIC_BOOT_TAIL_COMPLETE || result->status) {
		return MONACO_PMIC_STATE;
	}
	if (!pmic || !pmic->io.transfer || !pmic->io.delay_us || !pmic->transfer_timeout_us ||
	    !time || !time->time_us) {
		result->status = MONACO_PMIC_INVALID;
		return result->status;
	}
	rc = tail_ready(pmic, result);
	if (rc != 0) {
		result->status = rc;
		return rc;
	}
	boot = (struct boot){.pmic = *pmic, .transport = pmic->io, .result = result};
	result->stage = MONACO_PMIC_BOOT_SETTLE;
	rc = settle_time(time, result, &start);
	if (rc == 0) {
		rc = delay(&boot, 3000);
	}
	if (rc == 0) {
		rc = settle_time(time, result, &end);
	}
	if (rc == 0) {
		if (end < start) {
			rc = MONACO_PMIC_BOOT_TIMEBASE;
		} else {
			result->settle_elapsed_us = end - start;
			result->settle_elapsed_valid = true;
			if (result->settle_elapsed_us < 3000) {
				rc = MONACO_PMIC_BOOT_TIMEBASE;
			}
		}
	}
	if (rc == 0) {
		result->stage = MONACO_PMIC_BOOT_COMPLETE;
	}
	result->status = rc;
	return rc;
}
