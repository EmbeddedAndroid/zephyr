/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_RESET_PREPARE_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_RESET_PREPARE_H_

#include "timer.h"

enum monaco_reset_prepare_stage {
	MONACO_RESET_PREPARE_FRESH,
	MONACO_RESET_PREPARE_VALIDATE,
	MONACO_RESET_PREPARE_PREFLIGHT,
	MONACO_RESET_PREPARE_CONFIGURE,
	MONACO_RESET_PREPARE_VERIFY,
	MONACO_RESET_PREPARE_COMPLETE,
};

enum monaco_reset_prepare_valid {
	MONACO_RESET_PREPARE_FSM_VALID = 1U,
	MONACO_RESET_PREPARE_STATUS_VALID = 2U,
	MONACO_RESET_PREPARE_MASK_BEFORE_VALID = 4U,
	MONACO_RESET_PREPARE_VECTOR_BEFORE_VALID = 8U,
	MONACO_RESET_PREPARE_MASK_READBACK_VALID = 16U,
	MONACO_RESET_PREPARE_VECTOR_READBACK_VALID = 32U,
};

enum monaco_reset_prepare_store {
	MONACO_RESET_PREPARE_MASK_STORE = 1U,
	MONACO_RESET_PREPARE_VECTOR_STORE = 2U,
};

struct monaco_reset_prepare_result {
	enum monaco_reset_prepare_stage stage;
	int status;
	int callback_status;
	int guard_status;
	uint32_t valid;
	uint32_t fsm;
	uint32_t md_status;
	uint32_t mask_before;
	uint32_t vector_before;
	uint32_t mask_readback;
	uint32_t vector_readback;
	uint32_t address;
	uint8_t stores_attempted;
	uint8_t stores_completed;
	uint8_t guards_attempted;
	uint8_t guards_completed;
};

/**
 * @brief Set the main-domain reset mask and the CPU0 reset vector for a reset.
 *
 * Requires reset FSM 0x00020101 and MD status bit 12 set. The mask must read
 * 0x0000ffff or 0x0000cff8 and the vector 0 or 0x00400ea0; a default value is
 * written once and read back, then both are verified again. @p guard runs
 * before and after every register access and must fail once the reset service
 * is no longer live. @p result must be zeroed and is single use.
 *
 * @param io Ordered read and write callbacks; both are required.
 * @param entry Address of sail_reset, which must be 0x0801d400.
 * @param result Caller-owned attempt state and diagnostics.
 * @param guard Required liveness check.
 * @param guard_context Opaque argument passed to guard; may be NULL.
 * @retval 0 Both controls hold their target values.
 * @retval -EINVAL A required argument, callback or entry address is invalid.
 * @retval -EALREADY This result has already been used.
 * @retval -ESTALE FSM, MD status or a control value is unsupported.
 * @retval -EIO A readback differs or a callback returned a positive error.
 * @return A negative I/O or guard error is propagated unchanged.
 */
int monaco_reset_prepare(const struct monaco_timer_io *io, uint32_t entry,
			 struct monaco_reset_prepare_result *result, int (*guard)(void *context),
			 void *guard_context);

#endif
