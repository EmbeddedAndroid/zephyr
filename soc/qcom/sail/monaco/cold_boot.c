/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cold_boot.h"

const struct monaco_cold_boot_limits monaco_cold_boot_defaults = {
	.timer_read_budget = 16000000,
	.clock_poll_reads = 4096,
	.transfer_timeout_us = 2000,
	.md_bist = {.max_poll_reads = 1000000},
	.policy = {
			.presence_timeout_us = 100000,
			.ready_timeout_us = 10000000,
			.max_presence_checks = 101,
			.max_ready_waits = 10,
		},
};

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct monaco_cold_boot *boot = context;

	return boot->io.read32(boot->io.context, address, value);
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct monaco_cold_boot *boot = context;

	return boot->io.write32(boot->io.context, address, value);
}

static int read_mpidr(void *context, uint32_t *value)
{
	struct monaco_cold_boot *boot = context;

	return boot->io.read_mpidr(boot->io.context, value);
}

static int time_us(void *context, uint64_t *value)
{
	struct monaco_cold_boot *boot = context;

	return monaco_timer_time_us(&boot->timer, value);
}

static int delay_us(void *context, uint32_t delay)
{
	struct monaco_cold_boot *boot = context;

	return monaco_timer_delay_us(&boot->timer, delay);
}

static int md_publish(void *context, enum md_bist_event event)
{
	struct monaco_cold_boot *boot = context;

	return md_publication_event(&boot->md_publication, event);
}

static int md_invalidate(void *context)
{
	struct monaco_cold_boot *boot = context;

	return md_publication_invalidate_stateless_clocks(&boot->md_publication);
}

static int active_md_bist(struct monaco_cold_boot *boot)
{
	const struct md_bist_io io = {
		.context = boot,
		.read32 = read32,
		.write32 = write32,
		.time_us = time_us,
		.delay_us = delay_us,
		.invalidate_md_clocks = md_invalidate,
		.publish = md_publish,
	};
	struct md_bist_prerequisites prerequisites = {
		.access_contract = boot->io.md_access_contract,
		.clocks_initialized = boot->early_clocks.stage == MONACO_EARLY_CLOCK_COMPLETE &&
				      boot->early_clocks.completed_branches == 62,
		.timer_validated = boot->timer.initialized && boot->timer.frequency == 19200000 &&
				   boot->clock.timer == &boot->timer,
	};
	int result = md_bist_assess_policy(&boot->policy.bist);

	/*
	 * Entry state, identity, timer, clocks and policy are already checked.
	 * The access contract is the caller's assertion and is not verified.
	 */
	boot->md_bist_diagnostic.stage = MD_STAGE_POLICY;
	if (!result && boot->io.md_access_contract != MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE) {
		result = MD_BIST_PREREQUISITE;
	}
	if (!result && !boot->io.md_publication.write32) {
		result = MONACO_COLD_MD_PUBLICATION_REQUIRED;
	}
	if (!result) {
		result = md_publication_init(&boot->md_publication, boot->io.md_publication.context,
					     boot->io.md_publication.write32, &boot->clock);
	}
	if (!result) {
		prerequisites.md_clock_model_adapter_validated =
			boot->md_publication.started && !boot->md_publication.error;
		result = md_bist_run(&boot->policy.bist, &prerequisites, &io, &boot->limits.md_bist,
				     &boot->md_bist_diagnostic);
	} else {
		boot->md_bist_diagnostic.result = result;
	}
	boot->md_bist_result = result;
	boot->md_bist_completed = result == 0;
	return result;
}

static int probe(void *context, uint8_t address, uint32_t timeout)
{
	struct monaco_cold_boot *boot = context;

	return monaco_i2c_probe(&boot->i2c, address, timeout);
}

static int transfer(void *context, uint8_t address, const uint8_t *tx, size_t tx_size, uint8_t *rx,
		    size_t rx_size, uint32_t timeout)
{
	struct monaco_cold_boot *boot = context;

	return monaco_i2c_transfer(&boot->i2c, address, tx, tx_size, rx, rx_size, timeout);
}

static int entry_state(struct monaco_cold_boot *boot)
{
	boot->entry_callback_status = boot->io.read_cpsr(boot->io.context, &boot->cpsr);
	if (boot->entry_callback_status) {
		return MONACO_COLD_CALLBACK;
	}
	/* SYS/EL1, A32, little-endian, I/F masked, no IT/J state. */
	if ((boot->cpsr & UINT32_C(0x0700feff)) != UINT32_C(0xdf)) {
		return MONACO_COLD_ENTRY;
	}
	boot->entry_callback_status = read_mpidr(boot, &boot->mpidr);
	if (boot->entry_callback_status) {
		return MONACO_COLD_CALLBACK;
	}
	return (boot->mpidr & UINT32_C(0x00ffffff)) ? MONACO_COLD_ENTRY : 0;
}

/*
 * Runtime PMIC self-test is allowed only for the VENTUNO Q profile, with the
 * board, PMIC sample and identified PMIC from this boot all in agreement.
 */
static bool runtime_bist_profile(const struct monaco_cold_boot *boot)
{
	const struct monaco_pmic_policy_selection *s = &boot->pmic_selection;
	const struct monaco_board_identity *id = &boot->board.current_identity;

	return boot->board.descriptor != NULL &&
	       boot->board.descriptor->board == MONACO_BOARD_VENTUNO_Q &&
	       boot->board.descriptor->entry == MONACO_BOARD_PBL_NORMAL_COLD &&
	       boot->board.stage == MONACO_BOARD_COMPLETE && boot->board.status == 0 &&
	       boot->pmic_snapshot.profile == MONACO_PMIC_BOARD_VENTUNO_Q &&
	       boot->pmic_snapshot.sampled && boot->pmic_snapshot.status == 0 &&
	       boot->pmic_snapshot.stage == MONACO_PMIC_POLICY_SAMPLED && s->sku == id->sku &&
	       s->chip_variant == id->chip_variant && s->chip_version == id->chip_version &&
	       s->boot.topology == MONACO_PMIC_TOPOLOGY_INDEPENDENT &&
	       s->boot.present_slaves == 7U && s->boot.reset_fsm_valid &&
	       s->boot.reset_fsm == id->reset_fsm && s->boot.reset_fsm < 12U &&
	       s->boot.platform_type_valid && s->boot.platform_type == 0U &&
	       s->boot.regulators != NULL &&
	       s->boot.regulator_count == MONACO_PMIC_BOOT_REGULATORS &&
	       boot->pmic.state == MONACO_PMIC_IDENTIFIED && boot->pmic.version.device == 0x82U &&
	       s->pmic_version.device == boot->pmic.version.device &&
	       s->pmic_version.revision == boot->pmic.version.revision &&
	       s->pmic_version.firmware == boot->pmic.version.firmware &&
	       s->pmic_version.firmware_revision == boot->pmic.version.firmware_revision &&
	       boot->pmic.io.context == boot && boot->pmic.io.transfer != NULL &&
	       boot->pmic.io.delay_us != NULL && boot->pmic.transfer_timeout_us > 0U &&
	       boot->timer.initialized && boot->timer.frequency == 19200000U &&
	       boot->timer.read_budget > 0U && boot->timer.io.context == boot &&
	       boot->clock.timer == &boot->timer;
}

static int execute(struct monaco_cold_boot *boot)
{
	int rc;
	const struct monaco_timer_io mmio = {boot, read32, write32};
	const struct monaco_policy_io policy_io = {
		boot, read32, read_mpidr, time_us, delay_us,
	};
	const struct monaco_pmic_policy_io pmic_policy_io = {boot, read32, probe};
	const struct monaco_pmic_io pmic_io = {boot, transfer, delay_us};
	const struct monaco_pmic_boot_time settle_time = {boot, time_us};
	struct monaco_pmic_boot_inputs *selected = &boot->pmic_selection.boot;

	boot->stage = MONACO_COLD_ENTRY_STATE;
	rc = entry_state(boot);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_BOARD;
	rc = monaco_board_prepare(&mmio, MONACO_BOARD_VENTUNO_Q, &boot->board);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_TIMER;
	rc = monaco_timer_init(&boot->timer, &mmio, boot->limits.timer_read_budget);
	if (rc != 0) {
		return rc;
	}
	if (boot->timer.frequency != 19200000) {
		return MONACO_TIMER_INVALID;
	}
	boot->stage = MONACO_COLD_POLICY_PREPARE;
	rc = monaco_policy_prepare(&policy_io, &boot->limits.policy, &boot->policy,
				   &boot->policy_diagnostic);
	if (rc != 0) {
		return rc;
	}
	boot->clock = (struct monaco_clock){mmio, &boot->timer, boot->limits.clock_poll_reads};
	boot->stage = MONACO_COLD_EARLY_CLOCKS;
	rc = monaco_early_clocks_enable(&boot->clock, &boot->early_clocks);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_POLICY_FINISH;
	boot->policy_result =
		monaco_policy_finish(&policy_io, &boot->policy, &boot->policy_diagnostic);
	if (boot->policy_result == MONACO_POLICY_READY) {
		boot->stage = MONACO_COLD_MD_BIST;
		rc = active_md_bist(boot);
		if (rc != 0) {
			return rc;
		}
	} else if (boot->policy_result == MONACO_POLICY_SKIP) {
		/* Recorded as skipped, not as passed. */
		boot->md_bist_skipped = true;
	} else {
		return boot->policy_result;
	}
	boot->stage = MONACO_COLD_PARENTS;
	rc = monaco_board_after_clocks(&mmio, &boot->board, &boot->i2c_qualification,
				       &boot->hbcu_policy);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_HBCU;
	rc = monaco_hbcu_capture(&mmio, &boot->hbcu_policy, &boot->hbcu);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_I2C;
	rc = monaco_i2c_boot_init(&boot->i2c, &boot->clock, &boot->i2c_qualification,
				  &boot->i2c_setup);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_PMIC_SAMPLE;
	rc = monaco_pmic_policy_sample(MONACO_PMIC_BOARD_VENTUNO_Q, &pmic_policy_io,
				       boot->limits.transfer_timeout_us, &boot->pmic_snapshot);
	if (rc != 0) {
		return rc;
	}
	/* Compare only defined fields; reserved SKU and FSM bits may differ. */
	if (((boot->pmic_snapshot.raw_sku ^ boot->board.current_identity.raw_sku) &
	     UINT32_C(0xffff)) ||
	    boot->pmic_snapshot.raw_soc_identity != boot->board.current_identity.raw_soc_identity ||
	    ((boot->pmic_snapshot.raw_reset_fsm ^ boot->board.current_identity.raw_reset_fsm) &
	     UINT32_C(0xf))) {
		return MONACO_COLD_POLICY_CHANGED;
	}
	boot->stage = MONACO_COLD_PMIC_IDENTIFY;
	rc = monaco_pmic_init(&boot->pmic, &pmic_io, boot->limits.transfer_timeout_us);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_PMIC_IDENTIFY;
	rc = monaco_pmic_identify(&boot->pmic);
	if (rc != 0) {
		return rc;
	}
	/* supply_policy_validated stays false; pretarget and tail take narrower flags. */
	boot->stage = MONACO_COLD_PMIC_SELECT;
	rc = monaco_pmic_policy_select(&boot->pmic_snapshot, &boot->pmic, false,
				       &boot->pmic_selection);
	if (rc != 0) {
		return rc;
	}
	const bool allow_runtime_bist = runtime_bist_profile(boot);
	const struct monaco_pmic_pretarget_inputs pretarget = {
		.topology = selected->topology,
		.present_slaves = selected->present_slaves,
		.reset_fsm_valid = selected->reset_fsm_valid,
		.reset_fsm = selected->reset_fsm,
		.platform_type_valid = selected->platform_type_valid,
		.platform_type = selected->platform_type,
		.parameter_status_policy_validated = true,
		.runtime_bist_policy_validated = allow_runtime_bist,
	};
	const struct monaco_pmic_tail_inputs tail = {
		.regulator_posttarget_policy_validated = true,
		.regulators = selected->regulators,
		.regulator_count = selected->regulator_count,
	};
	/* Regulator setup in the tail does not depend on the runtime BIST decision. */
	boot->stage = MONACO_COLD_PMIC_PRETARGET;
	rc = monaco_pmic_pretarget(&boot->pmic, &pretarget,
				   allow_runtime_bist ? MONACO_PMIC_ALLOW_RUNTIME_BIST
						      : MONACO_PMIC_REQUIRE_BOOT_BIST,
				   &boot->pmic_boot);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_PMIC_TAIL;
	rc = monaco_pmic_boot_tail(&boot->pmic, &tail, &boot->pmic_boot);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_PMIC_SETTLE;
	rc = monaco_pmic_boot_finish(&boot->pmic, &settle_time, &boot->pmic_boot);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_POST_PMIC_CLOCKS;
	rc = monaco_post_pmic_clocks(&boot->clock, &boot->pmic, &boot->pmic_boot,
				     selected->regulators, selected->regulator_count,
				     &boot->post_clocks);
	if (rc != 0) {
		return rc;
	}
	boot->stage = MONACO_COLD_PCINIT;
	return MONACO_COLD_PCINIT_REQUIRED;
}

int monaco_cold_boot_run(struct monaco_cold_boot *boot, const struct monaco_cold_boot_io *io,
			 const struct monaco_cold_boot_limits *limits)
{
	if (!boot) {
		return MONACO_COLD_INVALID;
	}
	if (boot->started) {
		return MONACO_COLD_STATE;
	}
	boot->started = true;
	boot->status = MONACO_COLD_INVALID;
	if (!io || !io->read32 || !io->write32 || !io->read_mpidr || !io->read_cpsr || !limits ||
	    !limits->timer_read_budget || !limits->clock_poll_reads ||
	    !limits->md_bist.max_poll_reads || limits->transfer_timeout_us < 4 ||
	    !limits->policy.presence_timeout_us || !limits->policy.ready_timeout_us ||
	    !limits->policy.max_presence_checks || limits->policy.max_presence_checks > 101 ||
	    !limits->policy.max_ready_waits) {
		return boot->status;
	}
	boot->io = *io;
	boot->limits = *limits;
	boot->status = execute(boot);
	return boot->status;
}
