/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
/* Reuse the cold boot MMIO/PMIC fixture. This test runs only the full chain
 * and the deferred-mode cases below.
 */
#define sail_model_test cold_boot_model_tests
#include "test_cold_boot.c"
#undef sail_model_test
#include "boot_mode.h"
#include <zephyr/sys/util.h>

static void load_clocks(void)
{
	FILE *file = fopen(SAIL_TEST_FIXTURE_DIR "/early-clock.trace", "r");

	sail_check(file);
	while (clock_events < ARRAY_SIZE(clock_trace)) {
		struct clock_event *event = &clock_trace[clock_events];

		sail_check(fscanf(file, " %c %x %x", &event->kind, &event->address,
				  &event->value) == 3);
		clock_events++;
	}
	sail_check(fclose(file) == 0);
}

static void runtime_bist_deferred(void)
{
	static struct model model;
	struct monaco_cold_boot boot;

	prepare(&model, &boot, true);
	model.pmic[0x66] = 0U;
	model.pmic[0x81] = 0U;
	model.runtime_success = true;
	const int result = run(&model, &boot);
	const struct monaco_cold_boot saved = boot;
	const uint32_t accesses = model.count;

	sail_check(result == MONACO_COLD_PCINIT_REQUIRED && model.runtime_triggers == 1U);
	sail_check(sail_boot_mode_select(&boot, result, false) == SAIL_BOOT_STOPPED);
	sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_PCINIT_DEFERRED);
	sail_check(sail_boot_timer_qualified(&boot, result, SAIL_BOOT_PCINIT_DEFERRED));
	sail_check(!sail_boot_timer_qualified(&boot, result, SAIL_BOOT_NORMAL));
	for (uint32_t mutation = 0U; mutation < 11U; mutation++) {
		boot = saved;
		switch (mutation) {
		case 0:
			boot.pmic_boot.boot_bist_flags_valid = false;
			break;
		case 1:
			boot.pmic_boot.runtime_bist_required = false;
			break;
		case 2:
			boot.pmic_boot.runtime_bist_attempted = false;
			break;
		case 3:
			boot.pmic_boot.runtime_bist_flags_valid = false;
			break;
		case 4:
			boot.pmic_boot.runtime_bist_flag = false;
			break;
		case 5:
			boot.pmic_boot.runtime_bist_second_flag = false;
			break;
		case 6:
			boot.pmic_boot.runtime_bist_passed = false;
			break;
		case 7:
			boot.pmic_boot.runtime_bist_cleanup_complete = false;
			break;
		case 8:
			boot.pmic_boot.boot_bist_accepted = true;
			break;
		case 9:
			boot.pmic_boot.settle_elapsed_valid = false;
			break;
		case 10:
			boot.pmic_boot.tail_owner = NULL;
			break;
		}
		const struct monaco_cold_boot changed = boot;

		sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_STOPPED);
		sail_check(!sail_boot_timer_qualified(&boot, result, SAIL_BOOT_PCINIT_DEFERRED));
		sail_check(memcmp(&boot, &changed, sizeof(boot)) == 0 && model.count == accesses);
	}
	/* The firmware version exception applies to freshly read flags. */
	prepare(&model, &boot, true);
	model.pmic[3] = 1U;
	model.pmic[4] = 0U;
	model.pmic[0x66] = 0U;
	model.pmic[0x81] = 0U;
	model.runtime_success = true;
	model.runtime_second_flag = false;
	sail_check(run(&model, &boot) == result);
	sail_check(boot.pmic.version.firmware == 1U && boot.pmic.version.firmware_revision == 0U);
	sail_check(!boot.pmic_boot.runtime_bist_second_flag && boot.pmic_boot.runtime_bist_passed);
	sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_PCINIT_DEFERRED);
	puts("boot mode: fresh runtime BIST completion and 11 incomplete-state refusals "
	     "pass");
}

int sail_model_test(void)
{
	struct model model;
	struct monaco_cold_boot boot;

	load_clocks();
	prepare(&model, &boot, true);
	const int result = run(&model, &boot);

	sail_check(result == MONACO_COLD_PCINIT_REQUIRED);
	const struct monaco_cold_boot saved = boot;
	const unsigned int accesses = model.count;

	sail_check(sail_boot_mode_select(&boot, result, false) == SAIL_BOOT_STOPPED);
	sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_PCINIT_DEFERRED);
	sail_check(sail_boot_timer_qualified(&boot, result, SAIL_BOOT_PCINIT_DEFERRED));
	sail_check(!sail_boot_timer_qualified(&boot, result, SAIL_BOOT_NORMAL));
	sail_check(!sail_boot_timer_qualified(&boot, result, SAIL_BOOT_STOPPED));
	sail_check(!sail_boot_timer_qualified(&boot, result, (enum sail_boot_mode)7));
	sail_check(memcmp(&boot, &saved, sizeof(boot)) == 0 && model.count == accesses);
	sail_check((uint32_t)SAIL_BOOT_PCINIT_DEFERRED == UINT32_C(0x4f425356));
	sail_check((uint32_t)result == UINT32_C(0xfffff641));

	for (unsigned int mutation = 0; mutation < 46; mutation++) {
		boot = saved;
		switch (mutation) {
		case 0:
			boot.started = false;
			break;
		case 1:
			boot.stage = MONACO_COLD_POST_PMIC_CLOCKS;
			break;
		case 2:
			boot.status = 0;
			break;
		case 3:
			boot.md_bist_skipped = false;
			break;
		case 4:
			boot.policy_result = MONACO_POLICY_READY;
			break;
		case 5:
			boot.timer.initialized = false;
			break;
		case 6:
			boot.timer.frequency = 1000000;
			break;
		case 7:
			boot.timer.read_budget = 0;
			break;
		case 8:
			boot.timer.io.context = NULL;
			break;
		case 9:
			boot.timer.io.read32 = NULL;
			break;
		case 10:
			boot.timer.io.write32 = NULL;
			break;
		case 11:
			boot.clock.timer = NULL;
			break;
		case 12:
			boot.pmic.state = MONACO_PMIC_RAW;
			break;
		case 13:
			boot.pmic.version.device = 0x81;
			break;
		case 14:
			boot.pmic.io.context = NULL;
			break;
		case 15:
			boot.pmic.io.transfer = NULL;
			break;
		case 16:
			boot.pmic.io.delay_us = NULL;
			break;
		case 17:
			boot.pmic_boot.tail_owner = NULL;
			break;
		case 18:
			boot.pmic_boot.tail_result = NULL;
			break;
		case 19:
			boot.pmic_boot.tail_context.state = MONACO_PMIC_RAW;
			break;
		case 20:
			boot.pmic_boot.tail_context.io.context = NULL;
			break;
		case 21:
			boot.pmic_boot.tail_context.io.transfer = NULL;
			break;
		case 22:
			boot.pmic_boot.tail_context.io.delay_us = NULL;
			break;
		case 23:
			boot.pmic_boot.tail_context.transfer_timeout_us ^= 1;
			break;
		case 24:
			boot.pmic_boot.tail_context.version.device ^= 1;
			break;
		case 25:
			boot.pmic_boot.tail_context.version.revision ^= 1;
			break;
		case 26:
			boot.pmic_boot.tail_context.version.firmware ^= 1;
			break;
		case 27:
			boot.pmic_boot.tail_context.version.firmware_revision ^= 1;
			break;
		case 28:
			boot.pmic_boot.stage = MONACO_PMIC_BOOT_TAIL_COMPLETE;
			break;
		case 29:
			boot.pmic_boot.status = -1;
			break;
		case 30:
			boot.pmic_boot.boot_bist_accepted = false;
			break;
		case 31:
			boot.pmic_boot.runtime_bist_required = true;
			break;
		case 32:
			boot.pmic_boot.runtime_bist_attempted = true;
			break;
		case 33:
			boot.pmic_boot.last_callback_status = -1;
			break;
		case 34:
			boot.pmic_boot.operations_attempted++;
			break;
		case 35:
			boot.pmic_boot.regulators_completed--;
			break;
		case 36:
			boot.pmic_boot.posttarget_writes_completed--;
			break;
		case 37:
			boot.pmic_boot.last_operation = MONACO_PMIC_BOOT_READ;
			break;
		case 38:
			boot.pmic_boot.last_delay_us = 2999;
			break;
		case 39:
			boot.pmic_boot.settle_time_reads_attempted = 1;
			break;
		case 40:
			boot.pmic_boot.settle_time_reads_completed = 1;
			break;
		case 41:
			boot.pmic_boot.settle_elapsed_valid = false;
			break;
		case 42:
			boot.pmic_boot.settle_elapsed_us = 2999;
			break;
		case 43:
			boot.post_clocks.stage = MONACO_POST_PMIC_CLOCK_DMA_DOMAIN;
			break;
		case 44:
			boot.post_clocks.completed_domains = 3;
			break;
		case 45:
			boot.post_clocks.address = 1;
			break;
		default:
			sail_check(false);
		}
		const struct monaco_cold_boot changed = boot;

		sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_STOPPED);
		sail_check(!sail_boot_timer_qualified(&boot, result, SAIL_BOOT_PCINIT_DEFERRED));
		sail_check(memcmp(&boot, &changed, sizeof(boot)) == 0);
	}
	boot = saved;
	struct monaco_cold_boot copied = boot;

	sail_check(sail_boot_mode_select(&copied, result, true) == SAIL_BOOT_STOPPED);
	const int other_results[] = {0, 1, -1, MONACO_COLD_STATE,
				     MONACO_COLD_MD_PUBLICATION_REQUIRED};
	for (unsigned int i = 0; i < ARRAY_SIZE(other_results); i++) {
		sail_check(sail_boot_mode_select(&boot, other_results[i], true) ==
			   SAIL_BOOT_STOPPED);
		sail_check(!sail_boot_timer_qualified(&boot, other_results[i],
						  SAIL_BOOT_PCINIT_DEFERRED));
	}
	sail_check(model.count == accesses && memcmp(&boot, &saved, sizeof(boot)) == 0);
	sail_check(sail_boot_mode_select(NULL, result, true) == SAIL_BOOT_STOPPED);
	sail_check(!sail_boot_timer_qualified(NULL, result, SAIL_BOOT_PCINIT_DEFERRED));
	sail_check(!sail_boot_timer_qualified(NULL, 0, SAIL_BOOT_NORMAL));

	/* A completed active handshake enables the deferred mode without reporting SKIP. */
	prepare(&model, &boot, false);
	model.with_publication = true;
	sail_check(run(&model, &boot) == result);
	sail_check(!boot.md_bist_skipped && boot.md_bist_completed);
	sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_PCINIT_DEFERRED);
	sail_check(sail_boot_timer_qualified(&boot, result, SAIL_BOOT_PCINIT_DEFERRED));
	const struct monaco_cold_boot active = boot;

	for (unsigned int mutation = 0; mutation < 27; mutation++) {
		boot = active;
		switch (mutation) {
		case 0:
			boot.md_bist_completed = false;
			break;
		case 1:
			boot.md_bist_skipped = true;
			break;
		case 2:
			boot.md_bist_result = -1;
			break;
		case 3:
			boot.md_bist_diagnostic.stage = MD_STAGE_PHASE2_RESULT;
			break;
		case 4:
			boot.md_bist_diagnostic.result = -1;
			break;
		case 5:
			boot.io.md_access_contract = MD_BIST_ACCESS_UNSPECIFIED;
			break;
		case 6:
			boot.policy.bist.snapshot_valid = false;
			break;
		case 7:
			boot.md_publication.started = false;
			break;
		case 8:
			boot.md_publication.publishing = true;
			break;
		case 9:
			boot.md_publication.events_completed--;
			break;
		case 10:
			boot.md_publication.invalidations_completed--;
			break;
		case 11:
			boot.md_publication.writes_attempted--;
			break;
		case 12:
			boot.md_publication.writes_completed--;
			break;
		case 13:
			boot.md_publication.error = -1;
			break;
		case 14:
			boot.md_publication.callback_status = -1;
			break;
		case 15:
			boot.md_publication.clock = NULL;
			break;
		case 16:
			boot.md_publication.context = NULL;
			break;
		case 17:
			boot.md_publication.write32 = NULL;
			break;
		case 18:
			boot.md_publication.word--;
			break;
		case 19:
			boot.md_publication.event = MD_EVENT_PHASE2_READY;
			break;
		case 20:
			boot.md_publication.byte_index--;
			break;
		case 21:
			boot.md_publication.address -= 4;
			break;
		case 22:
			boot.md_publication.value--;
			break;
		case 23:
			boot.policy.prepared = false;
			break;
		case 24:
			boot.policy.consumed = false;
			break;
		case 25:
			boot.policy_diagnostic.stage = MONACO_STAGE_COMMAND;
			break;
		case 26:
			boot.policy_diagnostic.result = MONACO_POLICY_SKIP;
			break;
		default:
			sail_check(false);
		}
		sail_check(sail_boot_mode_select(&boot, result, true) == SAIL_BOOT_STOPPED);
		sail_check(!sail_boot_timer_qualified(&boot, result, SAIL_BOOT_PCINIT_DEFERRED));
	}

	/* Active MD and pretarget BIST failures never enter the deferred mode. */
	prepare(&model, &boot, false);
	int failed = run(&model, &boot);

	sail_check(failed == MONACO_COLD_MD_PUBLICATION_REQUIRED);
	sail_check(sail_boot_mode_select(&boot, failed, true) == SAIL_BOOT_STOPPED);
	sail_check(!sail_boot_timer_qualified(&boot, failed, SAIL_BOOT_PCINIT_DEFERRED));
	prepare(&model, &boot, true);
	model.pmic[0x66] = model.pmic[0x81] = 0;
	failed = run(&model, &boot);
	sail_check(failed == MONACO_PMIC_BOOT_BIST_FAILED);
	sail_check(sail_boot_mode_select(&boot, failed, true) == SAIL_BOOT_STOPPED);
	sail_check(!sail_boot_timer_qualified(&boot, failed, SAIL_BOOT_PCINIT_DEFERRED));
	runtime_bist_deferred();
	puts("boot mode: real skip/active chains, 46 common and 27 active incomplete/stale "
	     "guards, default refusal and retained status pass");
	return 0;
}
