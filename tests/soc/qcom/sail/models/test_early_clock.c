/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "early_clock.h"
#include "cpu_pll.h"

#include "sail_check.h"
#include <stdio.h>
#include <zephyr/sys/util.h>

#define PLL0          UINT32_C(0xf1100000)
#define PLL1          UINT32_C(0xf1101000)
#define PLL2          UINT32_C(0xf1102000)
#define PLL5          UINT32_C(0xf1105000)
#define PLL6          UINT32_C(0xf1106000)
#define DEMET         UINT32_C(0xf111d008)

struct reg {
	uint32_t address, value;
};
struct event {
	char kind;
	uint32_t address, value;
};
static struct event expected[81];
static unsigned int event_count;

struct fake {
	struct reg regs[256];
	unsigned int reg_count, accesses, fail_at, events, writes, high_reads;
	unsigned int pll_refresh_writes, branches;
	uint32_t stuck_command, stuck_branch;
	uint64_t ticks;
	bool demet_xo, timer_failure;
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

static bool known(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->reg_count; i++) {
		if (f->regs[i].address == address) {
			return true;
		}
	}
	return false;
}

static bool command(uint32_t address)
{
	for (unsigned int i = 0; i < event_count; i++) {
		if (expected[i].kind == 'R' && expected[i].address == address) {
			return true;
		}
	}
	return false;
}

static bool branch(uint32_t address)
{
	for (unsigned int i = 0; i < event_count; i++) {
		if (expected[i].kind == 'B' && expected[i].address == address) {
			return true;
		}
	}
	return false;
}

static bool fail(struct fake *f, uint32_t address)
{
	if (++f->accesses != f->fail_at) {
		return false;
	}
	f->timer_failure = address == 0xf1400000 || address == 0xf1400004;
	return true;
}

static int fake_read(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;

	if (fail(f, address)) {
		return -1;
	}
	if (address == 0xf1400010) {
		*value = 19200000;
		return 0;
	}
	if (address == 0xf1400000) {
		*value = (uint32_t)f->ticks;
		return 0;
	}
	if (address == 0xf1400004) {
		*value = (uint32_t)(f->ticks >> 32);
		if (++f->high_reads % 2 == 0) {
			f->ticks += 20;
		}
		return 0;
	}
	sail_check(known(f, address));
	uint32_t *value_ptr = reg(f, address);

	if (command(address) && address != f->stuck_command) {
		*value_ptr &= ~UINT32_C(1);
	}
	*value = *value_ptr;
	return 0;
}

static void check_event(struct fake *f, char kind, uint32_t address, uint32_t value)
{
	if (f->demet_xo && f->events < event_count && expected[f->events].kind == 'R' &&
	    expected[f->events].address == DEMET) {
		f->events++;
	}
	sail_check(f->events < event_count);
	const struct event *event = &expected[f->events++];

	sail_check(event->kind == kind && event->address == address);
	sail_check(event->value == value);
}

static int fake_write(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;

	sail_check(monaco_clock_write_allowed(address) || monaco_cpu_pll_write_allowed(address));
	sail_check(known(f, address));
	if (fail(f, address)) {
		return -1;
	}
	uint32_t *old = reg(f, address);

	f->writes++;
	if (command(address)) {
		sail_check(value == (*old | 1U));
	} else if (command(address - 4U)) {
		sail_check((value & ~UINT32_C(0x371f)) == (*old & ~UINT32_C(0x371f)));
		check_event(f, 'R', address - 4U, value & 0x371fU);
	} else if (branch(address)) {
		sail_check(value == (*old | 1U));
		check_event(f, 'B', address, value & 1U);
		f->branches++;
	} else {
		/* All sources already match; only the final CPU refresh may write a PLL. */
		const uint32_t refresh[] = {PLL1 + 4, PLL1 + 8, PLL1 + 0x10};

		sail_check(f->events == event_count && f->branches == 62);
		sail_check(f->pll_refresh_writes < ARRAY_SIZE(refresh));
		sail_check(address == refresh[f->pll_refresh_writes++]);
		sail_check(value == *old);
	}
	*old = value;
	return 0;
}

static void seed_pll(struct fake *f, uint32_t base)
{
	if (base == PLL0 || base == PLL2) {
		*reg(f, base) = 0x80000005;
		*reg(f, base + 4) = 1;
		*reg(f, base + 0x10) = base == PLL0 ? 0x1fU : 0x22U;
		*reg(f, base + 0x14) = base == PLL0 ? 0x4000U : 0x9555U;
		*reg(f, base + 0x18) = base == PLL0 ? 0x401U : 1U;
		*reg(f, base + 0x1c) = 0;
	} else if (base == PLL1) {
		*reg(f, base) = 0x80000007;
		*reg(f, base + 4) = 0x53;
		*reg(f, base + 8) = 0;
		*reg(f, base + 0xc) = 1;
		*reg(f, base + 0x10) = 0x02000000;
	} else {
		*reg(f, base) = 0x80001005;
		*reg(f, base + 4) = 1;
		*reg(f, base + 0x10) = 2;
		*reg(f, base + 0x14) = 0x403;
		*reg(f, base + 0x18) = 0x200000;
	}
}

static struct monaco_clock prepare(struct fake *f, struct monaco_timer *timer, bool demet_xo)
{
	*f = (struct fake){.demet_xo = demet_xo};
	for (unsigned int i = 0; i < event_count; i++) {
		uint32_t address = expected[i].address;

		if (expected[i].kind == 'R') {
			*reg(f, address) = 0x80;
			*reg(f, address + 4) = 0xa000c000U | expected[i].value;
		} else {
			*reg(f, address) = 0x20;
		}
	}
	if (demet_xo) {
		*reg(f, DEMET + 4) = 0xa000c001;
	}
	const uint32_t sources[] = {PLL0, PLL1, PLL2, PLL5, PLL6};

	for (unsigned int i = 0; i < ARRAY_SIZE(sources); i++) {
		seed_pll(f, sources[i]);
	}
	struct monaco_timer_io io = {f, fake_read, fake_write};

	sail_check(monaco_timer_init(timer, &io, 4) == 0);
	f->accesses = 0;
	return (struct monaco_clock){io, timer, 4};
}

static void success_and_faults(void)
{
	for (unsigned int xo = 0; xo < 2; xo++) {
		struct fake f;
		struct monaco_timer timer;
		struct monaco_early_clock_status status;
		struct monaco_clock c = prepare(&f, &timer, xo != 0);

		sail_check(monaco_early_clocks_enable(&c, &status) == 0);
		sail_check(status.stage == MONACO_EARLY_CLOCK_COMPLETE && status.address == 0);
		sail_check(status.completed_branches == 62 && f.branches == 62);
		sail_check(f.events == event_count && f.pll_refresh_writes == 3);
		sail_check(*reg(&f, DEMET + 4) == (xo ? 0xa000c001U : 0xa000c301U));
		unsigned int accesses = f.accesses;

		for (unsigned int fault = 1; fault <= accesses; fault++) {
			c = prepare(&f, &timer, xo != 0);
			f.fail_at = fault;
			int result = monaco_early_clocks_enable(&c, &status);

			sail_check(result ==
			       (f.timer_failure ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
			sail_check(f.accesses == fault);
			sail_check(status.stage != MONACO_EARLY_CLOCK_COMPLETE);
		}
		printf("early clocks: DEMET %s, %u access-fault cases\n", xo ? "XO" : "PLL5",
		       accesses);
	}
}

static void inherited_muxes(void)
{
	const uint32_t cfgs[] = {0xf112a024, 0xf1123020, DEMET + 4};
	const unsigned int first[] = {27, 41, 49};
	struct fake f;
	struct monaco_timer timer;
	struct monaco_early_clock_status status;

	for (unsigned int mux = 0; mux < ARRAY_SIZE(cfgs); mux++) {
		for (unsigned int raw = 0; raw < 2; raw++) {
			for (unsigned int mode = 0; mode < 4; mode++) {
				struct monaco_clock c = prepare(&f, &timer, mux == 2);

				*reg(&f, cfgs[mux]) = 0xa000c000U | (mode << 12) | raw;
				sail_check(monaco_early_clocks_enable(&c, &status) == 0);
				sail_check(status.completed_branches == 62);
				if (mux == 2) {
					sail_check(*reg(&f, cfgs[mux]) ==
					       (0xa000c000U | (mode << 12) | raw));
				}
			}
		}
		for (unsigned int mismatch = 0; mismatch < 2; mismatch++) {
			struct monaco_clock c = prepare(&f, &timer, false);

			*reg(&f, cfgs[mux]) = mismatch ? 0x701 : 2;
			sail_check(monaco_early_clocks_enable(&c, &status) == MONACO_CLOCK_STATE);
			sail_check(status.stage == MONACO_EARLY_CLOCK_DOMAIN);
			sail_check(status.address == cfgs[mux] - 4);
			sail_check(status.completed_branches == first[mux]);
		}
	}
}

static void refusal_and_bounded_waits(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_early_clock_status status;
	struct monaco_clock c = prepare(&f, &timer, false);

	*reg(&f, PLL1 + 4) = 84;
	sail_check(monaco_early_clocks_enable(&c, &status) == MONACO_CLOCK_STATE);
	sail_check(f.writes == 0 && status.completed_branches == 0);
	sail_check(status.stage == MONACO_EARLY_CLOCK_DOMAIN && status.address == 0xf1124000);
	for (unsigned int before = 0; before < 2; before++) {
		c = prepare(&f, &timer, false);
		f.stuck_command = 0xf111103c;
		if (before) {
			*reg(&f, f.stuck_command) |= 1;
		}
		sail_check(monaco_early_clocks_enable(&c, &status) == MONACO_CLOCK_POLL_LIMIT);
		sail_check(status.stage == MONACO_EARLY_CLOCK_DOMAIN);
		sail_check(status.completed_branches == 16 && status.address == f.stuck_command);
	}
	c = prepare(&f, &timer, false);
	f.stuck_branch = 0xf111b004;
	*reg(&f, f.stuck_branch) = 0x80000020;
	sail_check(monaco_early_clocks_enable(&c, &status) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(status.stage == MONACO_EARLY_CLOCK_BRANCH);
	sail_check(status.completed_branches == 17 && status.address == f.stuck_branch);

	c = prepare(&f, &timer, false);
	status = (struct monaco_early_clock_status){MONACO_EARLY_CLOCK_COMPLETE, 99, 62};
	sail_check(monaco_early_clocks_enable(NULL, &status) == MONACO_CLOCK_INVALID);
	sail_check(status.stage == MONACO_EARLY_CLOCK_INPUT && status.completed_branches == 0);
	sail_check(monaco_early_clocks_enable(&c, NULL) == MONACO_CLOCK_INVALID);
	c.max_poll_reads = 0;
	sail_check(monaco_early_clocks_enable(&c, &status) == MONACO_CLOCK_INVALID);
	sail_check(f.accesses == 0 && f.writes == 0);
}

int sail_model_test(void)
{
	FILE *fixture = fopen(SAIL_TEST_FIXTURE_DIR "/early-clock.trace", "r");

	sail_check(fixture);
	while (event_count < ARRAY_SIZE(expected)) {
		struct event *event = &expected[event_count];

		if (fscanf(fixture, " %c %x %x", &event->kind, &event->address, &event->value) !=
		    3) {
			break;
		}
		event_count++;
	}
	sail_check(event_count == ARRAY_SIZE(expected));
	sail_check(fclose(fixture) == 0);
	success_and_faults();
	inherited_muxes();
	refusal_and_bounded_waits();
	puts("early clock sequence tests passed");
	return 0;
}
