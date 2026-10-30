/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_HBCU_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_HBCU_H_

#include "timer.h"

enum monaco_hbcu_error {
	MONACO_HBCU_INVALID = -2300,
	MONACO_HBCU_POLICY,
	MONACO_HBCU_IO,
	MONACO_HBCU_MBIST_FAILED,
	MONACO_HBCU_LBIST_FAILED,
};

enum monaco_hbcu_phase {
	MONACO_HBCU_INPUT,
	MONACO_HBCU_DISABLE,
	MONACO_HBCU_MBIST,
	MONACO_HBCU_LBIST,
	MONACO_HBCU_COMPLETE,
};

enum monaco_hbcu_outcome {
	MONACO_HBCU_UNOBSERVED,
	MONACO_HBCU_SKIPPED,
	MONACO_HBCU_PASSED,
	MONACO_HBCU_FAILED,
};

enum monaco_hbcu_operation {
	MONACO_HBCU_NO_OPERATION,
	MONACO_HBCU_READ,
	MONACO_HBCU_WRITE,
};

struct monaco_hbcu_policy {
	/* Set only when the board policy has been validated. */
	bool valid;
	/* Bit0 injects MBIST failure, bit1 injects LBIST failure. */
	uint8_t injected_failures;
};

struct monaco_hbcu_test_result {
	uint32_t words[3];
	/*
	 * Bit n is set once word n is read or published. A skipped test publishes
	 * zero as word 2 without reading it.
	 */
	uint8_t captured, published;
	enum monaco_hbcu_outcome outcome;
};

struct monaco_hbcu_result {
	enum monaco_hbcu_phase phase;
	enum monaco_hbcu_operation last_operation;
	uint32_t address, value;
	uint32_t completed_reads, completed_writes;
	int callback_status, status;
	bool disabled;
	struct monaco_hbcu_test_result mbist, lbist;
};

bool monaco_hbcu_write_allowed(uint32_t address);
/*
 * Disable HBCU bit 0, then capture and publish the MBIST and LBIST status.
 * The caller must have exclusive access to the registers. Makes at most 14
 * register accesses and never waits. A zero return does not mean both tests
 * passed: check each outcome, since a skipped test has not passed. On failure,
 * result holds the phase and partial progress; a failed write may still have
 * reached the hardware.
 *
 * @retval 0 capture and publication completed.
 * @retval MONACO_HBCU_INVALID bad arguments or injection bits.
 * @retval MONACO_HBCU_POLICY board policy not valid.
 * @retval MONACO_HBCU_IO register access failed.
 * @retval MONACO_HBCU_MBIST_FAILED or MONACO_HBCU_LBIST_FAILED test failed.
 */
int monaco_hbcu_capture(const struct monaco_timer_io *io, const struct monaco_hbcu_policy *policy,
			struct monaco_hbcu_result *result);

#endif
