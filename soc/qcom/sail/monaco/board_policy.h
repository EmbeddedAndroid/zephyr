/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_BOARD_POLICY_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_BOARD_POLICY_H_

#include "hbcu.h"
#include "i2c_boot.h"

enum monaco_board {
	MONACO_BOARD_UNKNOWN,
	MONACO_BOARD_VENTUNO_Q,
};

enum monaco_board_entry_contract {
	MONACO_BOARD_PBL_NORMAL_COLD = 1,
};

enum monaco_board_assumption {
	MONACO_BOARD_INHERITED_XO = 1U << 0,
	MONACO_BOARD_EXCLUSIVE_EARLY_OWNER = 1U << 1,
	MONACO_BOARD_NO_CONCURRENT_RESET = 1U << 2,
};

/* Fixed per-board settings; nothing here is read from hardware. */
struct monaco_board_descriptor {
	enum monaco_board board;
	enum monaco_board_entry_contract entry;
	const char *name;
	uint32_t inherited_xo_hz;
	uint32_t assumptions;
	uint8_t hbcu_injected_failures;
};

enum monaco_board_error {
	MONACO_BOARD_INVALID = -2600,
	MONACO_BOARD_UNSUPPORTED,
	MONACO_BOARD_IDENTITY,
	MONACO_BOARD_NOT_COLD,
	MONACO_BOARD_STALE,
	MONACO_BOARD_PARENT,
	MONACO_BOARD_SE_STATE,
	MONACO_BOARD_CALLBACK,
};

enum monaco_board_stage {
	MONACO_BOARD_INPUT,
	MONACO_BOARD_SKU,
	MONACO_BOARD_SOC,
	MONACO_BOARD_FSM,
	MONACO_BOARD_CHECK_IDENTITY,
	MONACO_BOARD_PREPARED,
	MONACO_BOARD_AFTER_CLOCKS,
	MONACO_BOARD_CORE_ROOT,
	MONACO_BOARD_AHB_ROOT,
	MONACO_BOARD_QUP_BRANCHES,
	MONACO_BOARD_SE_CONTROL,
	MONACO_BOARD_COMPLETE,
};

struct monaco_board_identity {
	uint32_t raw_sku, raw_soc_identity, raw_reset_fsm;
	uint16_t sku, chip_variant;
	uint32_t chip_version;
	uint8_t reset_fsm;
};

struct monaco_board_clocks {
	uint32_t core_command, core_config, ahb_command, ahb_config;
	uint32_t core2x_branch, core_branch, master_ahb_branch, slave_ahb_branch;
	uint32_t se_dfs, se_command, se_branch;
};

struct monaco_board_policy {
	const struct monaco_board_descriptor *descriptor;
	/* Samples from prepare and from after_clocks, which must match. */
	struct monaco_board_identity identity, current_identity;
	struct monaco_board_clocks clocks;
	enum monaco_board_stage stage;
	uint32_t last_address, last_value;
	unsigned int reads_attempted, reads_completed;
	int callback_status, status;
	bool value_valid;
};

/**
 * Select the board policy after reading the SKU, SoC identity and reset FSM,
 * in that order. Only the seven supported SKUs and a cold reset FSM (low
 * nibble < 12) are accepted. Every call, including a failed one, clears any
 * earlier policy. io->write32 is not used and may be NULL.
 *
 * @retval 0 on success, or a negative MONACO_BOARD_ or read callback error.
 */
int monaco_board_prepare(const struct monaco_timer_io *io, enum monaco_board board,
			 struct monaco_board_policy *policy);

/**
 * After the early clocks are up, check that identity and reset FSM still
 * match monaco_board_prepare(), that the QUP core and AHB roots and four
 * parent branches are set up, and that SE0 has DFS off, no pending update
 * and its branch off. Reads only; the GENI registers are not touched since
 * SE0 is not yet clocked. Assumes a cold PBL entry on core 0 with interrupts
 * masked, other cores parked and XO at 19.2 MHz. Consumes the prepared
 * policy even on failure. On success fills the I2C boot assertions and the
 * HBCU policy; on error both are cleared.
 *
 * @retval 0 on success.
 * @retval MONACO_BOARD_STALE not prepared, or identity changed.
 * @retval negative MONACO_BOARD_ or read callback error.
 */
int monaco_board_after_clocks(const struct monaco_timer_io *io, struct monaco_board_policy *policy,
			      struct monaco_i2c_boot_qualification *i2c,
			      struct monaco_hbcu_policy *hbcu);

#endif
