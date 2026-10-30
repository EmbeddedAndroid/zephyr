/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "boot_mode.h"
#include "monaco/cold_boot.h"

static bool timer_ready(const struct monaco_cold_boot *boot)
{
	return boot && boot->timer.initialized && boot->timer.frequency == 19200000 &&
	       boot->timer.read_budget && boot->timer.io.context == boot && boot->timer.io.read32 &&
	       boot->timer.io.write32 && boot->clock.timer == &boot->timer;
}

static bool md_qualified(const struct monaco_cold_boot *boot)
{
	if (boot->md_bist_skipped) {
		return boot->policy_result == MONACO_POLICY_SKIP && !boot->md_bist_completed &&
		       !boot->md_publication.started && boot->policy.prepared &&
		       boot->policy.consumed && boot->policy.warm_cookie_valid &&
		       boot->policy.warm_cookie != 0xf &&
		       boot->policy_diagnostic.result == MONACO_POLICY_SKIP &&
		       (boot->policy.bist.skip || boot->policy.bist.mode == 0 ||
			boot->policy.bist.mode == 3);
	}
	return boot->policy_result == MONACO_POLICY_READY && boot->policy.prepared &&
	       boot->policy.consumed && boot->policy_diagnostic.stage == MONACO_STAGE_COMPLETE &&
	       !boot->policy_diagnostic.result &&
	       boot->io.md_access_contract == MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE &&
	       md_bist_assess_policy(&boot->policy.bist) == MD_BIST_OK && boot->md_bist_completed &&
	       !boot->md_bist_result && boot->md_bist_diagnostic.stage == MD_STAGE_COMPLETE &&
	       !boot->md_bist_diagnostic.result && boot->md_publication.clock == &boot->clock &&
	       boot->md_publication.context == boot->io.md_publication.context &&
	       boot->md_publication.write32 == boot->io.md_publication.write32 &&
	       md_publication_is_complete(&boot->md_publication);
}

/* The PMIC self-test must pass at boot or, failing that, in the runtime retry.
 * PMIC firmware 1.0 does not require the second flag.
 */
static bool pmic_bist_complete(const struct monaco_cold_boot *boot)
{
	const struct monaco_pmic_boot_result *p = &boot->pmic_boot;
	const bool exception =
		boot->pmic.version.firmware == 1U && boot->pmic.version.firmware_revision == 0U;

	if (!p->boot_bist_flags_valid) {
		return false;
	}
	if (p->boot_bist_accepted) {
		return p->boot_bist_flag && (p->boot_bist_second_flag || exception) &&
		       !p->runtime_bist_required && !p->runtime_bist_attempted;
	}
	return !(p->boot_bist_flag && (p->boot_bist_second_flag || exception)) &&
	       p->runtime_bist_required && p->runtime_bist_attempted &&
	       p->runtime_bist_flags_valid && p->runtime_bist_flag &&
	       (p->runtime_bist_second_flag || exception) && p->runtime_bist_passed &&
	       p->runtime_bist_cleanup_complete;
}

static bool pcinit_deferred_eligible(const struct monaco_cold_boot *boot, int result)
{
	if (!boot || !boot->started || result != MONACO_COLD_PCINIT_REQUIRED ||
	    boot->status != result || boot->stage != MONACO_COLD_PCINIT || !md_qualified(boot) ||
	    !timer_ready(boot)) {
		return false;
	}
	const struct monaco_pmic_boot_result *pmic = &boot->pmic_boot;

	return boot->pmic.state == MONACO_PMIC_CRC_READY && boot->pmic.version.device == 0x82 &&
	       boot->pmic.io.context == boot && boot->pmic.io.transfer && boot->pmic.io.delay_us &&
	       pmic->tail_owner == &boot->pmic && pmic->tail_result == pmic &&
	       pmic->tail_context.state == boot->pmic.state &&
	       pmic->tail_context.io.context == boot->pmic.io.context &&
	       pmic->tail_context.io.transfer == boot->pmic.io.transfer &&
	       pmic->tail_context.io.delay_us == boot->pmic.io.delay_us &&
	       pmic->tail_context.transfer_timeout_us == boot->pmic.transfer_timeout_us &&
	       pmic->tail_context.version.device == boot->pmic.version.device &&
	       pmic->tail_context.version.revision == boot->pmic.version.revision &&
	       pmic->tail_context.version.firmware == boot->pmic.version.firmware &&
	       pmic->tail_context.version.firmware_revision ==
		       boot->pmic.version.firmware_revision &&
	       pmic->stage == MONACO_PMIC_BOOT_COMPLETE && !pmic->status &&
	       pmic_bist_complete(boot) && !pmic->last_callback_status &&
	       pmic->operations_attempted == pmic->operations_completed &&
	       pmic->regulators_completed == MONACO_PMIC_BOOT_REGULATORS &&
	       pmic->posttarget_writes_completed == 6 &&
	       pmic->last_operation == MONACO_PMIC_BOOT_DELAY && pmic->last_delay_us == 3000 &&
	       pmic->settle_time_reads_attempted == 2 && pmic->settle_time_reads_completed == 2 &&
	       pmic->settle_elapsed_valid && pmic->settle_elapsed_us >= 3000 &&
	       boot->post_clocks.stage == MONACO_POST_PMIC_CLOCK_COMPLETE &&
	       boot->post_clocks.completed_domains == 4 && !boot->post_clocks.address;
}

enum sail_boot_mode sail_boot_mode_select(const struct monaco_cold_boot *boot, int result,
					  bool allow_pcinit_deferred)
{
	if (!boot || !boot->started || boot->status != result) {
		return SAIL_BOOT_STOPPED;
	}
	if (!result) {
		return SAIL_BOOT_NORMAL;
	}
	return allow_pcinit_deferred && pcinit_deferred_eligible(boot, result)
		       ? SAIL_BOOT_PCINIT_DEFERRED
		       : SAIL_BOOT_STOPPED;
}

bool sail_boot_timer_qualified(const struct monaco_cold_boot *boot, int result,
			       enum sail_boot_mode mode)
{
	if (mode == SAIL_BOOT_PCINIT_DEFERRED) {
		return pcinit_deferred_eligible(boot, result);
	}
	return mode == SAIL_BOOT_NORMAL && boot && boot->started && !result && !boot->status &&
	       timer_ready(boot);
}
