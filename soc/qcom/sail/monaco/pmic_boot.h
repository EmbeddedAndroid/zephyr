/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_BOOT_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_PMIC_BOOT_H_

#include "pmic.h"

#define MONACO_PMIC_BOOT_REGULATORS 9U

enum monaco_pmic_topology {
	MONACO_PMIC_TOPOLOGY_UNKNOWN,
	MONACO_PMIC_TOPOLOGY_INDEPENDENT,
	MONACO_PMIC_TOPOLOGY_MERGED,
};

struct monaco_pmic_boot_inputs {
	/* From completed probes of each PMIC address, not from timeouts. */
	enum monaco_pmic_topology topology;
	uint8_t present_slaves;
	bool reset_fsm_valid;
	uint32_t reset_fsm;
	bool platform_type_valid;
	uint32_t platform_type;
	/* Set when the board's supply and self-test policy has been validated. */
	bool supply_policy_validated;
	/* Exactly LDO0..3 followed by SMPS0..4, in that order. */
	const struct monaco_pmic_regulator_policy *regulators;
	size_t regulator_count;
};

enum monaco_pmic_boot_status {
	MONACO_PMIC_BOOT_SKIPPED = 1,
	MONACO_PMIC_BOOT_TOPOLOGY = -1100,
	MONACO_PMIC_BOOT_BIST_FAILED = -1101,
	MONACO_PMIC_BOOT_RUNTIME_BIST_REQUIRED = -1102,
	MONACO_PMIC_BOOT_TIMEBASE = -1103,
};

enum monaco_pmic_bist_policy {
	MONACO_PMIC_REQUIRE_BOOT_BIST,
	MONACO_PMIC_ALLOW_RUNTIME_BIST,
};

struct monaco_pmic_pretarget_inputs {
	enum monaco_pmic_topology topology;
	uint8_t present_slaves;
	bool reset_fsm_valid;
	uint32_t reset_fsm;
	bool platform_type_valid;
	uint32_t platform_type;
	/* Board policy flags; PMIC identification does not set them. */
	bool parameter_status_policy_validated;
	bool runtime_bist_policy_validated;
};

struct monaco_pmic_tail_inputs {
	/* Allows regulator and posttarget writes; does not allow runtime self-test. */
	bool regulator_posttarget_policy_validated;
	const struct monaco_pmic_regulator_policy *regulators;
	size_t regulator_count;
};

struct monaco_pmic_boot_time {
	void *context;
	/* Monotonic microseconds from the same timebase as delay_us; 0 or negative error. */
	int (*time_us)(void *context, uint64_t *value);
};

enum monaco_pmic_boot_stage {
	MONACO_PMIC_BOOT_VALIDATE,
	MONACO_PMIC_BOOT_SUPPORT,
	MONACO_PMIC_BOOT_PLATFORM,
	MONACO_PMIC_BOOT_PARAMETERS,
	MONACO_PMIC_BOOT_FLAGS,
	MONACO_PMIC_BOOT_STATUS,
	MONACO_PMIC_BOOT_BIST_ISOLATE_LDO,
	MONACO_PMIC_BOOT_BIST_GATE,
	MONACO_PMIC_BOOT_BIST_SMPS,
	MONACO_PMIC_BOOT_BIST_LDO,
	MONACO_PMIC_BOOT_BIST_RUN,
	MONACO_PMIC_BOOT_BIST_VERIFY,
	MONACO_PMIC_BOOT_BIST_UNGATE,
	MONACO_PMIC_BOOT_BIST_RESTORE_SMPS,
	MONACO_PMIC_BOOT_BIST_RESTORE_LDO,
	MONACO_PMIC_BOOT_BIST_SETTLE_LDO,
	MONACO_PMIC_BOOT_REGULATOR,
	MONACO_PMIC_BOOT_POSTTARGET,
	MONACO_PMIC_BOOT_SETTLE,
	MONACO_PMIC_BOOT_COMPLETE,
	MONACO_PMIC_BOOT_SKIP_COLD,
	MONACO_PMIC_BOOT_BIST_REQUIRED,
	MONACO_PMIC_BOOT_PRETARGET_COMPLETE,
	MONACO_PMIC_BOOT_TAIL_COMPLETE,
};

enum monaco_pmic_boot_operation {
	MONACO_PMIC_BOOT_NO_OPERATION,
	MONACO_PMIC_BOOT_READ,
	MONACO_PMIC_BOOT_WRITE,
	MONACO_PMIC_BOOT_DELAY,
};

struct monaco_pmic_boot_result {
	enum monaco_pmic_boot_stage stage;
	enum monaco_pmic_boot_operation last_operation;
	unsigned int rail_index;
	uint8_t last_address7;
	uint8_t last_register;
	uint32_t last_delay_us;
	uint32_t operations_attempted;
	uint32_t operations_completed;
	uint32_t delay_us_completed;
	int last_callback_status;
	int status;
	uint8_t status_initial;
	uint8_t status_residual;
	bool status_initial_valid;
	bool status_residual_valid;
	bool boot_bist_flag;
	bool boot_bist_second_flag;
	bool boot_bist_flags_valid;
	bool boot_bist_accepted;
	bool runtime_bist_required;
	bool runtime_bist_attempted;
	bool runtime_bist_flag;
	bool runtime_bist_second_flag;
	bool runtime_bist_flags_valid;
	bool runtime_bist_passed;
	bool runtime_bist_cleanup_complete;
	unsigned int regulators_completed;
	unsigned int posttarget_writes_completed;
	struct monaco_pmic_regulator_result regulators[MONACO_PMIC_BOOT_REGULATORS];
	/* Set by monaco_pmic_boot_tail() for monaco_pmic_boot_finish(); do not write. */
	const struct monaco_pmic *tail_owner;
	const struct monaco_pmic_boot_result *tail_result;
	struct monaco_pmic tail_context;
	uint8_t settle_time_reads_attempted;
	uint8_t settle_time_reads_completed;
	bool settle_elapsed_valid;
	uint64_t settle_elapsed_us;
};

/**
 * Run the full cold-boot sequence for independent PMICs, including the final
 * 3000 us settle. pmic must be IDENTIFIED (not yet in CRC mode) and inputs
 * fully validated; otherwise no I/O is done. Each transfer is tried once; on
 * failure result holds the failing stage and nothing is rolled back.
 *
 * @retval 0 on success.
 * @retval MONACO_PMIC_BOOT_SKIPPED reset FSM >= 12, no I/O done.
 * @retval negative MONACO_PMIC_ or MONACO_PMIC_BOOT_ error.
 */
int monaco_pmic_boot(struct monaco_pmic *pmic, const struct monaco_pmic_boot_inputs *inputs,
		     struct monaco_pmic_boot_result *result);

/**
 * Run the first half of monaco_pmic_boot(): switch to CRC mode, program
 * platform parameters, handle status and the PMIC BIST. Regulators are left
 * to monaco_pmic_boot_tail(). pmic must be IDENTIFIED. With
 * MONACO_PMIC_REQUIRE_BOOT_BIST, a PMIC whose boot BIST result is not
 * accepted stops before any runtime BIST step. Cannot be retried on the same
 * context once the CRC switch has been attempted.
 *
 * @retval 0 on success (stage PRETARGET_COMPLETE).
 * @retval MONACO_PMIC_BOOT_SKIPPED reset FSM >= 12, no I/O done.
 * @retval MONACO_PMIC_BOOT_RUNTIME_BIST_REQUIRED runtime BIST needed but not allowed.
 * @retval negative MONACO_PMIC_ or MONACO_PMIC_BOOT_ error.
 */
int monaco_pmic_pretarget(struct monaco_pmic *pmic,
			  const struct monaco_pmic_pretarget_inputs *inputs,
			  enum monaco_pmic_bist_policy bist_policy,
			  struct monaco_pmic_boot_result *result);

/**
 * Initialize the nine regulators and set the posttarget reset policy and
 * interrupts. Takes the pmic and result of a successful
 * monaco_pmic_pretarget() and updates result in place; all nine policies are
 * checked before any I/O. Does not include the 3000 us settle; call
 * monaco_pmic_boot_finish() before using the supplies.
 *
 * @retval 0 on success (stage TAIL_COMPLETE).
 * @retval MONACO_PMIC_INVALID or MONACO_PMIC_STATE bad input or prior phase.
 * @retval negative transfer error.
 */
int monaco_pmic_boot_tail(struct monaco_pmic *pmic, const struct monaco_pmic_tail_inputs *inputs,
			  struct monaco_pmic_boot_result *result);

/**
 * Wait the 3000 us supply settle after monaco_pmic_boot_tail(). The delay is
 * measured with time and must cover at least 3000 us. pmic must be unchanged
 * since boot_tail. Single use: a failure leaves the stage at SETTLE and
 * cannot be retried. No PMIC transfer is made.
 *
 * @retval 0 on success (stage COMPLETE).
 * @retval MONACO_PMIC_BOOT_TIMEBASE elapsed time short or backwards.
 * @retval negative MONACO_PMIC_ error or callback error.
 */
int monaco_pmic_boot_finish(struct monaco_pmic *pmic, const struct monaco_pmic_boot_time *time,
			    struct monaco_pmic_boot_result *result);

#endif
