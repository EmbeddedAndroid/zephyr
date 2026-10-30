/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_reset.h"

#include "sail_check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define TIMEOUT_US    5000U
#define IO_ERROR      (-77)

struct event {
	uint8_t reg, value, check;
	size_t tx_size, rx_size;
};

struct fixture {
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot snapshot;
	struct monaco_pmic_boot_result boot;
	struct monaco_pmic_reset_result result;
	uint8_t registers[256];
	uint64_t elapsed_us;
	unsigned int raw_reads, sample_reads, probes, transfers, delays, time_reads;
	unsigned int reset_transfers, fail_at, corrupt_at;
	int error;
	bool crc_enabled, active, fail_after_effect, override_readback, reenter;
	uint8_t readback[2], corrupt_mask;
	struct event trace[5];
	uint32_t sku, soc, fsm;
};

/* Polynomial long division, independent of the driver's byte recurrence. */
static uint8_t crc(const uint8_t *data, size_t size)
{
	uint64_t word = 0;
	unsigned int bits = (unsigned int)size * 8U;

	sail_check(size >= 1 && size <= 4);
	for (size_t i = 0; i < size; ++i) {
		word = word << 8 | data[i];
	}
	word = word << 8 ^ UINT64_C(0xff) << bits;
	for (unsigned int bit = bits + 8U; bit-- > 8U;) {
		if (word & UINT64_C(1) << bit) {
			word ^= UINT64_C(0x107) << (bit - 8U);
		}
	}
	return (uint8_t)word;
}

static int configure(struct fixture *f)
{
	return monaco_pmic_reset_configure_readback(&f->pmic, &f->snapshot, &f->boot, &f->result);
}

static int transfer(void *context, uint8_t address, const uint8_t *tx, size_t tx_size, uint8_t *rx,
		    size_t rx_size, uint32_t timeout)
{
	static const uint8_t identity[] = {1, 2, 3, 0xa7};
	static const uint8_t reset_registers[] = {0xc9, 0xcb, 0xcc, 0xc9, 0xcc};
	static const uint8_t reset_values[] = {0, 0x0a, 1};
	static const uint8_t reset_checks[] = {0x5f, 0x43, 0x19};
	struct fixture *f = context;
	uint8_t covered[4] = {0x50, tx[0], 0x51, 0};
	unsigned int n = 0;
	bool fail_now = false;

	sail_check(address == 0x28 && timeout == TIMEOUT_US && tx);
	f->transfers++;
	if (!f->crc_enabled) {
		sail_check(!f->active);
		if (rx_size) {
			sail_check(tx_size == 1 && rx_size == 1 && rx);
			sail_check(f->raw_reads < ARRAY_SIZE(identity));
			sail_check(tx[0] == identity[f->raw_reads++]);
			rx[0] = f->registers[tx[0]];
		} else {
			sail_check(tx_size == 2 && !rx && tx[0] == 0x85 && tx[1] == 2);
			sail_check(f->raw_reads == ARRAY_SIZE(identity));
			f->crc_enabled = true;
		}
		return 0;
	}
	sail_check(f->elapsed_us >= 2000);
	if (f->active) {
		struct monaco_pmic_reset_result saved = f->result;

		n = f->reset_transfers++;
		sail_check(n < ARRAY_SIZE(f->trace));
		sail_check(tx[0] == reset_registers[n]);
		sail_check(f->result.last_register == tx[0]);
		sail_check(f->result.stage ==
		       (enum monaco_pmic_reset_stage)(MONACO_PMIC_RESET_WRITE_C9 + n));
		sail_check(tx_size == (n < 3 ? 3U : 1U));
		sail_check(rx_size == (n < 3 ? 0U : 2U));
		if (n < 3) {
			sail_check(tx[1] == reset_values[n] && tx[2] == reset_checks[n]);
		}
		f->trace[n] = (struct event){tx[0], n < 3 ? tx[1] : 0, n < 3 ? tx[2] : 0, tx_size,
					     rx_size};
		if (f->reenter) {
			sail_check(configure(f) == MONACO_PMIC_STATE);
			sail_check(memcmp(&saved, &f->result, sizeof(saved)) == 0);
		}
		fail_now = f->reset_transfers == f->fail_at;
		if (fail_now && !f->fail_after_effect) {
			return f->error;
		}
	}
	if (rx_size) {
		sail_check(tx_size == 1 && rx_size == 2 && rx);
		rx[0] = f->active && f->override_readback ? f->readback[n - 3U]
							  : f->registers[tx[0]];
		covered[3] = rx[0];
		rx[1] = crc(covered, 4);
		if (f->active && f->reset_transfers == f->corrupt_at) {
			rx[1] ^= f->corrupt_mask;
		}
	} else {
		sail_check(tx_size == 3 && !rx);
		covered[2] = tx[1];
		sail_check(tx[2] == crc(covered, 3));
		f->registers[tx[0]] = tx[1];
	}
	return fail_now ? f->error : 0;
}

static int delay(void *context, uint32_t us)
{
	struct fixture *f = context;

	sail_check(!f->active);
	f->delays++;
	f->elapsed_us += us;
	return 0;
}

static int time_us(void *context, uint64_t *value)
{
	struct fixture *f = context;

	sail_check(!f->active);
	f->time_reads++;
	*value = f->elapsed_us;
	return 0;
}

static int sample_read(void *context, uint32_t address, uint32_t *value)
{
	static const uint32_t addresses[] = {0xf0146014, 0xf0300028, 0xf1149024};
	struct fixture *f = context;
	uint32_t values[] = {f->sku, f->soc, f->fsm};

	sail_check(!f->active && f->sample_reads < ARRAY_SIZE(addresses));
	sail_check(address == addresses[f->sample_reads]);
	*value = values[f->sample_reads++];
	return 0;
}

static int probe(void *context, uint8_t address, uint32_t timeout)
{
	static const uint8_t addresses[] = {0x28, 0x29, 0x12};
	struct fixture *f = context;

	sail_check(!f->active && timeout == TIMEOUT_US && f->probes < ARRAY_SIZE(addresses));
	sail_check(address == addresses[f->probes++]);
	return 0;
}

static void prepare(struct fixture *f, uint32_t sku, uint32_t fsm)
{
	struct monaco_pmic_io io = {f, transfer, delay};
	struct monaco_pmic_policy_io policy_io = {f, sample_read, probe};
	struct monaco_pmic_boot_time time = {f, time_us};
	struct monaco_pmic_policy_selection selected;
	struct monaco_pmic_pretarget_inputs pretarget;
	struct monaco_pmic_tail_inputs tail;

	*f = (struct fixture){
		.sku = sku, .soc = 0x601a0100, .fsm = fsm, .error = IO_ERROR, .corrupt_mask = 1};
	f->registers[1] = 0x82;
	f->registers[2] = 0xa5;
	f->registers[3] = 2;
	f->registers[0x66] = 1;
	f->registers[0x81] = 8;
	for (unsigned int i = 0; i < 3; ++i) {
		f->registers[0x23 + i] = 8;
	}
	f->registers[0x26] = 32;
	for (unsigned int i = 0; i < 5; ++i) {
		f->registers[0x0e + 2 * i] = 20;
	}
	sail_check(monaco_pmic_init(&f->pmic, &io, TIMEOUT_US) == 0);
	sail_check(monaco_pmic_policy_sample(MONACO_PMIC_BOARD_VENTUNO_Q, &policy_io, TIMEOUT_US,
					 &f->snapshot) == 0);
	sail_check(monaco_pmic_identify(&f->pmic) == 0);
	sail_check(monaco_pmic_policy_select(&f->snapshot, &f->pmic, false, &selected) == 0);
	pretarget = (struct monaco_pmic_pretarget_inputs){
		.topology = selected.boot.topology,
		.present_slaves = selected.boot.present_slaves,
		.reset_fsm_valid = selected.boot.reset_fsm_valid,
		.reset_fsm = selected.boot.reset_fsm,
		.platform_type_valid = selected.boot.platform_type_valid,
		.platform_type = selected.boot.platform_type,
		/* Set by the test: the device model implements this subset. */
		.parameter_status_policy_validated = true,
	};
	tail = (struct monaco_pmic_tail_inputs){
		.regulator_posttarget_policy_validated = true,
		.regulators = selected.boot.regulators,
		.regulator_count = selected.boot.regulator_count,
	};
	sail_check(monaco_pmic_pretarget(&f->pmic, &pretarget, MONACO_PMIC_REQUIRE_BOOT_BIST,
				     &f->boot) == 0);
	sail_check(monaco_pmic_boot_tail(&f->pmic, &tail, &f->boot) == 0);
	sail_check(monaco_pmic_boot_finish(&f->pmic, &time, &f->boot) == 0);
	sail_check(f->boot.regulators[5].enable_skipped);
	sail_check(f->boot.settle_elapsed_valid && f->boot.settle_elapsed_us == 3000);
	sail_check(f->raw_reads == 4 && f->sample_reads == 3 && f->probes == 3);
	f->registers[0xc9] = 0xa5;
	f->registers[0xcb] = 0xa6;
	f->registers[0xcc] = 0xa7;
	f->active = true;
}

static void unchanged(const struct fixture *f, const struct monaco_pmic *pmic,
		      const struct monaco_pmic_boot_result *boot,
		      const struct monaco_pmic_policy_snapshot *snapshot)
{
	sail_check(memcmp(&f->pmic, pmic, sizeof(*pmic)) == 0);
	sail_check(memcmp(&f->boot, boot, sizeof(*boot)) == 0);
	sail_check(memcmp(&f->snapshot, snapshot, sizeof(*snapshot)) == 0);
}

static void no_reentry(struct fixture *f)
{
	struct monaco_pmic_reset_result saved = f->result;
	unsigned int transfers = f->transfers;

	sail_check(configure(f) == MONACO_PMIC_STATE);
	sail_check(f->transfers == transfers);
	sail_check(memcmp(&saved, &f->result, sizeof(saved)) == 0);
}

static void test_success(void)
{
	static const uint16_t skus[] = {0x026e, 0x026c, 0x026d, 0x028f, 0x02e6, 0x02e7, 0x02fb};
	static const uint8_t vectors[][4] = {
		{0x50, 0xc9, 0},       {0x50, 0xcb, 0x0a},    {0x50, 0xcc, 1},
		{0x50, 0xc9, 0x51, 0}, {0x50, 0xcc, 0x51, 1},
	};
	static const uint8_t checks[] = {0x5f, 0x43, 0x19, 0x83, 0x44};
	struct fixture f;

	for (unsigned int i = 0; i < ARRAY_SIZE(vectors); ++i) {
		sail_check(crc(vectors[i], i < 3 ? 3 : 4) == checks[i]);
	}
	for (unsigned int s = 0; s < ARRAY_SIZE(skus); ++s) {
		for (unsigned int fsm = 0; fsm < 12; ++fsm) {
			struct monaco_pmic pmic;
			struct monaco_pmic_boot_result boot;
			struct monaco_pmic_policy_snapshot snapshot;
			uint8_t before[256];
			unsigned int transfers, delays, times;

			prepare(&f, UINT32_C(0xffff0000) | skus[s], UINT32_C(0xfffffff0) | fsm);
			pmic = f.pmic;
			boot = f.boot;
			snapshot = f.snapshot;
			memcpy(before, f.registers, sizeof(before));
			transfers = f.transfers;
			delays = f.delays;
			times = f.time_reads;
			f.reenter = true;
			sail_check(configure(&f) == 0);
			sail_check(f.result.stage == MONACO_PMIC_RESET_VERIFIED &&
				   !f.result.status);
			sail_check(f.result.writes_attempted == 3 &&
				   f.result.writes_completed == 3);
			sail_check(f.result.reads_attempted == 2 && f.result.reads_completed == 2);
			sail_check(f.result.c9_valid && f.result.cc_valid);
			sail_check(f.result.observed_c9 == 0 && f.result.observed_cc == 1);
			sail_check(f.transfers == transfers + 5 && f.reset_transfers == 5);
			sail_check(f.delays == delays && f.time_reads == times);
			for (unsigned int r = 0; r < 256; ++r) {
				uint8_t expected = r == 0xc9   ? 0
						   : r == 0xcb ? 0x0a
						   : r == 0xcc ? 1
							       : before[r];
				sail_check(f.registers[r] == expected);
			}
			unchanged(&f, &pmic, &boot, &snapshot);
			no_reentry(&f);
		}
	}
}

static void test_transfer_failures(void)
{
	static const int errors[] = {IO_ERROR, -1, INT_MIN, 1, INT_MAX};
	struct fixture f;

	for (unsigned int n = 1; n <= 5; ++n) {
		for (unsigned int e = 0; e < ARRAY_SIZE(errors); ++e) {
			for (unsigned int after = 0; after < 2; ++after) {
				struct monaco_pmic pmic;
				struct monaco_pmic_boot_result boot;
				struct monaco_pmic_policy_snapshot snapshot;
				int expected = errors[e] > 0 ? MONACO_PMIC_CALLBACK : errors[e];

				prepare(&f, 0x02e6, 0);
				pmic = f.pmic;
				boot = f.boot;
				snapshot = f.snapshot;
				f.fail_at = n;
				f.error = errors[e];
				f.fail_after_effect = after != 0;
				sail_check(configure(&f) == expected &&
					   f.result.status == expected);
				sail_check(f.result.stage ==
				       (enum monaco_pmic_reset_stage)(MONACO_PMIC_RESET_WRITE_C9 +
								      n - 1));
				sail_check(f.reset_transfers == n);
				sail_check(f.result.writes_attempted == (n <= 3 ? n : 3));
				sail_check(f.result.writes_completed == (n <= 3 ? n - 1 : 3));
				sail_check(f.result.reads_attempted == (n <= 3 ? 0 : n - 3));
				sail_check(f.result.reads_completed == (n <= 3 ? 0 : n - 4));
				sail_check(f.result.c9_valid == (n == 5) && !f.result.cc_valid);
				sail_check(f.result.observed_c9 == 0 && f.result.observed_cc == 0);
				if (n <= 3) {
					static const uint8_t regs[] = {0xc9, 0xcb, 0xcc};
					static const uint8_t values[] = {0, 0x0a, 1};

					sail_check(f.registers[regs[n - 1]] ==
					       (after ? values[n - 1] : (uint8_t)(0xa4 + n)));
				}
				unchanged(&f, &pmic, &boot, &snapshot);
				no_reentry(&f);
			}
		}
	}
	for (unsigned int n = 4; n <= 5; ++n) {
		for (unsigned int bit = 0; bit < 8; ++bit) {
			prepare(&f, 0x02e6, 0);
			f.corrupt_at = n;
			f.corrupt_mask = (uint8_t)(1U << bit);
			sail_check(configure(&f) == MONACO_PMIC_CRC);
			sail_check(f.result.status == MONACO_PMIC_CRC && f.reset_transfers == n);
			sail_check(f.result.reads_attempted == n - 3 &&
			       f.result.reads_completed == n - 4);
			sail_check(f.result.c9_valid == (n == 5) && !f.result.cc_valid);
			no_reentry(&f);
		}
	}
}

static void test_readback_pairs(void)
{
	struct fixture f;

	prepare(&f, 0x02e6, 0);
	f.override_readback = true;
	for (unsigned int c9 = 0; c9 < 256; ++c9) {
		for (unsigned int cc = 0; cc < 256; ++cc) {
			bool accepted = c9 == 0 && cc == 1;

			/* Separate readback cases, not a retry policy. */
			f.result = (struct monaco_pmic_reset_result){0};
			f.reset_transfers = 0;
			f.readback[0] = (uint8_t)c9;
			f.readback[1] = (uint8_t)cc;
			sail_check(configure(&f) == (accepted ? 0 : MONACO_PMIC_RESET_VERIFY));
			sail_check(f.reset_transfers == 5 && f.result.c9_valid &&
				   f.result.cc_valid);
			sail_check(f.result.observed_c9 == c9 && f.result.observed_cc == cc);
			sail_check(f.result.stage ==
			       (accepted ? MONACO_PMIC_RESET_VERIFIED : MONACO_PMIC_RESET_COMPARE));
			no_reentry(&f);
		}
	}
}

static void test_guards(void)
{
	struct fixture f;

	for (unsigned int fault = 0; fault < 64; ++fault) {
		const struct monaco_pmic_policy_snapshot *snapshot;
		const struct monaco_pmic_boot_result *boot;
		struct monaco_pmic *pmic;
		struct monaco_pmic_boot_result copied;
		int expected = MONACO_PMIC_STATE;

		prepare(&f, 0x02e6, 0);
		snapshot = &f.snapshot;
		boot = &f.boot;
		pmic = &f.pmic;
		switch (fault) {
		case 0:
			pmic = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 1:
			snapshot = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 2:
			boot = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 3:
			f.pmic.io.transfer = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 4:
			f.pmic.io.delay_us = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 5:
			f.pmic.transfer_timeout_us = 0;
			expected = MONACO_PMIC_INVALID;
			break;
		case 6:
			f.pmic.state = MONACO_PMIC_IDENTIFIED;
			break;
		case 7:
			f.pmic.version.device = 0x81;
			break;
		case 8:
			f.pmic.version.revision ^= 1;
			break;
		case 9:
			f.pmic.version.firmware ^= 1;
			break;
		case 10:
			f.pmic.version.firmware_revision ^= 1;
			break;
		case 11:
			f.pmic.io.context = NULL;
			break;
		case 12:
			f.pmic.transfer_timeout_us++;
			break;
		case 13:
			f.snapshot.profile = MONACO_PMIC_BOARD_UNKNOWN;
			break;
		case 14:
			f.snapshot.sampled = false;
			break;
		case 15:
			f.snapshot.status = IO_ERROR;
			break;
		case 16:
			f.snapshot.stage = MONACO_PMIC_POLICY_PROBE;
			break;
		case 17:
			f.snapshot.last_callback_status = IO_ERROR;
			break;
		case 18:
			f.snapshot.reads_attempted++;
			break;
		case 19:
			f.snapshot.reads_completed--;
			break;
		case 20:
			f.snapshot.probes_attempted++;
			break;
		case 21:
			f.snapshot.probes_completed--;
			break;
		case 22:
			f.snapshot.raw_sku = 0xffff;
			break;
		case 23:
			f.snapshot.raw_soc_identity ^= 0x10000;
			break;
		case 24:
			f.snapshot.raw_reset_fsm = 12;
			break;
		case 25:
			f.snapshot.raw_reset_fsm = 13;
			break;
		case 26:
			f.snapshot.raw_reset_fsm = 14;
			break;
		case 27:
			f.snapshot.raw_reset_fsm = UINT32_MAX;
			break;
		case 28:
			copied = f.boot;
			boot = &copied;
			break;
		case 29:
			f.boot.tail_owner = NULL;
			break;
		case 30:
			f.boot.tail_result = NULL;
			break;
		case 31:
			f.boot.status = IO_ERROR;
			break;
		case 32:
			f.boot.stage = MONACO_PMIC_BOOT_TAIL_COMPLETE;
			break;
		case 33:
			f.boot.tail_context.state = MONACO_PMIC_TRANSITION;
			break;
		case 34:
			f.boot.tail_context.io.context = NULL;
			break;
		case 35:
			f.boot.tail_context.io.transfer = NULL;
			break;
		case 36:
			f.boot.tail_context.io.delay_us = NULL;
			break;
		case 37:
			f.boot.tail_context.transfer_timeout_us++;
			break;
		case 38:
			f.boot.tail_context.version.device ^= 1;
			break;
		case 39:
			f.boot.tail_context.version.revision ^= 1;
			break;
		case 40:
			f.boot.tail_context.version.firmware ^= 1;
			break;
		case 41:
			f.boot.tail_context.version.firmware_revision ^= 1;
			break;
		case 42:
			f.boot.operations_attempted++;
			break;
		case 43:
			f.boot.last_operation = MONACO_PMIC_BOOT_WRITE;
			break;
		case 44:
			f.boot.last_delay_us = 2999;
			break;
		case 45:
			f.boot.last_callback_status = IO_ERROR;
			break;
		case 46:
			f.boot.delay_us_completed = 4999;
			break;
		case 47:
			f.boot.boot_bist_flags_valid = false;
			break;
		case 48:
			f.boot.boot_bist_accepted = false;
			break;
		case 49:
			f.boot.boot_bist_accepted = false;
			f.boot.runtime_bist_passed = true;
			break;
		case 50:
			f.boot.regulators_completed--;
			break;
		case 51:
			f.boot.posttarget_writes_completed--;
			break;
		case 52:
			f.boot.settle_time_reads_attempted++;
			break;
		case 53:
			f.boot.settle_time_reads_completed--;
			break;
		case 54:
			f.boot.settle_elapsed_valid = false;
			break;
		case 55:
			f.boot.settle_elapsed_us = 2999;
			break;
		case 56:
			f.boot.regulators[0].stage = MONACO_PMIC_REGULATOR_ENABLE;
			break;
		case 57:
			f.boot.regulators[1].voltage_valid = false;
			break;
		case 58:
			f.boot.regulators[2].mode_valid = false;
			break;
		case 59:
			f.boot.regulators[3].enable_valid = false;
			break;
		case 60:
			f.boot.regulators[4].stage = MONACO_PMIC_REGULATOR_ENABLE;
			break;
		case 61:
			f.boot.regulators[5].voltage_valid = false;
			break;
		case 62:
			f.boot.regulators[7].mode_valid = false;
			break;
		case 63:
			f.boot.regulators[8].enable_valid = false;
			break;
		default:
			sail_check(false);
		}
		sail_check(monaco_pmic_reset_configure_readback(pmic, snapshot, boot, &f.result) ==
		       expected);
		sail_check(f.result.status == expected &&
			   f.result.stage == MONACO_PMIC_RESET_VALIDATE);
		sail_check(!f.reset_transfers && !f.result.writes_attempted &&
		       !f.result.reads_attempted);
		no_reentry(&f);
	}
	prepare(&f, 0x02e6, 0);
	sail_check(monaco_pmic_reset_configure_readback(&f.pmic, &f.snapshot, &f.boot, NULL) ==
	       MONACO_PMIC_INVALID);
	sail_check(!f.reset_transfers);
}

static void test_each_rail_guard(void)
{
	struct fixture f;

	for (unsigned int rail = 0; rail < MONACO_PMIC_BOOT_REGULATORS; ++rail) {
		for (unsigned int field = 0; field < 4; ++field) {
			struct monaco_pmic_regulator_result *r;

			prepare(&f, 0x02e6, 0);
			r = &f.boot.regulators[rail];
			switch (field) {
			case 0:
				r->stage = MONACO_PMIC_REGULATOR_ENABLE;
				break;
			case 1:
				r->voltage_valid = false;
				break;
			case 2:
				r->mode_valid = false;
				break;
			case 3:
				r->enable_valid = false;
				break;
			default:
				sail_check(false);
			}
			sail_check(configure(&f) == MONACO_PMIC_STATE);
			sail_check(!f.reset_transfers);
			no_reentry(&f);
		}
	}
}

struct direct_guard {
	unsigned int count, fail_at;
	int error;
};

static int direct_check(void *context)
{
	struct direct_guard *guard = context;

	return ++guard->count == guard->fail_at ? guard->error : 0;
}

static void test_direct_guard(void)
{
	struct fixture f;

	for (unsigned int failure = 1; failure <= 6; failure++) {
		for (unsigned int positive = 0; positive < 2; positive++) {
			prepare(&f, 0x02e7, 0);
			struct direct_guard guard = {0, failure, positive ? 9 : IO_ERROR};
			int expected = positive ? MONACO_PMIC_CALLBACK : IO_ERROR;

			sail_check(monaco_pmic_reset_configure_readback_guarded(
				       &f.pmic, &f.snapshot, &f.boot, &f.result, direct_check,
				       &guard) == expected);
			sail_check(f.result.guard_status == guard.error &&
				   f.result.status == expected);
			sail_check(f.result.guards_attempted == failure &&
			       f.result.guards_completed == failure - 1);
			sail_check(f.reset_transfers == failure - 1 &&
			       f.result.stage != MONACO_PMIC_RESET_VERIFIED);
			no_reentry(&f);
		}
	}
}

int sail_model_test(void)
{
	test_success();
	test_transfer_failures();
	test_readback_pairs();
	test_guards();
	test_each_rail_guard();
	test_direct_guard();
	puts("PMIC reset configure/readback: real carried boot, 84 profile/FSM cases, "
	     "50 transport faults, 16 CRC faults, 65536 pairs, 100 provenance guards and 12 "
	     "live-guard failures passed");
	return 0;
}
