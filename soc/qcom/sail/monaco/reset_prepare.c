/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "reset_prepare.h"

#include <errno.h>
#include <stddef.h>

#define RESET_FSM_ADDRESS   0xf1149024U
#define MD_STATUS_ADDRESS   0xf1149020U
#define RESET_MASK_ADDRESS  0xf1149420U
#define CPU0_CONFIG_ADDRESS 0xf0300044U
#define CPU0_RESET_ENTRY    0x0801d400U
#define RESET_MASK_DEFAULT  0x0000ffffU
#define RESET_MASK_TARGET   0x0000cff8U
#define CPU0_VECTOR_TARGET  (CPU0_RESET_ENTRY >> 5)

struct preparation {
	const struct monaco_timer_io *io;
	struct monaco_reset_prepare_result *result;
	int (*guard)(void *context);
	void *guard_context;
};

static int check_guard(struct preparation *preparation)
{
	struct monaco_reset_prepare_result *result = preparation->result;

	result->guards_attempted++;
	result->guard_status = preparation->guard(preparation->guard_context);
	if (result->guard_status != 0) {
		return result->guard_status < 0 ? result->guard_status : -EIO;
	}
	result->guards_completed++;
	return 0;
}

static int read_control(struct preparation *preparation, uint32_t address, uint32_t *out,
			uint32_t valid)
{
	struct monaco_reset_prepare_result *result = preparation->result;
	uint32_t value;
	int status = check_guard(preparation);
	int after;

	if (status != 0) {
		return status;
	}
	result->address = address;
	result->callback_status =
		preparation->io->read32(preparation->io->context, address, &value);
	if (result->callback_status == 0) {
		*out = value;
		result->valid |= valid;
	}
	after = check_guard(preparation);
	if (result->callback_status != 0) {
		return result->callback_status < 0 ? result->callback_status : -EIO;
	}
	return after;
}

static int write_control(struct preparation *preparation, uint32_t address, uint32_t value,
			 uint8_t store)
{
	struct monaco_reset_prepare_result *result = preparation->result;
	int status = check_guard(preparation);
	int after;

	if (status != 0) {
		return status;
	}
	result->address = address;
	result->stores_attempted |= store;
	result->callback_status =
		preparation->io->write32(preparation->io->context, address, value);
	if (result->callback_status == 0) {
		result->stores_completed |= store;
	}
	after = check_guard(preparation);
	if (result->callback_status != 0) {
		return result->callback_status < 0 ? result->callback_status : -EIO;
	}
	return after;
}

static int prepare_controls(struct preparation *preparation)
{
	struct monaco_reset_prepare_result *result = preparation->result;
	int status;

	result->stage = MONACO_RESET_PREPARE_PREFLIGHT;
	status = read_control(preparation, RESET_FSM_ADDRESS, &result->fsm,
			      MONACO_RESET_PREPARE_FSM_VALID);
	if (status == 0) {
		status = read_control(preparation, MD_STATUS_ADDRESS, &result->md_status,
				      MONACO_RESET_PREPARE_STATUS_VALID);
	}
	if (status == 0) {
		status = read_control(preparation, RESET_MASK_ADDRESS, &result->mask_before,
				      MONACO_RESET_PREPARE_MASK_BEFORE_VALID);
	}
	if (status == 0) {
		status = read_control(preparation, CPU0_CONFIG_ADDRESS, &result->vector_before,
				      MONACO_RESET_PREPARE_VECTOR_BEFORE_VALID);
	}
	if (status != 0) {
		return status;
	}
	if (result->fsm != 0x00020101U || (result->md_status & 0x1000U) == 0U ||
	    (result->mask_before != RESET_MASK_DEFAULT &&
	     result->mask_before != RESET_MASK_TARGET) ||
	    (result->vector_before != 0U && result->vector_before != CPU0_VECTOR_TARGET)) {
		return -ESTALE;
	}

	result->stage = MONACO_RESET_PREPARE_CONFIGURE;
	if (result->mask_before == RESET_MASK_DEFAULT) {
		status = write_control(preparation, RESET_MASK_ADDRESS, RESET_MASK_TARGET,
				       MONACO_RESET_PREPARE_MASK_STORE);
		if (status == 0) {
			status = read_control(preparation, RESET_MASK_ADDRESS,
					      &result->mask_readback,
					      MONACO_RESET_PREPARE_MASK_READBACK_VALID);
		}
		if (status == 0 && result->mask_readback != RESET_MASK_TARGET) {
			status = -EIO;
		}
	}
	if (status == 0 && result->vector_before == 0U) {
		status = write_control(preparation, CPU0_CONFIG_ADDRESS, CPU0_VECTOR_TARGET,
				       MONACO_RESET_PREPARE_VECTOR_STORE);
		if (status == 0) {
			status = read_control(preparation, CPU0_CONFIG_ADDRESS,
					      &result->vector_readback,
					      MONACO_RESET_PREPARE_VECTOR_READBACK_VALID);
		}
		if (status == 0 && result->vector_readback != CPU0_VECTOR_TARGET) {
			status = -EIO;
		}
	}
	if (status != 0) {
		return status;
	}

	result->stage = MONACO_RESET_PREPARE_VERIFY;
	status = read_control(preparation, RESET_MASK_ADDRESS, &result->mask_readback,
			      MONACO_RESET_PREPARE_MASK_READBACK_VALID);
	if (status == 0 && result->mask_readback != RESET_MASK_TARGET) {
		status = -EIO;
	}
	if (status == 0) {
		status = read_control(preparation, CPU0_CONFIG_ADDRESS, &result->vector_readback,
				      MONACO_RESET_PREPARE_VECTOR_READBACK_VALID);
	}
	if (status == 0 && result->vector_readback != CPU0_VECTOR_TARGET) {
		status = -EIO;
	}
	if (status == 0) {
		status = check_guard(preparation);
	}
	return status;
}

int monaco_reset_prepare(const struct monaco_timer_io *io, uint32_t entry,
			 struct monaco_reset_prepare_result *result, int (*guard)(void *context),
			 void *guard_context)
{
	struct preparation preparation = {io, result, guard, guard_context};

	if (result == NULL) {
		return -EINVAL;
	}
	if (result->stage != MONACO_RESET_PREPARE_FRESH) {
		return -EALREADY;
	}
	*result = (struct monaco_reset_prepare_result){.stage = MONACO_RESET_PREPARE_VALIDATE};
	if (io == NULL || io->read32 == NULL || io->write32 == NULL || guard == NULL ||
	    entry != CPU0_RESET_ENTRY || (entry & 31U) != 0U) {
		result->status = -EINVAL;
		return result->status;
	}
	result->status = prepare_controls(&preparation);
	if (result->status == 0) {
		result->stage = MONACO_RESET_PREPARE_COMPLETE;
	}
	return result->status;
}
