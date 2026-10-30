/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_boot.h"

#include "sail_check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>


#define TIMEOUT_US 5000U
#define IO_ERROR   (-77)

struct event {
	char kind;
	uint8_t address;
	uint8_t reg;
	uint8_t value;
	uint32_t delay;
	enum monaco_pmic_boot_stage stage;
};

struct fixture {
	struct event expected[384];
	unsigned int length;
	unsigned int next;
	unsigned int fail_at;
	unsigned int corrupt_at;
	int failure;
	enum monaco_pmic_boot_stage stage;
	struct monaco_pmic_boot_result *result;
	uint64_t elapsed_us;
};

/* Polynomial division over a complete frame, independent of the driver. */
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

static void event(struct fixture *f, char kind, uint8_t address, uint8_t reg, uint8_t value,
		  uint32_t delay)
{
	sail_check(f->length < ARRAY_SIZE(f->expected));
	f->expected[f->length++] = (struct event){
		.kind = kind,
		.address = address,
		.reg = reg,
		.value = value,
		.delay = delay,
		.stage = f->stage,
	};
}

static void rd(struct fixture *f, uint8_t reg, uint8_t value)
{
	event(f, 'R', 0x28, reg, value, 0);
}

static void wr(struct fixture *f, uint8_t reg, uint8_t value)
{
	event(f, 'W', 0x28, reg, value, 0);
}

static void rw(struct fixture *f, uint8_t reg, uint8_t old, uint8_t value)
{
	rd(f, reg, old);
	wr(f, reg, value);
}

static void wait_us(struct fixture *f, uint32_t us)
{
	event(f, 'D', 0, 0, 0, us);
}

static const struct event *consume(struct fixture *f)
{
	const struct event *e;

	sail_check(f->next < f->length);
	e = &f->expected[f->next++];
	sail_check(f->result->stage == e->stage);
	sail_check(f->result->operations_attempted == f->next);
	return e;
}

static int transfer(void *context, uint8_t address, const uint8_t *tx, size_t tx_size, uint8_t *rx,
		    size_t rx_size, uint32_t timeout)
{
	struct fixture *f = context;
	const struct event *e = consume(f);
	uint8_t covered[4];

	sail_check(timeout == TIMEOUT_US && e->address == address);
	sail_check(tx && tx[0] == e->reg);
	sail_check(f->result->last_address7 == address && f->result->last_register == tx[0]);
	covered[0] = (uint8_t)(address << 1);
	covered[1] = tx[0];
	if (e->kind == 'R') {
		sail_check(tx_size == 1 && rx && rx_size == 2);
		sail_check(f->result->last_operation == MONACO_PMIC_BOOT_READ);
		rx[0] = e->value;
		covered[2] = (uint8_t)(covered[0] | 1U);
		covered[3] = e->value;
		rx[1] = crc(covered, 4);
		if (f->next == f->corrupt_at) {
			rx[1] ^= 1U;
		}
	} else {
		sail_check(e->kind == 'W' && !rx && !rx_size);
		sail_check(f->result->last_operation == MONACO_PMIC_BOOT_WRITE);
		sail_check(tx[1] == e->value);
		if (e->stage == MONACO_PMIC_BOOT_SUPPORT) {
			sail_check(tx_size == 2 && e->reg == 0x85 && e->value == 2);
		} else {
			sail_check(tx_size == 3);
			covered[2] = e->value;
			sail_check(tx[2] == crc(covered, 3));
		}
	}
	return f->next == f->fail_at ? f->failure : 0;
}

static int delay(void *context, uint32_t us)
{
	struct fixture *f = context;
	const struct event *e = consume(f);

	sail_check(e->kind == 'D' && e->delay == us);
	sail_check(f->result->last_operation == MONACO_PMIC_BOOT_DELAY);
	sail_check(f->result->last_delay_us == us);
	/* A failing callback may already have waited; the driver must not retry. */
	f->elapsed_us += us;
	return f->next == f->fail_at ? f->failure : 0;
}

static void make_policy(struct monaco_pmic_regulator_policy *p)
{
	/* Typed board inputs used as the test fixture. */
	for (unsigned int i = 0; i < 9; ++i) {
		bool ldo = i < 4;
		unsigned int index = ldo ? i : i - 4U;

		p[i] = (struct monaco_pmic_regulator_policy){
			.group = ldo ? MONACO_PMIC_LDO : MONACO_PMIC_SMPS,
			.index = index,
			.write_permitted = ldo ? index != 2 : index >= 2,
			.enable_required = ldo ? (index == 0 || index == 3) : index != 0,
			.required_mode = ldo ? 2 : 0,
			.minimum_uv = ldo ? (index == 3 ? 1200000U : 600000U) : 300000U,
			.maximum_uv = ldo ? 3300000U : 3340000U,
		};
	}
}

static void prepare(struct fixture *f, struct monaco_pmic *pmic,
		    struct monaco_pmic_boot_inputs *inputs,
		    struct monaco_pmic_regulator_policy *policies,
		    struct monaco_pmic_boot_result *result)
{
	struct monaco_pmic_io io = {f, transfer, delay};

	*f = (struct fixture){.failure = IO_ERROR, .result = result};
	*result = (struct monaco_pmic_boot_result){0};
	sail_check(monaco_pmic_init(pmic, &io, TIMEOUT_US) == 0);
	/* Identification is a separately tested caller prerequisite. */
	pmic->state = MONACO_PMIC_IDENTIFIED;
	pmic->version = (struct monaco_pmic_version){0x82, 0xa5, 2, 0};
	make_policy(policies);
	*inputs = (struct monaco_pmic_boot_inputs){
		.topology = MONACO_PMIC_TOPOLOGY_INDEPENDENT,
		.present_slaves = 7,
		.reset_fsm_valid = true,
		.reset_fsm = 0,
		.platform_type_valid = true,
		.platform_type = 0x0f,
		.supply_policy_validated = true,
		.regulators = policies,
		.regulator_count = 9,
	};
}

static void pretarget(struct fixture *f, uint32_t platform, uint8_t first, uint8_t second)
{
	f->stage = MONACO_PMIC_BOOT_SUPPORT;
	wr(f, 0x85, 2);
	wait_us(f, 2000);
	f->stage = MONACO_PMIC_BOOT_PLATFORM;
	if (platform == 0x0f) {
		rw(f, 0x0d, 0xc2, 0xc5);
	}
	f->stage = MONACO_PMIC_BOOT_PARAMETERS;
	wr(f, 0xcd, 0x3b);
	wr(f, 0xce, 0x3b);
	wr(f, 0xcf, 0x49);
	wr(f, 0xd0, 0x49);
	rw(f, 0xca, 0xa5, 0xa3);
	f->stage = MONACO_PMIC_BOOT_FLAGS;
	rd(f, 0x66, first);
	rd(f, 0x81, second);
}

static void status_trace(struct fixture *f, uint8_t top, uint8_t subs, uint8_t remaining)
{
	f->stage = MONACO_PMIC_BOOT_STATUS;
	rd(f, 0x5a, top);
	if (top & 0x80U) {
		rd(f, 0x69, subs);
		if (subs & 0x80U) {
			event(f, 'R', 0x12, 0x08, 0xa5, 0);
			event(f, 'W', 0x12, 0x08, 0xa5, 0);
		}
		if (subs & 0x40U) {
			rw(f, 0x6c, 0x11, 0x11);
		}
		if (subs & 0x20U) {
			rw(f, 0x6b, 0x22, 0x22);
		}
		if (subs & 0x10U) {
			rw(f, 0x6a, 0x33, 0x33);
		}
		/* New sub-bitmap value must be reread before the final writeback. */
		rw(f, 0x69, 0x5a, 0x5a);
	}
	if (top & 0x40U) {
		rw(f, 0x68, 0x44, 0x44);
	}
	if (top & 0x20U) {
		rw(f, 0x67, 0x55, 0x55);
	}
	if (top & 0x10U) {
		/* This later status value must not replace saved boot BIST flags. */
		rw(f, 0x66, 0x80, 0x80);
	}
	if (top & 0x08U) {
		rw(f, 0x65, 0x77, 0x77);
	}
	if (top & 0x04U) {
		rd(f, 0x63, subs);
		if (subs & 0x08U) {
			rw(f, 0x64, 0x88, 0x88);
		}
		rw(f, 0x63, 0xa7, 0xa7);
	}
	if (top & 0x02U) {
		rd(f, 0x5f, subs);
		if (subs & 0x10U) {
			rw(f, 0x62, 0x99, 0x99);
		}
		if (subs & 0x02U) {
			rw(f, 0x61, 0xaa, 0xaa);
		}
		if (subs & 0x01U) {
			rw(f, 0x60, 0xbb, 0xbb);
		}
	}
	if (top & 0x01U) {
		rd(f, 0x5b, subs);
		if (subs & 0x04U) {
			rw(f, 0x5e, 0xcc, 0xcc);
		}
		if (subs & 0x02U) {
			rw(f, 0x5d, 0xdd, 0xdd);
		}
		if (subs & 0x01U) {
			rw(f, 0x5c, 0xee, 0xee);
		}
	}
	rd(f, 0x5a, remaining);
}

/* Explicit traces for paired control bits, preserving the other fields. */
static void enable_even(struct fixture *f, uint8_t reg)
{
	rw(f, reg, 0xc0, 0xc2);
	rw(f, reg, 0xc2, 0xc3);
}

static void enable_odd(struct fixture *f, uint8_t reg)
{
	rw(f, reg, 0xc3, 0xe3);
	rw(f, reg, 0xe3, 0xf3);
}

static void clear_even(struct fixture *f, uint8_t reg, bool has_odd)
{
	if (has_odd) {
		rw(f, reg, 0xf3, 0xf1);
		rw(f, reg, 0xf1, 0xf0);
	} else {
		rw(f, reg, 0xc3, 0xc1);
		rw(f, reg, 0xc1, 0xc0);
	}
}

static void clear_odd(struct fixture *f, uint8_t reg)
{
	rw(f, reg, 0xf0, 0xd0);
	rw(f, reg, 0xd0, 0xc0);
}

static void runtime_trace(struct fixture *f, uint8_t first, uint8_t second, bool pass)
{
	f->stage = MONACO_PMIC_BOOT_BIST_ISOLATE_LDO;
	rd(f, 0x1d, 0x93);
	rw(f, 0x1d, 0xb3, 0xa3);
	rd(f, 0x1e, 0x80);
	rd(f, 0x1f, 0x91);
	rw(f, 0x1f, 0x91, 0x81);
	rd(f, 0x20, 0x82);
	f->stage = MONACO_PMIC_BOOT_BIST_GATE;
	enable_even(f, 0x4e);
	f->stage = MONACO_PMIC_BOOT_BIST_SMPS;
	enable_even(f, 0x49);
	enable_odd(f, 0x49);
	enable_even(f, 0x4a);
	enable_odd(f, 0x4a);
	enable_even(f, 0x4b);
	f->stage = MONACO_PMIC_BOOT_BIST_LDO;
	enable_even(f, 0x4c);
	enable_odd(f, 0x4c);
	enable_even(f, 0x4d);
	enable_odd(f, 0x4d);
	f->stage = MONACO_PMIC_BOOT_BIST_RUN;
	wr(f, 0x85, 4);
	wait_us(f, 1800);
	f->stage = MONACO_PMIC_BOOT_BIST_VERIFY;
	rd(f, 0x66, first);
	rd(f, 0x81, second);
	if (!pass) {
		return;
	}
	f->stage = MONACO_PMIC_BOOT_BIST_UNGATE;
	clear_even(f, 0x4e, false);
	f->stage = MONACO_PMIC_BOOT_BIST_RESTORE_SMPS;
	clear_even(f, 0x49, true);
	clear_odd(f, 0x49);
	clear_even(f, 0x4a, true);
	clear_odd(f, 0x4a);
	clear_even(f, 0x4b, false);
	f->stage = MONACO_PMIC_BOOT_BIST_RESTORE_LDO;
	/* Changed enable state must be read again rather than using initial bits. */
	rd(f, 0x1d, 0x80);
	rd(f, 0x1e, 0x41);
	rw(f, 0x1e, 0x41, 0x51);
	rd(f, 0x1f, 0x91);
	rw(f, 0x1f, 0x91, 0x91);
	rd(f, 0x20, 0xc1);
	rw(f, 0x20, 0xc1, 0xd1);
	f->stage = MONACO_PMIC_BOOT_BIST_SETTLE_LDO;
	rd(f, 0x1d, 0x80);
	wait_us(f, 1000);
	clear_even(f, 0x4c, true);
	rd(f, 0x1e, 0x53);
	wait_us(f, 3500);
	clear_odd(f, 0x4c);
	rd(f, 0x1f, 0x91);
	wait_us(f, 1000);
	clear_even(f, 0x4d, true);
	rd(f, 0x20, 0xd3);
	wait_us(f, 3500);
	clear_odd(f, 0x4d);
}

static void regulator_trace(struct fixture *f, const struct monaco_pmic_regulator_policy *policies)
{
	f->stage = MONACO_PMIC_BOOT_REGULATOR;
	for (unsigned int i = 0; i < 9; ++i) {
		const struct monaco_pmic_regulator_policy *p = &policies[i];
		uint8_t control;

		if (i < 4) {
			control = (uint8_t)(0x1dU + i);
			rd(f, (uint8_t)(0x23U + i), i == 3 ? 32 : 8);
		} else {
			control = (uint8_t)(4U + 2U * (i - 4U));
			rd(f, control, 0x80);
			rd(f, (uint8_t)(control + 0x0aU), 15);
			rd(f, control, 0x80);
		}
		rd(f, control, 0x80);
		if (p->write_permitted && p->enable_required) {
			rw(f, control, 0x80, 0x81);
			rw(f, control, 0x81, i < 4 ? 0x91 : 0xb1);
			wait_us(f, 1000);
		}
	}
}

static void posttarget(struct fixture *f)
{
	f->stage = MONACO_PMIC_BOOT_POSTTARGET;
	wr(f, 0xc9, 0x00);
	wr(f, 0xcb, 0x0a);
	wr(f, 0xcc, 0x00);
	wr(f, 0x4f, 0xff);
	wr(f, 0x50, 0xff);
	wr(f, 0x51, 0x3f);
	f->stage = MONACO_PMIC_BOOT_SETTLE;
	wait_us(f, 3000);
}

static void full_trace(struct fixture *f, const struct monaco_pmic_boot_inputs *inputs,
		       bool runtime)
{
	pretarget(f, inputs->platform_type, runtime ? 0 : 1, 8);
	status_trace(f, 0xff, 0xff, 0x24);
	if (runtime) {
		runtime_trace(f, 1, 8, true);
	}
	regulator_trace(f, inputs->regulators);
	posttarget(f);
}

static void assert_complete(const struct fixture *f, const struct monaco_pmic *pmic,
			    const struct monaco_pmic_boot_result *r)
{
	sail_check(f->next == f->length && r->operations_attempted == f->length);
	sail_check(r->operations_completed == f->length && !r->last_callback_status);
	sail_check(r->stage == MONACO_PMIC_BOOT_COMPLETE && r->status == 0);
	sail_check(r->regulators_completed == 9);
	sail_check(pmic->state == MONACO_PMIC_CRC_READY);
	sail_check(pmic->io.context == f && pmic->io.transfer == transfer &&
	       pmic->io.delay_us == delay);
}

static void test_complete_traces(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];

	for (unsigned int runtime = 0; runtime < 2; ++runtime) {
		prepare(&f, &pmic, &inputs, policies, &result);
		full_trace(&f, &inputs, runtime != 0);
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == 0);
		assert_complete(&f, &pmic, &result);
		sail_check(result.status_initial == 0xff && result.status_residual == 0x24);
		sail_check(result.status_initial_valid && result.status_residual_valid);
		sail_check(result.boot_bist_flags_valid && result.boot_bist_flag == !runtime);
		sail_check(result.runtime_bist_attempted == (runtime != 0));
		sail_check(result.runtime_bist_passed == (runtime != 0));
		sail_check(result.runtime_bist_cleanup_complete == (runtime != 0));
		sail_check(result.regulators[5].enable_skipped);
		sail_check(!result.regulators[2].enable_sequence_complete);
		sail_check(result.delay_us_completed == (runtime ? 20800U : 10000U));
		printf("PMIC boot %s trace: %u operations\n",
		       runtime ? "runtime BIST" : "boot BIST", f.length);
		unsigned int before = f.next;

		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == MONACO_PMIC_STATE);
		sail_check(f.next == before && result.operations_attempted == 0);
	}
}

static void test_platform_and_status_branches(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];
	static const uint32_t platforms[] = {0, 0x0e, 0x0f, 0x10, 0x10f, UINT32_MAX};

	for (unsigned int p = 0; p < ARRAY_SIZE(platforms); ++p) {
		prepare(&f, &pmic, &inputs, policies, &result);
		inputs.platform_type = platforms[p];
		pretarget(&f, inputs.platform_type, 1, 8);
		status_trace(&f, 0, 0xff, 0);
		regulator_trace(&f, policies);
		posttarget(&f);
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == 0);
		assert_complete(&f, &pmic, &result);
		sail_check(result.status_initial_valid && result.status_residual_valid);
		sail_check(!result.status_initial && !result.status_residual);
	}
	/* Every top bitmap, and each nested bitmap bit independently. */
	for (unsigned int top = 0; top <= UINT8_MAX; ++top) {
		prepare(&f, &pmic, &inputs, policies, &result);
		pretarget(&f, inputs.platform_type, 1, 8);
		status_trace(&f, (uint8_t)top, 0xff, 0);
		regulator_trace(&f, policies);
		posttarget(&f);
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == 0);
		assert_complete(&f, &pmic, &result);
		sail_check(result.status_initial == top && !result.status_residual);
	}
	for (unsigned int bit = 0; bit <= 8; ++bit) {
		prepare(&f, &pmic, &inputs, policies, &result);
		pretarget(&f, inputs.platform_type, 1, 8);
		status_trace(&f, 0xff, (uint8_t)(bit == 8 ? 0U : (1U << bit)), 0);
		regulator_trace(&f, policies);
		posttarget(&f);
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == 0);
		assert_complete(&f, &pmic, &result);
	}
}

static void test_bist_acceptance(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];
	static const struct {
		uint8_t firmware, revision;
		bool exception;
	} versions[] = {
		{1, 0, true},
		{1, 1, false},
		{0, 0, false},
		{0x81, 0, false},
	};

	for (unsigned int v = 0; v < ARRAY_SIZE(versions); ++v) {
		for (unsigned int flags = 0; flags < 4; ++flags) {
			bool first = (flags & 1U) != 0;
			bool second = (flags & 2U) != 0;
			bool accepted = first && (second || versions[v].exception);

			prepare(&f, &pmic, &inputs, policies, &result);
			pmic.version.firmware = versions[v].firmware;
			pmic.version.firmware_revision = versions[v].revision;
			/* Other bits deliberately set; only 66:0 and 81:3 count. */
			pretarget(&f, inputs.platform_type, first ? 0xff : 0xfe,
				  second ? 0xff : 0xf7);
			status_trace(&f, 0, 0, 0);
			if (!accepted) {
				runtime_trace(&f, 1, 8, true);
			}
			regulator_trace(&f, policies);
			posttarget(&f);
			sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == 0);
			assert_complete(&f, &pmic, &result);
			sail_check(result.runtime_bist_attempted == !accepted);

			prepare(&f, &pmic, &inputs, policies, &result);
			pmic.version.firmware = versions[v].firmware;
			pmic.version.firmware_revision = versions[v].revision;
			pretarget(&f, inputs.platform_type, 0, 0);
			status_trace(&f, 0, 0, 0);
			runtime_trace(&f, first ? 0xff : 0xfe, second ? 0xff : 0xf7, accepted);
			if (accepted) {
				regulator_trace(&f, policies);
				posttarget(&f);
			}
			int expected = accepted ? 0 : MONACO_PMIC_BOOT_BIST_FAILED;

			sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == expected);
			sail_check(f.next == f.length && result.status == expected);
			sail_check(result.runtime_bist_flags_valid &&
			       result.runtime_bist_passed == accepted);
			sail_check(result.runtime_bist_cleanup_complete == accepted);
			if (accepted) {
				assert_complete(&f, &pmic, &result);
			} else {
				sail_check(result.stage == MONACO_PMIC_BOOT_BIST_VERIFY);
				sail_check(result.regulators_completed == 0);
			}
		}
	}
}

static void test_every_operation_failure(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];
	unsigned int length;

	prepare(&f, &pmic, &inputs, policies, &result);
	full_trace(&f, &inputs, true);
	length = f.length;
	for (unsigned int at = 1; at <= length; ++at) {
		for (unsigned int positive = 0; positive < 2; ++positive) {
			prepare(&f, &pmic, &inputs, policies, &result);
			full_trace(&f, &inputs, true);
			f.fail_at = at;
			f.failure = positive ? 0x16 : IO_ERROR;
			int expected = positive ? MONACO_PMIC_CALLBACK : IO_ERROR;

			sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == expected);
			sail_check(f.next == at && result.operations_attempted == at);
			sail_check(result.operations_completed == at - 1U &&
				   result.status == expected);
			sail_check(result.last_callback_status == f.failure);
			sail_check(result.stage == f.expected[at - 1U].stage);
			sail_check(pmic.state ==
			       (at <= 2 ? MONACO_PMIC_TRANSITION : MONACO_PMIC_CRC_READY));
			sail_check(pmic.io.context == &f && pmic.io.transfer == transfer &&
			       pmic.io.delay_us == delay);
			uint32_t elapsed = 0;

			for (unsigned int i = 0; i + 1U < at; ++i) {
				elapsed += f.expected[i].delay;
			}
			sail_check(result.delay_us_completed == elapsed);
		}
		prepare(&f, &pmic, &inputs, policies, &result);
		full_trace(&f, &inputs, true);
		if (f.expected[at - 1U].kind == 'R') {
			f.corrupt_at = at;
			sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == MONACO_PMIC_CRC);
			sail_check(f.next == at && result.operations_completed == at);
			sail_check(result.stage == f.expected[at - 1U].stage);
			sail_check(!result.last_callback_status &&
				   result.status == MONACO_PMIC_CRC);
		}
	}
}

static void test_regulator_failure_boundaries(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];

	for (unsigned int index = 0; index < 9; ++index) {
		prepare(&f, &pmic, &inputs, policies, &result);
		pretarget(&f, inputs.platform_type, 1, 8);
		status_trace(&f, 0, 0, 0);
		regulator_trace(&f, policies);
		/* Fail bounds after a valid read, before any later rail or posttarget. */
		policies[index].maximum_uv = index == 3 ? 1199999U : 599999U;
		policies[index].minimum_uv = 0;
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == MONACO_PMIC_RANGE);
		sail_check(result.stage == MONACO_PMIC_BOOT_REGULATOR &&
			   result.rail_index == index);
		sail_check(result.regulators_completed == index);
		sail_check(result.regulators[index].stage == MONACO_PMIC_REGULATOR_VOLTAGE);
		sail_check(result.regulators[index].voltage_valid &&
		       !result.regulators[index].mode_valid);
		sail_check(f.next < f.length);
	}
}

static void test_input_guards(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];

	prepare(&f, &pmic, &inputs, policies, &result);
	sail_check(monaco_pmic_boot(NULL, &inputs, &result) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_boot(&pmic, NULL, &result) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_boot(&pmic, &inputs, NULL) == MONACO_PMIC_INVALID);
	for (unsigned int bit = 0; bit < 8; ++bit) {
		prepare(&f, &pmic, &inputs, policies, &result);
		inputs.present_slaves = (uint8_t)(7U ^ (1U << bit));
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == MONACO_PMIC_BOOT_TOPOLOGY);
		sail_check(!f.next && !result.operations_attempted);
	}
	for (unsigned int invalid = 0; invalid < 18; ++invalid) {
		prepare(&f, &pmic, &inputs, policies, &result);
		int expected = MONACO_PMIC_INVALID;

		switch (invalid) {
		case 0:
			inputs.topology = MONACO_PMIC_TOPOLOGY_UNKNOWN;
			expected = MONACO_PMIC_BOOT_TOPOLOGY;
			break;
		case 1:
			inputs.topology = MONACO_PMIC_TOPOLOGY_MERGED;
			expected = MONACO_PMIC_BOOT_TOPOLOGY;
			break;
		case 2:
			inputs.reset_fsm_valid = false;
			break;
		case 3:
			inputs.platform_type_valid = false;
			break;
		case 4:
			inputs.supply_policy_validated = false;
			break;
		case 5:
			inputs.regulators = NULL;
			break;
		case 6:
			inputs.regulator_count = 8;
			break;
		case 7:
			inputs.regulator_count = 10;
			break;
		case 8:
			pmic.io.transfer = NULL;
			break;
		case 9:
			pmic.io.delay_us = NULL;
			break;
		case 10:
			pmic.transfer_timeout_us = 0;
			break;
		case 11:
			pmic.state = MONACO_PMIC_RAW;
			expected = MONACO_PMIC_STATE;
			break;
		case 12:
			pmic.state = MONACO_PMIC_TRANSITION;
			expected = MONACO_PMIC_STATE;
			break;
		case 13:
			pmic.state = MONACO_PMIC_CRC_READY;
			expected = MONACO_PMIC_STATE;
			break;
		case 14:
			pmic.version.device = 0x81;
			expected = MONACO_PMIC_STATE;
			break;
		case 15:
			inputs.present_slaves = 0;
			expected = MONACO_PMIC_BOOT_TOPOLOGY;
			break;
		case 16:
			inputs.topology = (enum monaco_pmic_topology)99;
			expected = MONACO_PMIC_BOOT_TOPOLOGY;
			break;
		case 17:
			inputs.regulator_count = SIZE_MAX;
			break;
		default:
			sail_check(false);
		}
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == expected);
		sail_check(result.stage == MONACO_PMIC_BOOT_VALIDATE && result.status == expected);
		sail_check(!f.next && !result.operations_attempted);
	}
	/* Validate every rail policy before the first raw support write. */
	for (unsigned int index = 0; index < 9; ++index) {
		for (unsigned int field = 0; field < 4; ++field) {
			prepare(&f, &pmic, &inputs, policies, &result);
			switch (field) {
			case 0:
				policies[index].group = (enum monaco_pmic_regulator_group)99;
				break;
			case 1:
				policies[index].index++;
				break;
			case 2:
				policies[index].required_mode = 3;
				break;
			case 3:
				policies[index].minimum_uv = UINT32_MAX;
				break;
			default:
				sail_check(false);
			}
			sail_check(monaco_pmic_boot(&pmic, &inputs, &result) ==
				   MONACO_PMIC_INVALID);
			sail_check(!f.next && !result.operations_attempted &&
			       pmic.state == MONACO_PMIC_IDENTIFIED);
		}
	}
	static const uint32_t skip_states[] = {12, 13, 255, UINT32_MAX};

	for (unsigned int i = 0; i < ARRAY_SIZE(skip_states); ++i) {
		prepare(&f, &pmic, &inputs, policies, &result);
		inputs.reset_fsm = skip_states[i];
		sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == MONACO_PMIC_BOOT_SKIPPED);
		sail_check(result.stage == MONACO_PMIC_BOOT_SKIP_COLD);
		sail_check(result.status == MONACO_PMIC_BOOT_SKIPPED &&
			   !result.operations_attempted && !f.next);
		sail_check(pmic.state == MONACO_PMIC_IDENTIFIED);
	}
	prepare(&f, &pmic, &inputs, policies, &result);
	inputs.reset_fsm = 11;
	full_trace(&f, &inputs, false);
	sail_check(monaco_pmic_boot(&pmic, &inputs, &result) == 0);
	assert_complete(&f, &pmic, &result);
}

static struct monaco_pmic_pretarget_inputs pre_inputs(void)
{
	return (struct monaco_pmic_pretarget_inputs){
		.topology = MONACO_PMIC_TOPOLOGY_INDEPENDENT,
		.present_slaves = 7,
		.reset_fsm_valid = true,
		.platform_type_valid = true,
		.parameter_status_policy_validated = true,
	};
}

static void test_pretarget_phase(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];

	for (unsigned int policy = 0; policy < 2; policy++) {
		for (unsigned int version = 0; version < 3; version++) {
			for (unsigned int flags = 0; flags < 4; flags++) {
				prepare(&f, &pmic, &inputs, policies, &result);
				struct monaco_pmic_pretarget_inputs p = pre_inputs();

				p.runtime_bist_policy_validated = policy != 0;
				pmic.version.firmware = version == 0 ? 1 : 2;
				pmic.version.firmware_revision = version == 2 ? 1 : 0;
				bool accepted = (flags & 1U) && ((flags & 2U) || version == 0);

				pretarget(&f, 0, (uint8_t)(flags & 1U), (flags & 2U) ? 8 : 0);
				status_trace(&f, 0xff, 0xff, 0xa5);
				if (!accepted && policy) {
					runtime_trace(&f, 1, 8, true);
				}
				int expected = accepted || policy
						       ? 0
						       : MONACO_PMIC_BOOT_RUNTIME_BIST_REQUIRED;
				sail_check(monaco_pmic_pretarget(&pmic, &p,
							     (enum monaco_pmic_bist_policy)policy,
							     &result) == expected);
				sail_check(f.next == f.length &&
				       result.operations_completed == f.length);
				sail_check(result.boot_bist_accepted == accepted &&
				       result.runtime_bist_required == !accepted);
				sail_check(result.runtime_bist_attempted == (!accepted && policy));
				sail_check(result.status_residual == 0xa5 &&
				       !result.regulators_completed);
				sail_check(result.stage ==
				       (expected ? MONACO_PMIC_BOOT_BIST_REQUIRED
						 : MONACO_PMIC_BOOT_PRETARGET_COMPLETE));
				sail_check(pmic.state == MONACO_PMIC_CRC_READY);
				sail_check(pmic.io.context == &f && pmic.io.transfer == transfer &&
				       pmic.io.delay_us == delay);
				sail_check(monaco_pmic_pretarget(&pmic, &p,
							     (enum monaco_pmic_bist_policy)policy,
							     &result) == MONACO_PMIC_STATE);
				sail_check(f.next == f.length && !result.operations_attempted);
			}
		}
	}
	prepare(&f, &pmic, &inputs, policies, &result);
	pretarget(&f, 0, 1, 8);
	status_trace(&f, 0xff, 0xff, 0);
	unsigned int length = f.length;

	for (unsigned int at = 1; at <= length; at++) {
		for (unsigned int kind = 0; kind < 3; kind++) {
			prepare(&f, &pmic, &inputs, policies, &result);
			struct monaco_pmic_pretarget_inputs p = pre_inputs();

			pretarget(&f, 0, 1, 8);
			status_trace(&f, 0xff, 0xff, 0);
			f.fail_at = at;
			f.failure = kind == 0 ? -1 : kind == 1 ? INT_MIN : 1;
			int expected = kind == 2 ? MONACO_PMIC_CALLBACK : f.failure;

			sail_check(monaco_pmic_pretarget(&pmic, &p, MONACO_PMIC_REQUIRE_BOOT_BIST,
						     &result) == expected);
			sail_check(f.next == at && result.operations_completed == at - 1);
			sail_check(!result.runtime_bist_attempted && !result.regulators_completed);
			sail_check(pmic.io.context == &f && pmic.io.transfer == transfer &&
			       pmic.io.delay_us == delay);
		}
	}
	for (unsigned int bad = 0; bad < 10; bad++) {
		prepare(&f, &pmic, &inputs, policies, &result);
		struct monaco_pmic_pretarget_inputs p = pre_inputs();
		enum monaco_pmic_bist_policy mode = MONACO_PMIC_REQUIRE_BOOT_BIST;
		int expected = MONACO_PMIC_INVALID;

		switch (bad) {
		case 0:
			p.topology = MONACO_PMIC_TOPOLOGY_UNKNOWN;
			expected = MONACO_PMIC_BOOT_TOPOLOGY;
			break;
		case 1:
			p.present_slaves = 3;
			expected = MONACO_PMIC_BOOT_TOPOLOGY;
			break;
		case 2:
			p.reset_fsm_valid = false;
			break;
		case 3:
			p.platform_type_valid = false;
			break;
		case 4:
			p.parameter_status_policy_validated = false;
			break;
		case 5:
			mode = MONACO_PMIC_ALLOW_RUNTIME_BIST;
			break;
		case 6:
			mode = (enum monaco_pmic_bist_policy)99;
			break;
		case 7:
			p.reset_fsm = 12;
			expected = MONACO_PMIC_BOOT_SKIPPED;
			break;
		case 8:
			pmic.state = MONACO_PMIC_RAW;
			expected = MONACO_PMIC_STATE;
			break;
		case 9:
			pmic.version.device = 0;
			expected = MONACO_PMIC_STATE;
			break;
		default:
			sail_check(false);
		}
		sail_check(monaco_pmic_pretarget(&pmic, &p, mode, &result) == expected);
		sail_check(!f.next && !result.operations_attempted);
	}
	for (unsigned int bad = 0; bad < 6; bad++) {
		struct monaco_pmic_pretarget_inputs p = pre_inputs();

		prepare(&f, &pmic, &inputs, policies, &result);
		if (bad == 0) {
			sail_check(monaco_pmic_pretarget(NULL, &p, MONACO_PMIC_REQUIRE_BOOT_BIST,
						     &result) == MONACO_PMIC_INVALID);
		} else if (bad == 1) {
			sail_check(monaco_pmic_pretarget(&pmic, NULL, MONACO_PMIC_REQUIRE_BOOT_BIST,
						     &result) == MONACO_PMIC_INVALID);
		} else if (bad == 2) {
			sail_check(monaco_pmic_pretarget(&pmic, &p, MONACO_PMIC_REQUIRE_BOOT_BIST,
						     NULL) == MONACO_PMIC_INVALID);
		} else {
			if (bad == 3) {
				pmic.io.transfer = NULL;
			} else if (bad == 4) {
				pmic.io.delay_us = NULL;
			} else {
				pmic.transfer_timeout_us = 0;
			}
			sail_check(monaco_pmic_pretarget(&pmic, &p, MONACO_PMIC_REQUIRE_BOOT_BIST,
						     &result) == MONACO_PMIC_INVALID);
		}
		sail_check(!f.next && pmic.state == MONACO_PMIC_IDENTIFIED);
	}
	puts("PMIC pretarget policies, strict no-isolation branch and operation faults passed");
}

static void test_tail_phase(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_boot_result result;
	struct monaco_pmic_regulator_policy policies[9];
	unsigned int first_tail, length;

	for (unsigned int runtime = 0; runtime < 2; runtime++) {
		struct monaco_pmic_pretarget_inputs p = pre_inputs();
		struct monaco_pmic_tail_inputs tail = {true, policies, 9};

		prepare(&f, &pmic, &inputs, policies, &result);
		p.platform_type = inputs.platform_type;
		p.runtime_bist_policy_validated = runtime != 0;
		full_trace(&f, &inputs, runtime != 0);
		f.length--; /* The phase excludes only the caller-owned final settle. */
		sail_check(monaco_pmic_pretarget(&pmic, &p,
					     runtime ? MONACO_PMIC_ALLOW_RUNTIME_BIST
						     : MONACO_PMIC_REQUIRE_BOOT_BIST,
					     &result) == 0);
		sail_check(monaco_pmic_boot_tail(&pmic, &tail, &result) == 0);
		sail_check(f.next == f.length && result.operations_completed == f.length);
		sail_check(result.regulators_completed == 9 &&
			   result.posttarget_writes_completed == 6);
		sail_check(result.stage == MONACO_PMIC_BOOT_TAIL_COMPLETE);
		sail_check(result.delay_us_completed == (runtime ? 17800U : 7000U));
		sail_check(result.regulators[5].enable_skipped &&
		       !result.regulators[5].enable_sequence_complete);
		sail_check(pmic.io.context == &f && pmic.io.transfer == transfer &&
		       pmic.io.delay_us == delay);
		sail_check(monaco_pmic_boot_tail(&pmic, &tail, &result) == MONACO_PMIC_STATE);
		sail_check(f.next == f.length);
	}
	prepare(&f, &pmic, &inputs, policies, &result);
	pretarget(&f, inputs.platform_type, 1, 8);
	status_trace(&f, 0xff, 0xff, 0x24);
	first_tail = f.length;
	regulator_trace(&f, policies);
	posttarget(&f);
	length = f.length - 1;
	for (unsigned int at = first_tail + 1; at <= length; at++) {
		for (unsigned int kind = 0; kind < 4; kind++) {
			struct monaco_pmic_pretarget_inputs p = pre_inputs();
			struct monaco_pmic_tail_inputs tail = {true, policies, 9};

			prepare(&f, &pmic, &inputs, policies, &result);
			p.platform_type = inputs.platform_type;
			full_trace(&f, &inputs, false);
			f.length--;
			sail_check(monaco_pmic_pretarget(&pmic, &p, MONACO_PMIC_REQUIRE_BOOT_BIST,
						     &result) == 0);
			if (kind == 3) {
				if (f.expected[at - 1].kind != 'R') {
					continue;
				}
				f.corrupt_at = at;
			} else {
				f.fail_at = at;
				f.failure = kind == 0 ? -1 : kind == 1 ? INT_MIN : 1;
			}
			int expected = kind == 3   ? MONACO_PMIC_CRC
				       : kind == 2 ? MONACO_PMIC_CALLBACK
						   : f.failure;
			sail_check(monaco_pmic_boot_tail(&pmic, &tail, &result) == expected);
			sail_check(f.next == at &&
			       result.operations_completed == at - (kind == 3 ? 0U : 1U));
			sail_check(pmic.state == MONACO_PMIC_CRC_READY && pmic.io.context == &f);
			sail_check(monaco_pmic_boot_tail(&pmic, &tail, &result) ==
				   MONACO_PMIC_STATE);
			sail_check(f.next == at);
		}
	}
	for (unsigned int bad = 0; bad < 18; bad++) {
		struct monaco_pmic_pretarget_inputs p = pre_inputs();
		struct monaco_pmic_tail_inputs tail = {true, policies, 9};
		int expected = MONACO_PMIC_INVALID;

		prepare(&f, &pmic, &inputs, policies, &result);
		p.platform_type = inputs.platform_type;
		pretarget(&f, inputs.platform_type, 1, 8);
		status_trace(&f, 0, 0, 0);
		sail_check(monaco_pmic_pretarget(&pmic, &p, MONACO_PMIC_REQUIRE_BOOT_BIST,
						 &result) == 0);
		unsigned int before = f.next;

		switch (bad) {
		case 0:
			tail.regulator_posttarget_policy_validated = false;
			break;
		case 1:
			tail.regulators = NULL;
			break;
		case 2:
			tail.regulator_count = 8;
			break;
		case 3:
			policies[8].index = 5;
			break;
		case 4:
			policies[8].group = MONACO_PMIC_LDO;
			break;
		case 5:
			policies[8].minimum_uv = UINT32_MAX;
			break;
		case 6:
			policies[8].required_mode = 1;
			break;
		case 7:
			pmic.io.transfer = NULL;
			break;
		case 8:
			pmic.io.delay_us = NULL;
			break;
		case 9:
			pmic.transfer_timeout_us = 0;
			break;
		case 10:
			pmic.state = MONACO_PMIC_IDENTIFIED;
			expected = MONACO_PMIC_STATE;
			break;
		case 11:
			pmic.version.device = 0;
			expected = MONACO_PMIC_STATE;
			break;
		case 12:
			result.stage = MONACO_PMIC_BOOT_BIST_REQUIRED;
			expected = MONACO_PMIC_STATE;
			break;
		case 13:
			result.status = -1;
			expected = MONACO_PMIC_STATE;
			break;
		case 14:
			result.boot_bist_flags_valid = false;
			expected = MONACO_PMIC_STATE;
			break;
		case 15:
			result.boot_bist_accepted = false;
			expected = MONACO_PMIC_STATE;
			break;
		case 16:
			result.regulators_completed = 1;
			expected = MONACO_PMIC_STATE;
			break;
		case 17:
			result.posttarget_writes_completed = 1;
			expected = MONACO_PMIC_STATE;
			break;
		default:
			sail_check(false);
		}
		sail_check(monaco_pmic_boot_tail(&pmic, &tail, &result) == expected &&
		       f.next == before);
	}
	sail_check(monaco_pmic_boot_tail(NULL, NULL, NULL) == MONACO_PMIC_INVALID);
	puts("PMIC carried-context tail, caller-owned settle, all tail faults and phase guards "
	     "passed");
}

struct finish_clock {
	struct fixture *fixture;
	unsigned int reads, fail_at;
	int failure;
	bool override;
	uint64_t values[2];
};

static int finish_time(void *context, uint64_t *value)
{
	struct finish_clock *clock = context;
	struct fixture *f = clock->fixture;

	sail_check(clock->reads < 2 && f->result->stage == MONACO_PMIC_BOOT_SETTLE);
	sail_check(f->next == f->length - (clock->reads == 0 ? 1U : 0U));
	*value = clock->override ? clock->values[clock->reads] : f->elapsed_us;
	clock->reads++;
	return clock->reads == clock->fail_at ? clock->failure : 0;
}

static void prepare_finished_tail(struct fixture *f, struct monaco_pmic *pmic,
				  struct monaco_pmic_boot_result *result, bool runtime)
{
	struct monaco_pmic_boot_inputs inputs;
	struct monaco_pmic_regulator_policy policies[9];
	struct monaco_pmic_pretarget_inputs pre = pre_inputs();
	struct monaco_pmic_tail_inputs tail = {true, policies, 9};

	prepare(f, pmic, &inputs, policies, result);
	pre.platform_type = inputs.platform_type;
	pre.runtime_bist_policy_validated = runtime;
	full_trace(f, &inputs, runtime);
	sail_check(monaco_pmic_pretarget(pmic, &pre,
				     runtime ? MONACO_PMIC_ALLOW_RUNTIME_BIST
					     : MONACO_PMIC_REQUIRE_BOOT_BIST,
				     result) == 0);
	sail_check(monaco_pmic_boot_tail(pmic, &tail, result) == 0);
	sail_check(f->next == f->length - 1 && result->stage == MONACO_PMIC_BOOT_TAIL_COMPLETE);
	sail_check(result->tail_owner == pmic && !result->settle_time_reads_attempted);
}

static void test_finish_success(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_result result;

	for (unsigned int runtime = 0; runtime < 2; ++runtime) {
		struct finish_clock clock = {.fixture = &f};
		const struct monaco_pmic_boot_time time = {&clock, finish_time};

		prepare_finished_tail(&f, &pmic, &result, runtime != 0);
		sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) == 0);
		assert_complete(&f, &pmic, &result);
		sail_check(clock.reads == 2 && result.settle_time_reads_attempted == 2);
		sail_check(result.settle_time_reads_completed == 2 && result.settle_elapsed_valid);
		sail_check(result.settle_elapsed_us == 3000);
		sail_check(result.delay_us_completed == (runtime ? 20800U : 10000U));
		sail_check(result.regulators[5].enable_skipped);
		struct monaco_pmic_boot_result saved = result;

		sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) == MONACO_PMIC_STATE);
		sail_check(!memcmp(&result, &saved, sizeof(result)) && clock.reads == 2);
		sail_check(f.next == f.length);
	}
	/* A delayed successful return is permitted; only a shorter wait is invalid. */
	struct finish_clock clock = {
		.fixture = &f, .override = true, .values = {UINT64_MAX - 4000U, UINT64_MAX}};
	const struct monaco_pmic_boot_time time = {&clock, finish_time};

	prepare_finished_tail(&f, &pmic, &result, false);
	sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) == 0);
	sail_check(result.settle_elapsed_us == 4000);
}

static void test_finish_failure(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_result result;
	const int errors[] = {IO_ERROR, INT_MIN, 1};

	for (unsigned int phase = 0; phase < 3; ++phase) {
		for (unsigned int kind = 0; kind < 3; ++kind) {
			struct finish_clock clock = {.fixture = &f, .failure = errors[kind]};
			const struct monaco_pmic_boot_time time = {&clock, finish_time};
			int expected = errors[kind] > 0 ? MONACO_PMIC_CALLBACK : errors[kind];

			prepare_finished_tail(&f, &pmic, &result, false);
			unsigned int before = f.next;
			uint64_t elapsed = f.elapsed_us;

			if (phase == 1) {
				f.fail_at = f.length;
				f.failure = errors[kind];
			} else {
				clock.fail_at = phase == 0 ? 1U : 2U;
			}
			sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) == expected);
			sail_check(result.stage == MONACO_PMIC_BOOT_SETTLE &&
			       result.status == expected);
			sail_check(result.last_callback_status == errors[kind]);
			sail_check(clock.reads == (phase == 2 ? 2U : 1U));
			sail_check(f.next == before + (phase == 0 ? 0U : 1U));
			sail_check(f.elapsed_us == elapsed + (phase == 0 ? 0U : 3000U));
			sail_check(result.operations_completed == before + (phase == 2 ? 1U : 0U));
			sail_check(!result.settle_elapsed_valid &&
				   pmic.state == MONACO_PMIC_CRC_READY);
			struct monaco_pmic_boot_result saved = result;
			unsigned int calls = f.next, reads = clock.reads;

			sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) ==
				   MONACO_PMIC_STATE);
			sail_check(f.next == calls && clock.reads == reads);
			sail_check(!memcmp(&result, &saved, sizeof(result)));
		}
	}
	const uint64_t times[][2] = {
		{1000, 1000},
		{1000, 3999},
		{1000, 999},
		{UINT64_MAX - 1000U, 2000},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(times); ++i) {
		struct finish_clock clock = {
			.fixture = &f, .override = true, .values = {times[i][0], times[i][1]}};
		const struct monaco_pmic_boot_time time = {&clock, finish_time};

		prepare_finished_tail(&f, &pmic, &result, false);
		sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) ==
			   MONACO_PMIC_BOOT_TIMEBASE);
		sail_check(result.stage == MONACO_PMIC_BOOT_SETTLE && f.next == f.length);
		sail_check(result.settle_elapsed_valid == (times[i][1] >= times[i][0]));
		sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) == MONACO_PMIC_STATE);
		sail_check(clock.reads == 2 && f.next == f.length);
	}
}

static void test_finish_guards(void)
{
	struct fixture f;
	struct monaco_pmic pmic, other;
	struct monaco_pmic_boot_result result;

	sail_check(monaco_pmic_boot_finish(NULL, NULL, NULL) == MONACO_PMIC_INVALID);
	for (unsigned int bad = 0; bad < 33 + 4 * 9; ++bad) {
		struct finish_clock clock = {.fixture = &f};
		struct monaco_pmic_boot_time time = {&clock, finish_time};
		struct monaco_pmic *argument = &pmic;
		const struct monaco_pmic_boot_time *time_argument = &time;
		int expected = MONACO_PMIC_STATE;

		prepare_finished_tail(&f, &pmic, &result, false);
		unsigned int before = f.next;

		other = pmic;
		switch (bad) {
		case 0:
			argument = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 1:
			time_argument = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 2:
			time.time_us = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 3:
			pmic.io.transfer = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 4:
			pmic.io.delay_us = NULL;
			expected = MONACO_PMIC_INVALID;
			break;
		case 5:
			pmic.transfer_timeout_us = 0;
			expected = MONACO_PMIC_INVALID;
			break;
		case 6:
			argument = &other;
			break;
		case 7:
			result.tail_owner = NULL;
			break;
		case 8:
			pmic.io.context = &other;
			break;
		case 9:
			pmic.transfer_timeout_us++;
			break;
		case 10:
			pmic.state = MONACO_PMIC_IDENTIFIED;
			break;
		case 11:
			result.tail_context.state = MONACO_PMIC_IDENTIFIED;
			break;
		case 12:
			pmic.version.device ^= 1U;
			break;
		case 13:
			pmic.version.revision ^= 1U;
			break;
		case 14:
			pmic.version.firmware ^= 1U;
			break;
		case 15:
			pmic.version.firmware_revision ^= 1U;
			break;
		case 16:
			result.stage = MONACO_PMIC_BOOT_PRETARGET_COMPLETE;
			break;
		case 17:
			result.status = IO_ERROR;
			break;
		case 18:
			result.boot_bist_flags_valid = false;
			break;
		case 19:
			result.boot_bist_accepted = false;
			break;
		case 20:
			result.regulators_completed--;
			break;
		case 21:
			result.posttarget_writes_completed--;
			break;
		case 22:
			result.operations_completed--;
			break;
		case 23:
			result.last_callback_status = IO_ERROR;
			break;
		case 24:
			result.settle_time_reads_attempted = 1;
			break;
		case 25:
			result.tail_context.io.transfer = NULL;
			break;
		case 26:
			result.tail_context.io.delay_us = NULL;
			break;
		case 27:
			result.boot_bist_accepted = false;
			result.runtime_bist_passed = true;
			break;
		case 28:
			result.boot_bist_accepted = false;
			result.runtime_bist_cleanup_complete = true;
			break;
		case 29:
			result.tail_result = NULL;
			break;
		case 30:
			result.settle_time_reads_completed = 1;
			break;
		case 31:
			result.settle_elapsed_valid = true;
			break;
		case 32:
			result.settle_elapsed_us = 3000;
			break;
		default: {
			unsigned int index = (bad - 33) / 4;
			struct monaco_pmic_regulator_result *rail = &result.regulators[index];

			switch ((bad - 33) % 4) {
			case 0:
				rail->stage = MONACO_PMIC_REGULATOR_ENABLE;
				break;
			case 1:
				rail->voltage_valid = false;
				break;
			case 2:
				rail->mode_valid = false;
				break;
			case 3:
				rail->enable_valid = false;
				break;
			}
			break;
		}
		}
		sail_check(monaco_pmic_boot_finish(argument, time_argument, &result) == expected);
		sail_check(f.next == before && !clock.reads &&
			   pmic.state != MONACO_PMIC_TRANSITION);
	}
	struct finish_clock clock = {.fixture = &f};
	const struct monaco_pmic_boot_time time = {&clock, finish_time};

	prepare_finished_tail(&f, &pmic, &result, false);
	struct monaco_pmic_boot_result stale_copy = result;

	sail_check(monaco_pmic_boot_finish(&pmic, &time, &stale_copy) == MONACO_PMIC_STATE);
	sail_check(!clock.reads && f.next == f.length - 1);
	sail_check(monaco_pmic_boot_finish(&pmic, &time, &result) == 0);
	puts("PMIC measured final settle: complete traces, 69 guards, stale copy, "
	     "callback/timebase faults and no reentry passed");
}

int sail_model_test(void)
{
	test_complete_traces();
	test_platform_and_status_branches();
	test_bist_acceptance();
	test_every_operation_failure();
	test_regulator_failure_boundaries();
	test_input_guards();
	test_pretarget_phase();
	test_tail_phase();
	test_finish_success();
	test_finish_failure();
	test_finish_guards();
	puts("PMIC cold-boot coordinator tests passed");
	return 0;
}
