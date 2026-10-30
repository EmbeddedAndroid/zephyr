/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic.h"

static const uint8_t addresses[] = {0x28, 0x29, 0x12};

static int callback_result(int result)
{
	return result > 0 ? MONACO_PMIC_CALLBACK : result;
}

static bool configured(const struct monaco_pmic *pmic)
{
	return pmic && pmic->io.transfer && pmic->io.delay_us && pmic->transfer_timeout_us != 0;
}

static int transfer(struct monaco_pmic *pmic, unsigned int slave, const uint8_t *tx, size_t tx_size,
		    uint8_t *rx, size_t rx_size)
{
	return callback_result(pmic->io.transfer(pmic->io.context, addresses[slave], tx, tx_size,
						 rx, rx_size, pmic->transfer_timeout_us));
}

static int delay(struct monaco_pmic *pmic, uint32_t us)
{
	return callback_result(pmic->io.delay_us(pmic->io.context, us));
}

/* CRC-8, polynomial 0x07, initial value 0xff, no reflection or final XOR. */
static uint8_t crc8(const uint8_t *bytes, size_t size)
{
	uint8_t crc = 0xff;

	for (size_t i = 0; i < size; ++i) {
		crc ^= bytes[i];
		for (unsigned int bit = 0; bit < 8; ++bit) {
			crc = (uint8_t)((uint32_t)crc << 1 ^ ((crc & 0x80U) ? 0x07U : 0U));
		}
	}
	return crc;
}

int monaco_pmic_init(struct monaco_pmic *pmic, const struct monaco_pmic_io *io,
		     uint32_t transfer_timeout_us)
{
	if (!pmic) {
		return MONACO_PMIC_INVALID;
	}
	if (!io || !io->transfer || !io->delay_us || !transfer_timeout_us) {
		*pmic = (struct monaco_pmic){0};
		return MONACO_PMIC_INVALID;
	}
	*pmic = (struct monaco_pmic){
		.io = *io,
		.transfer_timeout_us = transfer_timeout_us,
		.state = MONACO_PMIC_RAW,
	};
	return 0;
}

int monaco_pmic_identify(struct monaco_pmic *pmic)
{
	static const uint8_t regs[] = {0x01, 0x02, 0x03, 0xa7};
	uint8_t bytes[sizeof(regs)] = {0};
	int rc;

	if (!configured(pmic)) {
		return MONACO_PMIC_INVALID;
	}
	if (pmic->state == MONACO_PMIC_IDENTIFIED || pmic->state == MONACO_PMIC_CRC_READY) {
		return 0;
	}
	if (pmic->state != MONACO_PMIC_RAW) {
		return MONACO_PMIC_STATE;
	}
	pmic->version = (struct monaco_pmic_version){0};
	for (size_t i = 0; i < sizeof(regs); ++i) {
		rc = transfer(pmic, 0, &regs[i], 1, &bytes[i], 1);
		if (rc != 0) {
			return rc;
		}
	}
	if (bytes[0] != 0x82) {
		return MONACO_PMIC_UNSUPPORTED;
	}
	pmic->version = (struct monaco_pmic_version){
		.device = bytes[0],
		.revision = bytes[1],
		.firmware = bytes[2],
		.firmware_revision = bytes[3],
	};
	pmic->state = MONACO_PMIC_IDENTIFIED;
	return 0;
}

int monaco_pmic_enable_crc(struct monaco_pmic *pmic)
{
	static const uint8_t command[] = {0x85, 0x02};
	int rc;

	if (!configured(pmic)) {
		return MONACO_PMIC_INVALID;
	}
	if (pmic->state == MONACO_PMIC_CRC_READY) {
		return 0;
	}
	if (pmic->state != MONACO_PMIC_IDENTIFIED) {
		return MONACO_PMIC_STATE;
	}
	/* A failed transfer may still have changed the device's bus format. */
	pmic->state = MONACO_PMIC_TRANSITION;
	rc = transfer(pmic, 0, command, sizeof(command), NULL, 0);
	if (rc != 0) {
		return rc;
	}
	rc = delay(pmic, 2000);
	if (rc != 0) {
		return rc;
	}
	pmic->state = MONACO_PMIC_CRC_READY;
	return 0;
}

static int ready(const struct monaco_pmic *pmic, unsigned int slave)
{
	if (!configured(pmic) || slave >= sizeof(addresses)) {
		return MONACO_PMIC_INVALID;
	}
	return pmic->state == MONACO_PMIC_CRC_READY ? 0 : MONACO_PMIC_STATE;
}

int monaco_pmic_read(struct monaco_pmic *pmic, unsigned int slave, uint8_t reg, uint8_t *value)
{
	uint8_t rx[2] = {0};
	uint8_t covered[4];
	int rc = ready(pmic, slave);

	if (!value) {
		return MONACO_PMIC_INVALID;
	}
	if (rc != 0) {
		return rc;
	}
	rc = transfer(pmic, slave, &reg, 1, rx, sizeof(rx));
	if (rc != 0) {
		return rc;
	}
	covered[0] = (uint8_t)(addresses[slave] << 1);
	covered[1] = reg;
	covered[2] = (uint8_t)(covered[0] | 1U);
	covered[3] = rx[0];
	if (crc8(covered, sizeof(covered)) != rx[1]) {
		return MONACO_PMIC_CRC;
	}
	*value = rx[0];
	return 0;
}

int monaco_pmic_write(struct monaco_pmic *pmic, unsigned int slave, uint8_t reg, uint8_t value)
{
	uint8_t covered[3];
	uint8_t tx[3] = {reg, value, 0};
	int rc = ready(pmic, slave);

	if (rc != 0) {
		return rc;
	}
	covered[0] = (uint8_t)(addresses[slave] << 1);
	covered[1] = reg;
	covered[2] = value;
	tx[2] = crc8(covered, sizeof(covered));
	return transfer(pmic, slave, tx, sizeof(tx), NULL, 0);
}

int monaco_pmic_update_bits(struct monaco_pmic *pmic, unsigned int slave, uint8_t reg, uint8_t mask,
			    uint8_t value)
{
	uint8_t old;
	int rc = monaco_pmic_read(pmic, slave, reg, &old);

	if (rc != 0) {
		return rc;
	}
	return monaco_pmic_write(pmic, slave, reg,
				 (uint8_t)((old & (uint8_t)~mask) | (value & mask)));
}

static int regulator_voltage(struct monaco_pmic *pmic,
			     const struct monaco_pmic_regulator_policy *policy, uint32_t *uv)
{
	uint8_t code, control;
	int rc;

	if (policy->group == MONACO_PMIC_LDO) {
		rc = monaco_pmic_read(pmic, 0, (uint8_t)(0x23U + policy->index), &code);
		if (rc != 0) {
			return rc;
		}
		if (policy->index == 3) {
			code = (uint8_t)((code & 0x7fU) - 32U);
			*uv = 1200000U + 25000U * code;
		} else {
			code = (uint8_t)(((code & 0x7eU) >> 1) - 4U);
			*uv = 600000U + 50000U * code;
		}
		return 0;
	}
	control = (uint8_t)(4U + 2U * policy->index);
	rc = monaco_pmic_read(pmic, 0, control, &code);
	if (rc != 0) {
		return rc;
	}
	control = (uint8_t)(control + ((code & 8U) ? 0x0bU : 0x0aU));
	rc = monaco_pmic_read(pmic, 0, control, &code);
	if (rc != 0) {
		return rc;
	}
	if (code < 15) {
		*uv = 300000U + 20000U * code;
	} else if (code < 115) {
		*uv = 600000U + 5000U * (code - 15U);
	} else if (code < 171) {
		*uv = 1100000U + 10000U * (code - 115U);
	} else {
		*uv = 1660000U + 20000U * (code - 171U);
	}
	return 0;
}

int monaco_pmic_regulator_init(struct monaco_pmic *pmic,
			       const struct monaco_pmic_regulator_policy *policy,
			       struct monaco_pmic_regulator_result *result)
{
	uint8_t control, value, enable_mask;
	int rc = ready(pmic, 0);

	if (!result) {
		return MONACO_PMIC_INVALID;
	}
	*result = (struct monaco_pmic_regulator_result){0};
	if (!policy || policy->minimum_uv > policy->maximum_uv ||
	    (policy->group != MONACO_PMIC_LDO && policy->group != MONACO_PMIC_SMPS) ||
	    policy->index >= (policy->group == MONACO_PMIC_LDO ? 4U : 5U)) {
		return MONACO_PMIC_INVALID;
	}
	if (policy->required_mode != (policy->group == MONACO_PMIC_LDO ? 2U : 0U)) {
		return MONACO_PMIC_UNSUPPORTED;
	}
	if (rc != 0) {
		return rc;
	}
	result->stage = MONACO_PMIC_REGULATOR_VOLTAGE;
	rc = regulator_voltage(pmic, policy, &result->voltage_uv);
	if (rc != 0) {
		return rc;
	}
	result->voltage_valid = true;
	if (result->voltage_uv < policy->minimum_uv || result->voltage_uv > policy->maximum_uv) {
		return MONACO_PMIC_RANGE;
	}
	result->stage = MONACO_PMIC_REGULATOR_MODE;
	if (policy->group == MONACO_PMIC_LDO) {
		control = (uint8_t)(0x1dU + policy->index);
		enable_mask = 0x10;
		result->observed_mode = 2;
	} else {
		control = (uint8_t)(4U + 2U * policy->index);
		enable_mask = 0x30;
		rc = monaco_pmic_read(pmic, 0, control, &value);
		if (rc != 0) {
			return rc;
		}
		result->observed_mode = (uint8_t)((value >> 1) & 1U);
	}
	result->mode_valid = true;
	result->stage = MONACO_PMIC_REGULATOR_ENABLE;
	rc = monaco_pmic_read(pmic, 0, control, &value);
	if (rc != 0) {
		return rc;
	}
	result->observed_enabled = (value & 1U) != 0;
	result->enable_valid = true;
	if (policy->enable_required && !result->observed_enabled) {
		if (!policy->write_permitted) {
			result->enable_skipped = true;
		} else {
			result->stage = MONACO_PMIC_REGULATOR_ENABLE_BIT;
			rc = monaco_pmic_update_bits(pmic, 0, control, 0x01, 0x01);
			if (rc != 0) {
				return rc;
			}
			result->stage = MONACO_PMIC_REGULATOR_ENABLE_CONTROL;
			rc = monaco_pmic_update_bits(pmic, 0, control, enable_mask, enable_mask);
			if (rc != 0) {
				return rc;
			}
			result->stage = MONACO_PMIC_REGULATOR_SETTLE;
			rc = delay(pmic, 1000);
			if (rc != 0) {
				return rc;
			}
			result->enable_sequence_complete = true;
		}
	}
	result->stage = MONACO_PMIC_REGULATOR_COMPLETE;
	return 0;
}
