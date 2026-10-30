/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum monaco_pmic_error {
	MONACO_PMIC_INVALID = -1000,
	MONACO_PMIC_STATE = -1001,
	MONACO_PMIC_UNSUPPORTED = -1002,
	MONACO_PMIC_CRC = -1003,
	MONACO_PMIC_RANGE = -1004,
	MONACO_PMIC_CALLBACK = -1005,
};

struct monaco_pmic_io {
	void *context;
	/*
	 * transfer writes tx to a seven-bit address and, if rx is set, reads rx
	 * after a repeated START. It must return within timeout_us; a partial
	 * transfer is an error. delay_us waits at least the requested time.
	 * Both return 0 or a negative error, and calls must be serialized.
	 */
	int (*transfer)(void *context, uint8_t address7, const uint8_t *tx, size_t tx_size,
			uint8_t *rx, size_t rx_size, uint32_t timeout_us);
	int (*delay_us)(void *context, uint32_t delay);
};

enum monaco_pmic_state {
	MONACO_PMIC_UNINITIALIZED,
	MONACO_PMIC_RAW,
	MONACO_PMIC_IDENTIFIED,
	/* CRC switch failed; the PMICs must be reset to raw mode before reuse. */
	MONACO_PMIC_TRANSITION,
	MONACO_PMIC_CRC_READY,
};

struct monaco_pmic_version {
	uint8_t device;
	uint8_t revision;
	uint8_t firmware;
	uint8_t firmware_revision;
};

struct monaco_pmic {
	struct monaco_pmic_io io;
	uint32_t transfer_timeout_us;
	enum monaco_pmic_state state;
	struct monaco_pmic_version version;
};

/*
 * Initialize the PMIC context; no bus access. Before identify, the I2C
 * controller must be ready and all three PMICs in raw (non-CRC) mode.
 * Every operation tries each transfer once and stops at the first failure;
 * writes are never retried.
 */
int monaco_pmic_init(struct monaco_pmic *pmic, const struct monaco_pmic_io *io,
		     uint32_t transfer_timeout_us);
int monaco_pmic_identify(struct monaco_pmic *pmic);
int monaco_pmic_enable_crc(struct monaco_pmic *pmic);

/* Logical slave 0/1/2 maps to seven-bit address 0x28/0x29/0x12. */
int monaco_pmic_read(struct monaco_pmic *pmic, unsigned int slave, uint8_t reg, uint8_t *value);
int monaco_pmic_write(struct monaco_pmic *pmic, unsigned int slave, uint8_t reg, uint8_t value);
int monaco_pmic_update_bits(struct monaco_pmic *pmic, unsigned int slave, uint8_t reg, uint8_t mask,
			    uint8_t value);

enum monaco_pmic_regulator_group {
	MONACO_PMIC_LDO,
	MONACO_PMIC_SMPS,
};

/* Board policy for one rail, supplied by the caller. */
struct monaco_pmic_regulator_policy {
	enum monaco_pmic_regulator_group group;
	unsigned int index;
	bool write_permitted;
	bool enable_required;
	/* Normal boot supports LDO mode 2 and SMPS mode 0 only. */
	uint8_t required_mode;
	uint32_t minimum_uv;
	uint32_t maximum_uv;
};

enum monaco_pmic_regulator_stage {
	MONACO_PMIC_REGULATOR_INVALID,
	MONACO_PMIC_REGULATOR_VOLTAGE,
	MONACO_PMIC_REGULATOR_MODE,
	MONACO_PMIC_REGULATOR_ENABLE,
	MONACO_PMIC_REGULATOR_ENABLE_BIT,
	MONACO_PMIC_REGULATOR_ENABLE_CONTROL,
	MONACO_PMIC_REGULATOR_SETTLE,
	MONACO_PMIC_REGULATOR_COMPLETE,
};

struct monaco_pmic_regulator_result {
	enum monaco_pmic_regulator_stage stage;
	uint32_t voltage_uv;
	uint8_t observed_mode;
	bool observed_enabled;
	bool voltage_valid;
	bool mode_valid;
	bool enable_valid;
	bool enable_skipped;
	bool enable_sequence_complete;
};

/**
 * Check a rail's voltage (inclusive bounds) and enable state, and enable it
 * if required. Voltage and mode are never written. The enable is
 * written only when write_permitted; otherwise enable_skipped is set. On
 * failure result->stage names the failing step; a partial enable sequence
 * is not rolled back.
 *
 * @retval 0 on success, or a negative MONACO_PMIC_ error or callback error.
 */
int monaco_pmic_regulator_init(struct monaco_pmic *pmic,
			       const struct monaco_pmic_regulator_policy *policy,
			       struct monaco_pmic_regulator_result *result);

#endif
