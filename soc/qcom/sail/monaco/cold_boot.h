/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_COLD_BOOT_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_COLD_BOOT_H_

#include "board_policy.h"
#include "early_clock.h"
#include "hbcu.h"
#include "i2c_boot.h"
#include "monaco_policy.h"
#include "md_publication.h"
#include "pmic_policy.h"
#include "post_pmic_clock.h"

enum monaco_cold_boot_error {
	MONACO_COLD_INVALID = -2500,
	MONACO_COLD_STATE,
	MONACO_COLD_ENTRY,
	MONACO_COLD_CALLBACK,
	MONACO_COLD_MD_PUBLICATION_REQUIRED,
	MONACO_COLD_PCINIT_REQUIRED,
	MONACO_COLD_POLICY_CHANGED,
};

enum monaco_cold_boot_stage {
	MONACO_COLD_IDLE,
	MONACO_COLD_ENTRY_STATE,
	MONACO_COLD_BOARD,
	MONACO_COLD_TIMER,
	MONACO_COLD_POLICY_PREPARE,
	MONACO_COLD_EARLY_CLOCKS,
	MONACO_COLD_POLICY_FINISH,
	MONACO_COLD_MD_BIST,
	MONACO_COLD_PARENTS,
	MONACO_COLD_HBCU,
	MONACO_COLD_I2C,
	MONACO_COLD_PMIC_SAMPLE,
	MONACO_COLD_PMIC_IDENTIFY,
	MONACO_COLD_PMIC_SELECT,
	MONACO_COLD_PMIC_PRETARGET,
	MONACO_COLD_PMIC_TAIL,
	MONACO_COLD_PMIC_SETTLE,
	MONACO_COLD_POST_PMIC_CLOCKS,
	MONACO_COLD_PCINIT,
};

struct monaco_cold_boot_io {
	void *context;
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	int (*write32)(void *context, uint32_t address, uint32_t value);
	int (*read_mpidr)(void *context, uint32_t *value);
	int (*read_cpsr)(void *context, uint32_t *value);
	enum md_bist_access_contract md_access_contract;
	/* Dedicated writer for MD status words; write32 is never used in its place. */
	struct md_publication_backend md_publication;
};

struct monaco_cold_boot_limits {
	uint32_t timer_read_budget;
	uint32_t clock_poll_reads;
	uint32_t transfer_timeout_us;
	struct md_bist_limits md_bist;
	struct monaco_policy_limits policy;
};

extern const struct monaco_cold_boot_limits monaco_cold_boot_defaults;

struct monaco_cold_boot {
	bool started;
	enum monaco_cold_boot_stage stage;
	int status;
	uint32_t cpsr, mpidr;
	int entry_callback_status;
	int policy_result;
	bool md_bist_skipped;
	bool md_bist_completed;
	int md_bist_result;
	struct md_bist_diagnostic md_bist_diagnostic;
	struct md_publication md_publication;
	struct monaco_cold_boot_io io;
	struct monaco_cold_boot_limits limits;
	struct monaco_board_policy board;
	struct monaco_timer timer;
	struct monaco_clock clock;
	struct monaco_policy_snapshot policy;
	struct monaco_policy_diagnostic policy_diagnostic;
	struct monaco_early_clock_status early_clocks;
	struct monaco_hbcu_policy hbcu_policy;
	struct monaco_hbcu_result hbcu;
	struct monaco_i2c_boot_qualification i2c_qualification;
	struct monaco_i2c_boot_status i2c_setup;
	struct monaco_i2c i2c;
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot pmic_snapshot;
	struct monaco_pmic_policy_selection pmic_selection;
	struct monaco_pmic_boot_result pmic_boot;
	struct monaco_post_pmic_clock_status post_clocks;
};

/**
 * Run the cold-boot sequence once, from core 0 in SYS mode (A32, little
 * endian, IRQ and FIQ masked) with other cores parked and exclusive access
 * to the peripherals. boot must be zeroed and must not move afterwards; the
 * callback contexts must outlive it. Single use: a second call returns
 * MONACO_COLD_STATE and leaves the first result intact. On failure boot->stage
 * and the component results show how far it got; nothing is rolled back.
 * A BIST or publication failure stops before HBCU and PMIC setup.
 *
 * @retval MONACO_COLD_PCINIT_REQUIRED all implemented steps completed; PCInit
 *         is not implemented, so this is not a completed boot.
 * @retval MONACO_COLD_MD_PUBLICATION_REQUIRED BIST needed but no publication writer.
 * @retval negative error from the failing stage otherwise.
 */
int monaco_cold_boot_run(struct monaco_cold_boot *boot, const struct monaco_cold_boot_io *io,
			 const struct monaco_cold_boot_limits *limits);

#endif
