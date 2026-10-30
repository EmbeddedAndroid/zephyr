/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#define sail_model_test cold_component_tests
#include "test_cold_boot.c"
#undef sail_model_test
#include "boot_mode.h"
#include "pmic_transaction.h"

#include <errno.h>
#include <zephyr/sys/util.h>

static struct model device;
static struct monaco_cold_boot owned_boot;
static struct monaco_pmic_transaction transaction;
static uint32_t guards, fail_guard, drift_guard, transfers_before;
static uint32_t retire_read, delay_reads, reentries;
static int guard_error;
static bool authorized, active, retired, reenter, corrupt_crc, wrong_readback;
static bool stop_delay, reverse_delay;

static int execute(void);

static void reenter_once(void)
{
	if (active && reenter) {
		const struct monaco_pmic_transaction saved = transaction;
		const uint32_t accesses = device.count;

		sail_check(execute() == -EALREADY);
		sail_check(memcmp(&saved, &transaction, sizeof(saved)) == 0);
		sail_check(device.count == accesses);
		reentries++;
	}
}

static int observed_read(void *context, uint32_t address, uint32_t *value)
{
	int rc = read32(context, address, value);

	if (!active) {
		return rc;
	}
	reenter_once();
	if (transaction.result.phase == MONACO_PMIC_TRANSACTION_DELAYING) {
		delay_reads++;
		if (delay_reads == retire_read) {
			retired = true;
		}
	}
	if (rc == 0 && corrupt_crc && address == SE + 0x780U &&
	    device.transactions == transfers_before + 4U) {
		*value ^= 0x100U;
	}
	return rc;
}

static int observed_write(void *context, uint32_t address, uint32_t value)
{
	if (active && wrong_readback && address == SE + 0x700U &&
	    device.transactions == transfers_before + 3U) {
		device.pmic[0xc9] = 2U;
	}
	int rc = write32(context, address, value);

	reenter_once();
	return rc;
}

/* Test-only admission; the timer check alone is not a policy. */
static bool admitted(void *context, const struct monaco_cold_boot *boot)
{
	sail_check(context == &device);
	reenter_once();
	return authorized && boot == &owned_boot &&
	       sail_boot_timer_qualified(boot, boot->status, SAIL_BOOT_PCINIT_DEFERRED);
}

static int live(void *context)
{
	sail_check(context == &device);
	guards++;
	reenter_once();
	if (guards == drift_guard) {
		owned_boot.pmic.version.revision ^= 1U;
	}
	if (guards == 8U && stop_delay) {
		device.stopped_timer = true;
	}
	if (guards == 8U && reverse_delay) {
		device.ticks = 0U;
	}
	return retired ? -ECANCELED : guards == fail_guard ? guard_error : 0;
}

static void begin(void)
{
	active = false;
	prepare(&device, &owned_boot, true);
	const struct monaco_cold_boot_io io = {
		.context = &device,
		.read32 = observed_read,
		.write32 = observed_write,
		.read_mpidr = read_mpidr,
		.read_cpsr = read_cpsr,
		.md_access_contract = device.access_contract,
		.md_publication = {&device, NULL},
	};
	struct monaco_cold_boot_limits limits = monaco_cold_boot_defaults;

	limits.timer_read_budget = 6000U;
	limits.md_bist.max_poll_reads = 32U;
	limits.clock_poll_reads = 16U;
	sail_check(monaco_cold_boot_run(&owned_boot, &io, &limits) == MONACO_COLD_PCINIT_REQUIRED);
	sail_check(sail_boot_timer_qualified(&owned_boot, owned_boot.status,
					 SAIL_BOOT_PCINIT_DEFERRED));
	memset(&transaction, 0, sizeof(transaction));
	guards = fail_guard = drift_guard = retire_read = delay_reads = reentries = 0U;
	guard_error = -ECANCELED;
	authorized = active = true;
	retired = reenter = corrupt_crc = wrong_readback = stop_delay = reverse_delay = false;
	transfers_before = device.transactions;
}

static int execute(void)
{
	return monaco_pmic_transaction_run(&transaction, &owned_boot, admitted, live, &device);
}

static void no_retry(void)
{
	const struct monaco_pmic_transaction saved = transaction;
	const uint32_t count = device.count;

	sail_check(execute() == -EALREADY);
	sail_check(memcmp(&saved, &transaction, sizeof(saved)) == 0 && device.count == count);
}

int sail_model_test(void)
{
	FILE *file = fopen(SAIL_TEST_FIXTURE_DIR "/early-clock.trace", "r");

	sail_check(file != NULL);
	while (clock_events < ARRAY_SIZE(clock_trace)) {
		struct clock_event *event = &clock_trace[clock_events];

		sail_check(fscanf(file, " %c %x %x", &event->kind, &event->address,
				  &event->value) == 3);
		clock_events++;
	}
	sail_check(fclose(file) == 0);
	begin();
	const uint32_t start = device.count;

	sail_check(execute() == 0 && device.transactions == transfers_before + 5U);
	sail_check(transaction.result.phase == MONACO_PMIC_TRANSACTION_DELAYED);
	sail_check(transaction.result.pmic.stage == MONACO_PMIC_RESET_VERIFIED);
	sail_check(transaction.result.after_us - transaction.result.before_us >= 50000U);
	sail_check(guards == 9U && transaction.result.time_reads == 2U);
	sail_check(device.pmic[0xc9] == 0U && device.pmic[0xcb] == 10U && device.pmic[0xcc] == 1U);
	uint32_t transfer_end = start;

	for (uint32_t i = start; i < device.count; i++) {
		if (device.trace[i].address == SE + 0x780U) {
			transfer_end = i + 1U;
		}
	}
	sail_check(transfer_end > start);
	no_retry();
	/* Replacing the output storage does not release the retained claim. */
	memset(&transaction.result, 0, sizeof(transaction.result));
	no_retry();
	for (uint32_t i = 1U; i <= 9U; i++) {
		for (uint32_t positive = 0U; positive < 2U; positive++) {
			begin();
			fail_guard = i;
			guard_error = positive != 0U ? 1 : -ECANCELED;
			sail_check(execute() == (positive != 0U ? -EIO : -ECANCELED));
			sail_check(transaction.result.phase != MONACO_PMIC_TRANSACTION_DELAYED);
			const uint32_t expected = i < 3U ? 0U : i < 7U ? i - 2U : 5U;

			sail_check(device.transactions == transfers_before + expected);
			no_retry();
		}
		begin();
		drift_guard = i;
		sail_check(execute() == -ESTALE);
		no_retry();
	}
	/* Leaf and I2C callbacks fail before or after every transfer access. */
	for (uint32_t i = start + 1U; i <= transfer_end; i++) {
		for (uint32_t after = 0U; after < 2U; after++) {
			begin();
			device.fail_at = i;
			device.fault_after_effect = after != 0U;
			sail_check(execute() != 0);
			sail_check(transaction.result.phase != MONACO_PMIC_TRANSACTION_DELAYED);
			no_retry();
		}
	}
	begin();
	reenter = true;
	sail_check(execute() == 0 && reentries > 20U);
	begin();
	authorized = false;
	sail_check(execute() == -EPERM && device.transactions == transfers_before);
	no_retry();
	begin();
	struct monaco_cold_boot copied = owned_boot;

	sail_check(monaco_pmic_transaction_run(&transaction, &copied, admitted, live, &device) ==
	       -EPERM);
	sail_check(device.transactions == transfers_before);
	begin();
	owned_boot.i2c.busy = true;
	sail_check(execute() == -EPERM);
	begin();
	owned_boot.i2c.timer = NULL;
	sail_check(execute() == -EPERM);
	begin();
	owned_boot.pmic_boot.tail_owner = NULL;
	sail_check(execute() == -EPERM);
	begin();
	*reg(&device, 0xf1149024U) = 12U;
	sail_check(execute() == -ESTALE && device.transactions == transfers_before);
	begin();
	corrupt_crc = true;
	sail_check(execute() == MONACO_PMIC_CRC);
	sail_check(transaction.result.time_reads == 0U);
	begin();
	wrong_readback = true;
	sail_check(execute() == MONACO_PMIC_RESET_VERIFY);
	sail_check(transaction.result.time_reads == 0U);
	begin();
	stop_delay = true;
	sail_check(execute() == MONACO_TIMER_BUDGET);
	sail_check(transaction.result.pmic.stage == MONACO_PMIC_RESET_VERIFIED);
	begin();
	reverse_delay = true;
	sail_check(execute() == MONACO_TIMER_BACKWARD);
	begin();
	retire_read = 20U;
	sail_check(execute() == -ECANCELED && delay_reads >= retire_read);
	sail_check(transaction.result.after_us - transaction.result.before_us >= 50000U);
	sail_check(transaction.result.phase != MONACO_PMIC_TRANSACTION_DELAYED);
	no_retry();
	begin();
	sail_check(monaco_pmic_transaction_run(&transaction, &owned_boot, NULL, live, &device) ==
	       -EINVAL);
	sail_check(!transaction.claimed);
	sail_check(monaco_pmic_transaction_run(&transaction, &owned_boot, admitted, NULL,
					       &device) == -EINVAL);
	sail_check(!transaction.claimed);
	printf("PMIC transaction: actual cold/CRC/I2C/timer chain, nine guards, lifetime/reentry, "
	       "readback/timer/loss failures and %u transfer access faults pass\n",
	       2U * (transfer_end - start));
	return 0;
}
