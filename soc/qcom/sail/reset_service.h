/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_RESET_SERVICE_H_
#define ZEPHYR_SOC_QCOM_SAIL_RESET_SERVICE_H_

#include "md_receive.h"
#include "monaco/reset_prepare.h"
#include "monaco/pmic_transaction.h"

enum sail_reset_service_phase {
	SAIL_RESET_SERVICE_NEW,
	SAIL_RESET_SERVICE_LISTENING,
	SAIL_RESET_SERVICE_RETIRED,
	SAIL_RESET_SERVICE_PREPARING,
	SAIL_RESET_SERVICE_PMIC,
	SAIL_RESET_SERVICE_REPLY,
	SAIL_RESET_SERVICE_ACKED,
	SAIL_RESET_SERVICE_COMPLETE,
	SAIL_RESET_SERVICE_FAILED,
};

#define SAIL_RESET_DEBUG_ENABLE      0xf1149000U
#define SAIL_MD_PS_HOLD_MASK         0xf1149420U
#define SAIL_MD_PS_HOLD_MASK_DEFAULT 0x0000ffffU

/** Reset debug control on a board without a SAIL PMIC. */
struct sail_reset_debug_result {
	uint32_t before;
	uint32_t after;
	uint32_t mask_after;
	uint8_t reads;
	uint8_t writes;
	bool valid;
};

struct sail_reset_service_status {
	enum sail_reset_service_phase phase;
	int error;
	int cleanup_error;
	int retirement_error;
	uint64_t consumed;
	struct sail_md_receive_event event;
	uint32_t word;
	bool claimed;
	bool retirement_attempted;
	uint8_t reply_attempted;
	uint8_t reply_completed;
	uint32_t sdi_answers;
	struct sail_md_receive_stats receive;
	struct monaco_reset_prepare_result preparation;
	struct sail_reset_debug_result reset_debug;
	struct monaco_pmic_transaction_result transaction;
};

#endif
