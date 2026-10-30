/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cpu_pll.h"

#include "sail_check.h"
#include <stdio.h>
#include <zephyr/sys/util.h>

#define PLL0          UINT32_C(0xf1100000)
#define PLL1          UINT32_C(0xf1101000)
#define PLL2          UINT32_C(0xf1102000)
#define PLL4          UINT32_C(0xf1104000)
#define LOCK          UINT32_C(0x80000000)
static const unsigned int indices[] = {0, 1, 2, 4};
static const unsigned int family_indices[] = {0, 2, 4};
static const uint32_t expected_l[] = {31, 83, 34, 0, 26};
static const uint32_t expected_fraction[] = {0x4000, 0, 0x9555, 0, 0x0aaa};

struct reg {
	uint32_t address, value;
};
struct event {
	char operation;
	uint32_t address;
};
struct fake {
	struct reg regs[48], writes[64];
	struct event events[128];
	unsigned int event_count;
	unsigned int reg_count, write_count, accesses, fail_at, high_reads, low_reads;
	unsigned int polls[5], lock_after[5];
	uint64_t ticks, step;
	bool enabling[5], timer_failure, inject_mode_flag, refresh, ignore_l;
};

static uint32_t *reg(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->reg_count; i++) {
		if (f->regs[i].address == address) {
			return &f->regs[i].value;
		}
	}
	sail_check(f->reg_count < ARRAY_SIZE(f->regs));
	f->regs[f->reg_count].address = address;
	return &f->regs[f->reg_count++].value;
}

static void record(struct fake *f, char operation, uint32_t address)
{
	if (f->event_count < ARRAY_SIZE(f->events)) {
		f->events[f->event_count++] = (struct event){operation, address};
	}
}

static bool fail(struct fake *f, uint32_t address)
{
	if (++f->accesses != f->fail_at) {
		return false;
	}
	f->timer_failure = address >= 0xf1400000 && address <= 0xf1400010;
	return true;
}

static int fake_read(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;

	record(f, 'r', address);
	if (fail(f, address)) {
		return -1;
	}
	if (address == 0xf1400010) {
		*value = 19200000;
		return 0;
	}
	if (address == 0xf1400000) {
		f->low_reads++;
		*value = (uint32_t)f->ticks;
		return 0;
	}
	if (address == 0xf1400004) {
		*value = (uint32_t)(f->ticks >> 32);
		if (++f->high_reads % 2 == 0) {
			f->ticks += f->step;
		}
		return 0;
	}
	sail_check(monaco_cpu_pll_write_allowed(address));
	uint32_t *r = reg(f, address);

	if (address == PLL0 || address == PLL1 || address == PLL2 || address == PLL4) {
		unsigned int index = (address - PLL0) / 0x1000U;

		if (f->enabling[index] && !(*r & LOCK)) {
			if (index == 1 && f->inject_mode_flag) {
				*r |= 0x4000;
			}
			if (++f->polls[index] >= f->lock_after[index]) {
				*r |= LOCK;
			}
		}
	}
	*value = *r;
	return 0;
}

static int fake_write(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;

	record(f, 'w', address);
	sail_check(monaco_cpu_pll_write_allowed(address));
	if (fail(f, address)) {
		return -1;
	}
	sail_check(f->write_count < ARRAY_SIZE(f->writes));
	f->writes[f->write_count++] = (struct reg){address, value};
	if (address == PLL1 + 4 && f->ignore_l) {
		return 0;
	}
	uint32_t *r = reg(f, address);

	if (address == PLL0 || address == PLL1 || address == PLL2 || address == PLL4) {
		/* Lock is hardware-owned even when the saved MODE write contains zero. */
		uint32_t locked = *r & LOCK;

		if (value & 1U) {
			sail_check(locked);
		}
		*r = (value & ~LOCK) | locked;
	} else {
		*r = value;
	}
	if ((address == PLL0 || address == PLL2 || address == PLL4) && (value & 4U)) {
		unsigned int index = (address - PLL0) / 0x1000U;

		sail_check(*reg(f, address + 4) == 1);
		sail_check((*reg(f, address + 0x10) & 0xffffU) == expected_l[index]);
		sail_check((*reg(f, address + 0x14) & 0xffffU) == expected_fraction[index]);
		sail_check((*reg(f, address + 0x18) & 0x11c3fc01U) == (index ? 1U : 0x401U));
		f->enabling[index] = true;
	}
	if (address == PLL1 + 0x2c && value == 1) {
		sail_check((*reg(f, PLL1) & 6U) == 6);
		sail_check((*reg(f, PLL1 + 4) & 0xffU) == 0x53);
		sail_check((*reg(f, PLL1 + 8) & 0xffffU) == 0);
		sail_check((*reg(f, PLL1 + 0x10) & 0x12023000U) == 0x02000000);
		f->enabling[1] = true;
	}
	if (address == PLL1 + 0x10 && f->refresh) {
		*reg(f, PLL1) &= ~LOCK;
		f->polls[1] = 0;
	}
	return 0;
}

static void reset_trace(struct fake *f)
{
	f->accesses = f->write_count = f->low_reads = f->fail_at = 0;
	for (unsigned int i = 0; i < ARRAY_SIZE(f->polls); i++) {
		f->polls[i] = 0;
	}
	f->event_count = 0;
	f->timer_failure = false;
}

static struct monaco_clock prepare(struct fake *f, struct monaco_timer *timer)
{
	struct monaco_timer_io io = {f, fake_read, fake_write};
	*f = (struct fake){.step = 20, .lock_after = {1, 1, 1, 1, 1}};
	sail_check(monaco_timer_init(timer, &io, 4) == 0);
	reset_trace(f);
	return (struct monaco_clock){io, timer, 1100};
}

static int enable(struct monaco_clock *c, unsigned int index)
{
	switch (index) {
	case 0:
		return monaco_cpu_pll0_enable(c);
	case 1:
		return monaco_cpu_pll1_enable(c);
	case 2:
		return monaco_cpu_pll2_enable(c);
	case 4:
		return monaco_cpu_pll4_enable(c);
	default:
		sail_check(false);
		return MONACO_CLOCK_INVALID;
	}
}

static unsigned int collect(struct fake *f, uint32_t address, uint32_t *values)
{
	unsigned int count = 0;

	for (unsigned int i = 0; i < f->write_count; i++) {
		if (f->writes[i].address == address) {
			values[count++] = f->writes[i].value;
		}
	}
	return count;
}

static void off_sequences(void)
{
	struct fake f;
	struct monaco_timer timer;
	uint32_t values[64];

	for (unsigned int profile = 0; profile < ARRAY_SIZE(family_indices); profile++) {
		unsigned int index = family_indices[profile];
		uint32_t base = PLL0 + index * 0x1000U;
		uint32_t l = expected_l[index];

		for (unsigned int calibration = 0; calibration < 2; calibration++) {
			struct monaco_clock c = prepare(&f, &timer);
			*reg(&f, base) = 0x2000U | (calibration ? 0x100U : 0);
			*reg(&f, base + 0x10) = 0xabcd9999;
			*reg(&f, base + 0x14) = 0xfaceeeee;
			sail_check(enable(&c, index) == 0);
			const struct reg initial[] = {
				{base + 0x20, 0x20485699}, {base + 0x24, 0x00182261},
				{base + 0x28, 0x32aa299c}, {base + 0x2c, 0},
				{base + 0x30, 0},          {base + 0x34, 0},
				{base + 0x18, 0},          {base + 0x1c, 0x00400805},
			};
			for (unsigned int i = 0; i < ARRAY_SIZE(initial); i++) {
				sail_check(f.writes[i].address == initial[i].address);
				sail_check(f.writes[i].value == initial[i].value);
			}
			sail_check(collect(&f, base + 0x10, values) == 1 + calibration);
			sail_check(values[0] == (0xabcd0000U | l));
			if (calibration) {
				sail_check(values[1] == (0x00440000U | l));
			}
			sail_check(*reg(&f, base + 0x14) == expected_fraction[index]);
			sail_check(*reg(&f, base + 0x18) == (index ? 1U : 0x401U));
			sail_check((*reg(&f, base) & 0x2000U) != 0);
			sail_check(f.low_reads == 0);
		}
	}
	struct monaco_clock c = prepare(&f, &timer);
	*reg(&f, PLL1) = 0x2000;
	*reg(&f, PLL1 + 4) = 0xdeadbeff;
	*reg(&f, PLL1 + 8) = 0xfaceffff;
	f.inject_mode_flag = true;
	sail_check(monaco_cpu_pll1_enable(&c) == 0);
	const struct reg initial[] = {
		{PLL1 + 0x14, 0x08200800}, {PLL1 + 0x18, 0x05028001}, {PLL1 + 0x1c, 0},
		{PLL1 + 0x20, 0},          {PLL1 + 0x24, 0},          {PLL1 + 0x28, 0},
		{PLL1 + 0xc, 0},           {PLL1 + 0x10, 0},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(initial); i++) {
		sail_check(f.writes[i].address == initial[i].address);
		sail_check(f.writes[i].value == initial[i].value);
	}
	sail_check(*reg(&f, PLL1 + 4) == 0xdeadbe53);
	sail_check(*reg(&f, PLL1 + 8) == 0xface0000);
	sail_check(collect(&f, PLL1, values) == 3);
	sail_check(values[0] == 0x2002 && values[1] == 0x2006 && values[2] == 0x2007);
	sail_check(collect(&f, PLL1 + 4, values) == 1 && values[0] == 0xdeadbe53);
	sail_check(f.low_reads == 0);
}

static void active_and_partial(void)
{
	struct fake f;
	struct monaco_timer timer;
	const struct reg changes0[] = {
		{0, 1},         {4, 1},           {0x10, 1},          {0x14, 1},
		{0x18, 1},      {0x18, 0x400000}, {0x18, 0x10000000}, {0x18, 0x400},
		{0x18, 0x4000}, {0x1c, 0x10000},
	};
	const struct reg changes1[] = {
		{0, 1},
		{4, 1},
		{8, 1},
		{0xc, 1},
		{0x10, 0x20000},
		{0x10, 0x1000},
		{0x10, 0x02000000},
		{0x10, 0x10000000},
	};
	for (unsigned int profile = 0; profile < ARRAY_SIZE(indices); profile++) {
		unsigned int index = indices[profile];
		uint32_t base = PLL0 + index * 0x1000U;
		const struct reg *changes = index == 1 ? changes1 : changes0;
		unsigned int count = index == 1 ? ARRAY_SIZE(changes1) : ARRAY_SIZE(changes0);
		struct monaco_clock c = prepare(&f, &timer);

		sail_check(enable(&c, index) == 0);
		/* Rate detection ignores these high halves and must leave them unchanged. */
		*reg(&f, base + (index == 1 ? 4U : 0x10U)) |= 0xabcd0000;
		*reg(&f, base + (index == 1 ? 8U : 0x14U)) |= 0xbeef0000;
		reset_trace(&f);
		sail_check(enable(&c, index) == 0 && f.write_count == 0);
		unsigned int active_reads = f.accesses;

		for (unsigned int i = 0; i < count; i++) {
			*reg(&f, base + changes[i].address) ^= changes[i].value;
			reset_trace(&f);
			sail_check(enable(&c, index) == MONACO_CLOCK_STATE && f.write_count == 0);
			*reg(&f, base + changes[i].address) ^= changes[i].value;
		}
		for (unsigned int failure = 1; failure <= active_reads; failure++) {
			reset_trace(&f);
			f.fail_at = failure;
			sail_check(enable(&c, index) == MONACO_CLOCK_IO);
			sail_check(f.accesses == failure && f.write_count == 0);
		}
		const uint32_t partial[] = {1, 2, 4, 6, LOCK, LOCK | 2U};

		for (unsigned int i = 0; i < ARRAY_SIZE(partial); i++) {
			c = prepare(&f, &timer);
			*reg(&f, base) = partial[i];
			sail_check(enable(&c, index) == MONACO_CLOCK_STATE && f.write_count == 0);
		}
	}
	for (unsigned int profile = 0; profile < ARRAY_SIZE(family_indices); profile++) {
		unsigned int index = family_indices[profile];
		struct monaco_clock c = prepare(&f, &timer);
		*reg(&f, PLL0 + index * 0x1000U + 4) = 1;
		sail_check(enable(&c, index) == MONACO_CLOCK_STATE && f.write_count == 0);
	}
}

static void bounds_and_faults(void)
{
	struct fake f;
	struct monaco_timer timer;

	for (unsigned int profile = 0; profile < ARRAY_SIZE(indices); profile++) {
		unsigned int index = indices[profile];
		unsigned int limit = index == 1 ? 51U : 1001U;
		uint32_t base = PLL0 + index * 0x1000U;
		struct monaco_clock c = prepare(&f, &timer);

		f.lock_after[index] = limit;
		sail_check(enable(&c, index) == 0);
		sail_check(f.polls[index] == limit && f.low_reads == 2 * (limit - 1));
		c = prepare(&f, &timer);
		f.lock_after[index] = limit + 1;
		sail_check(enable(&c, index) == MONACO_CLOCK_POLL_LIMIT);
		sail_check(f.polls[index] == limit && f.low_reads == 2 * limit);
		sail_check((*reg(&f, base) & 1U) == 0);
		c = prepare(&f, &timer);
		c.max_poll_reads = 3;
		f.lock_after[index] = 4;
		sail_check(enable(&c, index) == MONACO_CLOCK_POLL_LIMIT);
		sail_check(f.polls[index] == 3 && f.low_reads == 6);
		c = prepare(&f, &timer);
		f.lock_after[index] = 2;
		f.step = 0;
		sail_check(enable(&c, index) == MONACO_CLOCK_TIMEBASE);
		sail_check(f.polls[index] == 1 && (*reg(&f, base) & 1U) == 0);
		c = prepare(&f, &timer);
		f.lock_after[index] = 2;
		f.ticks = 0;
		sail_check(enable(&c, index) == MONACO_CLOCK_TIMEBASE);
		sail_check((*reg(&f, base) & 1U) == 0);
		c = prepare(&f, &timer);
		f.lock_after[index] = 2;
		if (index != 1) {
			*reg(&f, base) = 0x100;
		}
		sail_check(enable(&c, index) == 0);
		unsigned int total = f.accesses;

		for (unsigned int failure = 1; failure <= total; failure++) {
			c = prepare(&f, &timer);
			f.lock_after[index] = 2;
			if (index != 1) {
				*reg(&f, base) = 0x100;
			}
			f.fail_at = failure;
			int result = enable(&c, index);

			sail_check(result ==
			       (f.timer_failure ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
			sail_check(f.accesses == failure);
		}
		printf("PLL%u: %u fault positions and last-success/first-timeout boundaries "
		       "passed\n",
		       index, total);
	}
}

static struct monaco_clock prepare_refresh(struct fake *f, struct monaco_timer *timer)
{
	struct monaco_clock c = prepare(f, timer);

	sail_check(monaco_cpu_pll1_enable(&c) == 0);
	*reg(f, PLL1 + 4) = 0xdeadbe53;
	*reg(f, PLL1 + 8) = 0xface0000;
	*reg(f, PLL1 + 0x10) = 0x0240c0f0;
	reset_trace(f);
	f->refresh = true;
	return c;
}

static void refresh_sequence_and_guards(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare_refresh(&f, &timer);
	const struct event expected[] = {
		{'r', PLL1},        {'r', PLL1 + 4},   {'r', PLL1 + 8},    {'r', PLL1 + 0xc},
		{'r', PLL1 + 0x10}, {'r', PLL1 + 4},   {'w', PLL1 + 4},    {'r', PLL1 + 0x10},
		{'r', PLL1 + 8},    {'w', PLL1 + 8},   {'w', PLL1 + 0x10}, {'r', 0xf1400004},
		{'r', 0xf1400000},  {'r', 0xf1400004}, {'r', 0xf1400004},  {'r', 0xf1400000},
		{'r', 0xf1400004},  {'r', PLL1},
	};
	sail_check(monaco_cpu_pll1_refresh(&c) == 0);
	sail_check(f.event_count == ARRAY_SIZE(expected));
	for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
		sail_check(f.events[i].operation == expected[i].operation);
		sail_check(f.events[i].address == expected[i].address);
	}
	sail_check(f.write_count == 3);
	sail_check(f.writes[0].value == 0xdeadbe53);
	sail_check(f.writes[1].value == 0xface0000);
	sail_check(f.writes[2].value == 0x0240c0f0);
	sail_check(f.low_reads == 2 && f.polls[1] == 1);
	/* An already locked source still receives the mode-1 writes and delay. */
	c = prepare_refresh(&f, &timer);
	f.refresh = false;
	sail_check(monaco_cpu_pll1_refresh(&c) == 0);
	sail_check(f.write_count == 3 && f.low_reads == 2);
	const struct reg changes[] = {
		{0, 1},
		{0, 2},
		{0, 4},
		{0, LOCK},
		{4, 1},
		{8, 1},
		{0xc, 1},
		{0x10, 0x20000},
		{0x10, 0x1000},
		{0x10, 0x2000},
		{0x10, 0x02000000},
		{0x10, 0x10000000},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(changes); i++) {
		c = prepare_refresh(&f, &timer);
		f.enabling[1] = false;
		*reg(&f, PLL1 + changes[i].address) ^= changes[i].value;
		sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_STATE);
		sail_check(f.write_count == 0 && f.low_reads == 0);
	}
	c = prepare(&f, &timer);
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_STATE && f.write_count == 0);
}

static void refresh_bounds_and_faults(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare_refresh(&f, &timer);

	f.lock_after[1] = 51;
	sail_check(monaco_cpu_pll1_refresh(&c) == 0);
	sail_check(f.polls[1] == 51 && f.low_reads == 102 && f.write_count == 3);
	c = prepare_refresh(&f, &timer);
	f.lock_after[1] = 52;
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.polls[1] == 51 && f.low_reads == 104 && f.write_count == 3);
	c = prepare_refresh(&f, &timer);
	c.max_poll_reads = 3;
	f.lock_after[1] = 4;
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.polls[1] == 3 && f.low_reads == 8);
	c = prepare_refresh(&f, &timer);
	f.step = 0;
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_TIMEBASE);
	sail_check(f.polls[1] == 0 && f.write_count == 3);
	c = prepare_refresh(&f, &timer);
	f.ticks = 0;
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_TIMEBASE);
	sail_check(f.polls[1] == 0 && f.write_count == 3);
	c = prepare_refresh(&f, &timer);
	f.lock_after[1] = 2;
	sail_check(monaco_cpu_pll1_refresh(&c) == 0);
	unsigned int total = f.accesses;

	for (unsigned int failure = 1; failure <= total; failure++) {
		c = prepare_refresh(&f, &timer);
		f.lock_after[1] = 2;
		f.fail_at = failure;
		int result = monaco_cpu_pll1_refresh(&c);

		sail_check(result == (f.timer_failure ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
		sail_check(f.accesses == failure);
	}
	printf("PLL1 refresh: %u fault positions and mandatory delay/51-read boundary passed\n",
	       total);
}

static void raise_sequence_and_faults(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare_refresh(&f, &timer);

	sail_check(monaco_cpu_pll1_raise(&c) == 0);
	sail_check(f.write_count == 3);
	sail_check(f.writes[0].address == PLL1 + 4 && f.writes[0].value == 0xdeadbe60);
	sail_check(f.writes[1].address == PLL1 + 8 && f.writes[1].value == 0xface0000);
	sail_check(f.writes[2].address == PLL1 + 0x10 && f.writes[2].value == 0x0240c0f0);
	sail_check(f.low_reads == 2 && f.polls[1] == 1);
	sail_check(f.events[f.event_count - 5].address == PLL1);
	sail_check(f.events[f.event_count - 1].address == PLL1 + 0x10);
	reset_trace(&f);
	sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_STATE && f.write_count == 0);
	const struct reg changes[] = {
		{0, 1},
		{0, 2},
		{0, 4},
		{0, LOCK},
		{4, 1},
		{8, 1},
		{0xc, 1},
		{0x10, 0x20000},
		{0x10, 0x1000},
		{0x10, 0x2000},
		{0x10, 0x02000000},
		{0x10, 0x10000000},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(changes); i++) {
		c = prepare_refresh(&f, &timer);
		f.enabling[1] = false;
		*reg(&f, PLL1 + changes[i].address) ^= changes[i].value;
		sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_STATE);
		sail_check(f.write_count == 0);
	}
	c = prepare_refresh(&f, &timer);
	f.ignore_l = true;
	sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_STATE);
	sail_check(f.write_count == 3 && (*reg(&f, PLL1 + 4) & 0xffU) == 83);
	c = prepare_refresh(&f, &timer);
	f.lock_after[1] = 51;
	sail_check(monaco_cpu_pll1_raise(&c) == 0 && f.polls[1] == 51);
	c = prepare_refresh(&f, &timer);
	f.lock_after[1] = 52;
	sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_POLL_LIMIT && f.polls[1] == 51);
	c = prepare_refresh(&f, &timer);
	f.step = 0;
	sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_TIMEBASE && f.polls[1] == 0);
	c = prepare_refresh(&f, &timer);
	f.lock_after[1] = 2;
	sail_check(monaco_cpu_pll1_raise(&c) == 0);
	unsigned int total = f.accesses;

	for (unsigned int failure = 1; failure <= total; failure++) {
		c = prepare_refresh(&f, &timer);
		f.lock_after[1] = 2;
		f.fail_at = failure;
		int result = monaco_cpu_pll1_raise(&c);

		sail_check(result == (f.timer_failure ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
		sail_check(f.accesses == failure);
	}
	printf("PLL1 raise: %u fault positions and guarded L83-to-L96 transition passed\n", total);
}

int sail_model_test(void)
{
	const uint32_t forbidden[] = {
		0,          PLL0 + 8,   PLL0 + 0x38, PLL1 + 0x30, PLL2 + 8,     PLL2 + 0x38,
		0xf1105000, PLL4 + 8,   PLL4 + 0xc,  PLL4 + 0x38, PLL4 + 0x100, 0xf1103000,
		0xf111001c, 0xf1124000, 0xf870418c,  0xf140c000,  0xf03050c4,   0xf1000008,
		0xc1ff900c, UINT32_MAX,
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(forbidden); i++) {
		sail_check(!monaco_cpu_pll_write_allowed(forbidden[i]));
	}
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);

	sail_check(monaco_cpu_pll0_enable(NULL) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll2_enable(NULL) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll4_enable(NULL) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll1_refresh(NULL) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll1_raise(NULL) == MONACO_CLOCK_INVALID);
	c.max_poll_reads = 0;
	sail_check(monaco_cpu_pll1_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll2_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll4_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_INVALID);
	c.max_poll_reads = 4;
	timer.initialized = false;
	sail_check(monaco_cpu_pll0_enable(&c) == MONACO_CLOCK_INVALID && f.accesses == 0);
	sail_check(monaco_cpu_pll2_enable(&c) == MONACO_CLOCK_INVALID && f.accesses == 0);
	sail_check(monaco_cpu_pll4_enable(&c) == MONACO_CLOCK_INVALID && f.accesses == 0);
	sail_check(monaco_cpu_pll1_refresh(&c) == MONACO_CLOCK_INVALID && f.accesses == 0);
	sail_check(monaco_cpu_pll1_raise(&c) == MONACO_CLOCK_INVALID && f.accesses == 0);
	off_sequences();
	active_and_partial();
	bounds_and_faults();
	refresh_sequence_and_guards();
	refresh_bounds_and_faults();
	raise_sequence_and_faults();
	puts("CPU/NoC PLL tests passed: fixed rates, calibration, saved MODE, refresh, guards and "
	     "failures");
	return 0;
}
