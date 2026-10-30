/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hbcu.h"

#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define CONTROL       UINT32_C(0xf8800004)
#define MBIST         UINT32_C(0xf8800068)
#define LBIST         UINT32_C(0xf8800074)
#define MBIST_OUT     UINT32_C(0xf0305024)
#define LBIST_OUT     UINT32_C(0xf0305030)

struct event {
	uint32_t address, value;
	bool write;
};
struct fake {
	uint32_t control, words[6], published[6];
	struct event trace[14];
	unsigned int count, fail_at;
	bool fail_after_effect;
};

static bool expected_write(uint32_t address)
{
	static const uint32_t allowed[] = {
		0xf8800004, 0xf0305024, 0xf0305028, 0xf030502c, 0xf0305030, 0xf0305034, 0xf0305038,
	};
	for (size_t i = 0; i < ARRAY_SIZE(allowed); i++) {
		if (address == allowed[i]) {
			return true;
		}
	}
	return false;
}

static bool record(struct fake *f, uint32_t address, uint32_t value, bool write)
{
	sail_check(f->count < ARRAY_SIZE(f->trace));
	f->trace[f->count++] = (struct event){address, value, write};
	return f->count == f->fail_at;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	uint32_t observed;

	if (address == CONTROL) {
		observed = f->control;
	} else {
		sail_check(address >= MBIST && address <= LBIST + 8 && !(address & 3U));
		observed = f->words[(address - MBIST) / 4];
	}
	if (record(f, address, observed, false)) {
		/* A failing backend may still have written its output argument. */
		*value = UINT32_C(0xdeaddead);
		return 73;
	}
	*value = observed;
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	bool failed;

	sail_check(expected_write(address));
	failed = record(f, address, value, true);
	if (failed && !f->fail_after_effect) {
		return -91;
	}
	if (address == CONTROL) {
		f->control = value;
	} else {
		f->published[(address - MBIST_OUT) / 4] = value;
	}
	return failed ? -91 : 0;
}

static void prepare(struct fake *f)
{
	*f = (struct fake){
		.control = 0xa5a55a5b,
		.words = {0x11111111, 0x22222222, 0x80000000, 0x33333333, 0x44444444, 0x87654321},
		.published = {0xdead, 0xdead, 0xdead, 0xdead, 0xdead, 0xdead},
	};
}

static int run(struct fake *f, uint8_t injected, struct monaco_hbcu_result *r)
{
	const struct monaco_timer_io io = {f, read32, write32};
	const struct monaco_hbcu_policy p = {true, injected};

	return monaco_hbcu_capture(&io, &p, r);
}

static void assert_event(const struct event *actual, const struct event *expected)
{
	sail_check(actual->address == expected->address);
	sail_check(actual->value == expected->value);
	sail_check(actual->write == expected->write);
}

static void golden_sequence(void)
{
	/* Explicit independent order, including both input reads before stores. */
	const struct event expected[] = {
		{0xf8800004, 0xa5a55a5b, false}, {0xf8800004, 0xa5a55a5a, true},
		{0xf8800068, 0x11111111, false}, {0xf880006c, 0x22222222, false},
		{0xf0305024, 0x11111111, true},  {0xf0305028, 0x22222222, true},
		{0xf8800070, 0x80000000, false}, {0xf030502c, 0x80000000, true},
		{0xf8800074, 0x33333333, false}, {0xf8800078, 0x44444444, false},
		{0xf0305030, 0x33333333, true},  {0xf0305034, 0x44444444, true},
		{0xf880007c, 0x87654321, false}, {0xf0305038, 0x87654321, true},
	};
	struct fake f;
	struct monaco_hbcu_result r;

	prepare(&f);
	sail_check(run(&f, 0, &r) == 0);
	sail_check(f.count == ARRAY_SIZE(expected));
	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		assert_event(&f.trace[i], &expected[i]);
	}
	sail_check(f.control == 0xa5a55a5a);
	sail_check(r.completed_reads == 7 && r.completed_writes == 7);
	sail_check(r.disabled && r.phase == MONACO_HBCU_COMPLETE);
	sail_check(r.mbist.outcome == MONACO_HBCU_PASSED);
	sail_check(r.lbist.outcome == MONACO_HBCU_PASSED);
	sail_check(r.mbist.captured == 7 && r.lbist.captured == 7);
	sail_check(r.mbist.published == 7 && r.lbist.published == 7);
	sail_check(memcmp(r.mbist.words, f.words, sizeof(r.mbist.words)) == 0);
	sail_check(memcmp(r.lbist.words, f.words + 3, sizeof(r.lbist.words)) == 0);
	sail_check(r.callback_status == 0 && r.status == 0);
}

static void outcome_matrix(void)
{
	unsigned int cases = 0;

	for (unsigned int m = 0; m < 4; m++) {
		for (unsigned int l = 0; l < 4; l++) {
			for (unsigned int raw = 0; raw < 4; raw++) {
				for (uint8_t injection = 0; injection < 4; injection++) {
					struct fake f;
					struct monaco_hbcu_result r;
					bool mskip = m != 3, lskip = l != 3;
					bool mfailed = !mskip && (!(raw & 1U) || (injection & 1U));
					bool lfailed = !lskip && (!(raw & 2U) || (injection & 2U));
					int expected = mfailed   ? MONACO_HBCU_MBIST_FAILED
						       : lfailed ? MONACO_HBCU_LBIST_FAILED
								 : 0;
					prepare(&f);
					if (!(m & 1U)) {
						f.words[0] = 0;
					}
					if (!(m & 2U)) {
						f.words[1] = 0;
					}
					if (!(l & 1U)) {
						f.words[3] = 0;
					}
					if (!(l & 2U)) {
						f.words[4] = 0;
					}
					if (!(raw & 1U)) {
						f.words[2] = 0;
					}
					if (!(raw & 2U)) {
						f.words[5] = 0;
					}
					sail_check(run(&f, injection, &r) == expected);
					sail_check(r.status == expected && r.disabled);
					sail_check(r.mbist.outcome ==
						   (mskip     ? MONACO_HBCU_SKIPPED
						    : mfailed ? MONACO_HBCU_FAILED
							      : MONACO_HBCU_PASSED));
					sail_check(r.mbist.captured == (mskip ? 3U : 7U));
					sail_check(r.mbist.published == 7);
					sail_check(f.published[0] == f.words[0] &&
					       f.published[1] == f.words[1]);
					sail_check(f.published[2] == (mskip ? 0 : f.words[2]));
					if (mfailed) {
						sail_check(r.phase == MONACO_HBCU_MBIST &&
						       f.count == 8);
						sail_check(r.lbist.outcome ==
							   MONACO_HBCU_UNOBSERVED);
						sail_check(r.lbist.captured == 0 &&
						       r.lbist.published == 0);
						sail_check(f.published[3] == 0xdead &&
						       f.published[5] == 0xdead);
					} else {
						sail_check(r.lbist.outcome ==
						       (lskip     ? MONACO_HBCU_SKIPPED
							: lfailed ? MONACO_HBCU_FAILED
								  : MONACO_HBCU_PASSED));
						sail_check(r.lbist.captured == (lskip ? 3U : 7U));
						sail_check(r.lbist.published == 7);
						sail_check(f.published[3] == f.words[3] &&
						       f.published[4] == f.words[4]);
						sail_check(f.published[5] ==
							   (lskip ? 0 : f.words[5]));
						sail_check(f.count == 14U - (unsigned int)mskip -
									  (unsigned int)lskip);
						sail_check(r.phase == (lfailed ? MONACO_HBCU_LBIST
									   : MONACO_HBCU_COMPLETE));
					}
					cases++;
				}
			}
		}
	}
	sail_check(cases == 256);
	printf("HBCU outcome combinations: %u\n", cases);
}

static void fault_case(bool skip, uint8_t injection, unsigned int *tested)
{
	struct fake baseline;
	struct monaco_hbcu_result baseline_result;

	prepare(&baseline);
	if (skip) {
		baseline.words[0] = baseline.words[4] = 0;
	}
	(void)run(&baseline, injection, &baseline_result);
	sail_check(baseline.count >= 8 && baseline.count <= 14);
	for (unsigned int after = 0; after < 2; after++) {
		for (unsigned int failure = 1; failure <= baseline.count; failure++) {
			struct fake f;
			struct monaco_hbcu_result r;
			unsigned int reads = 0, writes = 0;
			uint8_t captured[2] = {0}, published[2] = {0};

			prepare(&f);
			if (skip) {
				f.words[0] = f.words[4] = 0;
			}
			f.fail_at = failure;
			f.fail_after_effect = after != 0;
			sail_check(run(&f, injection, &r) == MONACO_HBCU_IO);
			sail_check(f.count == failure && r.status == MONACO_HBCU_IO);
			for (unsigned int n = 0; n < failure; n++) {
				const struct event *event = &baseline.trace[n];

				assert_event(&f.trace[n], event);
				if (n == failure - 1) {
					break;
				}
				if (event->write) {
					writes++;
					if (event->address != CONTROL) {
						unsigned int index =
							(event->address - MBIST_OUT) / 4;
						published[index / 3] |=
							(uint8_t)(1U << (index % 3));
					}
				} else {
					reads++;
					if (event->address != CONTROL) {
						unsigned int index = (event->address - MBIST) / 4;

						captured[index / 3] |= (uint8_t)(1U << (index % 3));
					}
				}
			}
			const struct event *failed = &baseline.trace[failure - 1];

			sail_check(r.address == failed->address);
			sail_check(r.last_operation ==
			       (failed->write ? MONACO_HBCU_WRITE : MONACO_HBCU_READ));
			sail_check(r.callback_status == (failed->write ? -91 : 73));
			sail_check(r.completed_reads == reads && r.completed_writes == writes);
			sail_check(r.mbist.captured == captured[0] &&
				   r.lbist.captured == captured[1]);
			sail_check(r.mbist.published == published[0] &&
			       r.lbist.published == published[1]);
			sail_check(r.disabled == (failure > 2));
			sail_check(r.phase == (failure <= 2                 ? MONACO_HBCU_DISABLE
					   : failed->address == CONTROL ? MONACO_HBCU_DISABLE
					   : ((failed->write && failed->address < LBIST_OUT) ||
					      (!failed->write && failed->address < LBIST))
						   ? MONACO_HBCU_MBIST
						   : MONACO_HBCU_LBIST));
			(*tested)++;
		}
	}
}

static void invalid_arguments(void)
{
	struct fake f;
	struct monaco_hbcu_result r;
	struct monaco_timer_io io = {&f, read32, write32};
	struct monaco_hbcu_policy p = {true, 0};

	prepare(&f);
	sail_check(monaco_hbcu_capture(&io, &p, NULL) == MONACO_HBCU_INVALID);
	sail_check(monaco_hbcu_capture(NULL, &p, &r) == MONACO_HBCU_INVALID);
	sail_check(monaco_hbcu_capture(&io, NULL, &r) == MONACO_HBCU_INVALID);
	io.read32 = NULL;
	sail_check(monaco_hbcu_capture(&io, &p, &r) == MONACO_HBCU_INVALID);
	io.read32 = read32;
	io.write32 = NULL;
	sail_check(monaco_hbcu_capture(&io, &p, &r) == MONACO_HBCU_INVALID);
	io.write32 = write32;
	p.valid = false;
	memset(&r, 0xff, sizeof(r));
	sail_check(monaco_hbcu_capture(&io, &p, &r) == MONACO_HBCU_POLICY);
	sail_check(!r.disabled && r.completed_writes == 0 && r.completed_reads == 0);
	sail_check(r.phase == MONACO_HBCU_INPUT && r.last_operation == MONACO_HBCU_NO_OPERATION);
	sail_check(r.mbist.captured == 0 && r.lbist.published == 0);
	p.valid = true;
	for (unsigned int mask = 4; mask <= UINT8_MAX; mask++) {
		p.injected_failures = (uint8_t)mask;
		sail_check(monaco_hbcu_capture(&io, &p, &r) == MONACO_HBCU_INVALID);
	}
	sail_check(f.count == 0);
}

static void write_boundary(void)
{
	const uint32_t forbidden[] = {
		0xf0305084, 0xf0305088, 0xf030508c, 0xf0305090, 0xf100000c, 0xf0300200, 0xf1000520,
		0xf1000524, 0xc1ffc000, 0xf0100000, 0xf0101000, 0xf0305094, 0xf0305098, 0xf030509c,
		0xf03050a0, 0xf140c000, 0x080ad1c0, 0xf8800068, 0xf8800070, 0xf880007c,
	};
	for (uint32_t offset = 0; offset < 0x100; offset++) {
		uint32_t a = 0xf0305000U + offset;
		uint32_t b = 0xf8800000U + offset;

		sail_check(monaco_hbcu_write_allowed(a) == expected_write(a));
		sail_check(monaco_hbcu_write_allowed(b) == expected_write(b));
	}
	for (size_t i = 0; i < ARRAY_SIZE(forbidden); i++) {
		sail_check(!monaco_hbcu_write_allowed(forbidden[i]));
	}
	sail_check(!monaco_hbcu_write_allowed(0));
	sail_check(!monaco_hbcu_write_allowed(UINT32_MAX));
}

int sail_model_test(void)
{
	unsigned int faults = 0;

	golden_sequence();
	outcome_matrix();
	fault_case(false, 0, &faults);
	fault_case(true, 3, &faults);
	fault_case(false, 1, &faults);
	fault_case(false, 2, &faults);
	invalid_arguments();
	write_boundary();
	sail_check(faults == 96);
	printf("HBCU callback failure positions: %u\nHBCU tests passed\n", faults);
	return 0;
}
