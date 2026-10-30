/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock.h"

#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define PLL5          UINT32_C(0xf1105000)
#define PLL6          UINT32_C(0xf1106000)
#define CMD           UINT32_C(0xf111d008)
#define CFG           (CMD + 4)
#define CBCR          UINT32_C(0xf111d020)
#define SE0_CBCR      UINT32_C(0xf1115038)
#define SE0_CMD       UINT32_C(0xf1115040)
#define SE0_CFG       (SE0_CMD + 4)
#define SE0_DFS       UINT32_C(0xf1115054)

/* The early 62 branches, two MD BIST branches and SE0. */
static const uint32_t branch_addresses[] = {
	0xf1124018, 0xf1124038, 0xf1124030, 0xf1124034, 0xf112402c, 0xf1124040, 0xf1124020,
	0xf1124044, 0xf111101c, 0xf1111014, 0xf1134000, 0xf1134008, 0xf1134010, 0xf1134018,
	0xf1134020, 0xf1134028, 0xf1124028, 0xf111b004, 0xf1110010, 0xf1111004, 0xf1111038,
	0xf1122004, 0xf111e004, 0xf112a000, 0xf112a004, 0xf112a008, 0xf112a00c, 0xf112a010,
	0xf112a014, 0xf112a018, 0xf112a01c, 0xf113203c, 0xf113303c, 0xf111000c, 0xf1111010,
	0xf1114030, 0xf1110008, 0xf111100c, 0xf1110050, 0xf1110018, 0xf1111018, 0xf1110014,
	0xf1123014, 0xf1128008, 0xf1110034, 0xf1111094, 0xf112300c, 0xf1111054, 0xf1111090,
	0xf111d020, 0xf1115018, 0xf111500c, 0xf111300c, 0xf1113004, 0xf1115004, 0xf1115008,
	0xf1111070, 0xf113201c, 0xf113301c, 0xf1137000, 0xf1132038, 0xf1133038, 0xf1136004,
	0xf1136008, SE0_CBCR,
};

static const uint32_t command_addresses[] = {
	0xf1124000, 0xf111103c, 0xf112a020, 0xf1114084, 0xf1114054, 0xf111001c, 0xf112301c,
	0xf1110038, 0xf1111020, 0xf1111058, 0xf111d008, 0xf1115020, 0xf1111074, 0xf1132004,
	0xf1133004, 0xf1137008, 0xf1132020, 0xf1133020, SE0_CMD,
};

static bool contains(const uint32_t *addresses, size_t count, uint32_t address)
{
	for (size_t i = 0; i < count; i++) {
		if (addresses[i] == address) {
			return true;
		}
	}
	return false;
}

static bool command_address(uint32_t address)
{
	return contains(command_addresses, ARRAY_SIZE(command_addresses), address);
}

struct reg {
	uint32_t address, value;
};
struct event {
	uint32_t address, value;
	bool write;
};
struct fake {
	struct reg regs[64];
	unsigned int count, accesses, fail_at, writes, events, high_reads;
	unsigned int cbcr_reads, mode_polls, command_reads, lock_after;
	struct event log[16384];
	uint64_t ticks, step;
	bool failure_is_timer, hold_cbcr, hold_rcg, busy_config, never_lock, ignore_cfg;
	bool check_parent_order;
};

static uint32_t *reg(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->count; i++) {
		if (f->regs[i].address == address) {
			return &f->regs[i].value;
		}
	}
	sail_check(f->count < 64);
	f->regs[f->count].address = address;
	return &f->regs[f->count++].value;
}

static bool timer_address(uint32_t address)
{
	return address == 0xf1400000 || address == 0xf1400004 || address == 0xf1400010;
}

static bool fail(struct fake *f, uint32_t address, bool write, uint32_t value)
{
	sail_check(f->events < ARRAY_SIZE(f->log));
	f->log[f->events++] = (struct event){address, value, write};
	if (++f->accesses != f->fail_at) {
		return false;
	}
	f->failure_is_timer = timer_address(address);
	return true;
}

static int fake_read(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	uint32_t *r;

	if (fail(f, address, false, 0)) {
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
			f->ticks += f->step;
		}
		return 0;
	}
	sail_check(monaco_clock_write_allowed(address) || address == 0xf870418c ||
	       address == 0xf8704190 || address == 0xf8704194 || address == 0xf8704198 ||
	       address == SE0_DFS);
	r = reg(f, address);
	if (address == CBCR || address == SE0_CBCR) {
		f->cbcr_reads++;
		if ((*r & 1) && !f->hold_cbcr) {
			*r &= ~UINT32_C(0xd0000000);
		}
	}
	if (command_address(address)) {
		f->command_reads++;
		if (!f->hold_rcg) {
			*r &= ~UINT32_C(1);
		}
	}
	if ((address == PLL5 || address == PLL6) && (*r & 4)) {
		f->mode_polls++;
		if (!f->never_lock && f->mode_polls >= f->lock_after) {
			*r |= UINT32_C(0x80000000);
		}
	}
	*value = *r;
	return 0;
}

static int fake_write(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;

	sail_check(monaco_clock_write_allowed(address));
	if (fail(f, address, true, value)) {
		return -1;
	}
	if (f->check_parent_order && address == CFG) {
		sail_check((*reg(f, PLL5) & 0x80001005U) == 0x80001005U);
	}
	if (f->check_parent_order && address == CBCR) {
		sail_check((*reg(f, CFG) & 0x371fU) == 0x301);
		sail_check((*reg(f, CMD) & 1U) == 0);
	}
	f->writes++;
	if ((address == CFG || address == SE0_CFG) && f->ignore_cfg) {
		return 0;
	}
	*reg(f, address) = value;
	if ((address == PLL5 || address == PLL6) && (value & 0x400) && f->busy_config) {
		*reg(f, address) |= 0x200;
	}
	return 0;
}

static void reset_trace(struct fake *f)
{
	f->accesses = f->writes = f->events = 0;
	f->cbcr_reads = f->mode_polls = f->command_reads = 0;
	f->fail_at = 0;
	f->failure_is_timer = false;
}

static struct monaco_clock prepare(struct fake *f, struct monaco_timer *timer)
{
	struct monaco_timer_io io = {f, fake_read, fake_write};
	*f = (struct fake){.step = 20};
	sail_check(monaco_timer_init(timer, &io, 4) == 0);
	reset_trace(f);
	return (struct monaco_clock){io, timer, 4};
}

static unsigned int first_write(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->events; i++) {
		if (f->log[i].write && f->log[i].address == address) {
			return i;
		}
	}
	return UINT32_MAX;
}

static bool expected_allowed(uint32_t address)
{
	static const uint32_t pll_offsets[] = {
		0, 4, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
	};
	if (contains(branch_addresses, ARRAY_SIZE(branch_addresses), address) ||
	    command_address(address) || command_address(address - 4)) {
		return true;
	}
	for (size_t i = 0; i < ARRAY_SIZE(pll_offsets); i++) {
		if (address == PLL5 + pll_offsets[i] || address == PLL6 + pll_offsets[i]) {
			return true;
		}
	}
	return false;
}

static void complete_allowlist(void)
{
	struct fake f;
	struct monaco_timer timer;
	unsigned int allowed_count = 0;

	sail_check(ARRAY_SIZE(branch_addresses) == 65);
	sail_check(ARRAY_SIZE(command_addresses) == 19);
	/* Check holes and byte alignment as well as every allowed address. */
	for (uint32_t address = 0xf1100000; address < 0xf1138000; address++) {
		bool expected = expected_allowed(address);

		sail_check(monaco_clock_write_allowed(address) == expected);
		if (expected) {
			allowed_count++;
		}
	}
	sail_check(allowed_count == 131);
	for (size_t i = 0; i < ARRAY_SIZE(branch_addresses); i++) {
		struct monaco_clock c = prepare(&f, &timer);

		sail_check(monaco_clock_cbcr_enable(&c, branch_addresses[i]) == 0);
		sail_check(f.writes == 1);
		sail_check(f.log[first_write(&f, branch_addresses[i])].value == 1);
		sail_check(monaco_clock_rcg_update(&c, branch_addresses[i], 0, 2) ==
		       MONACO_CLOCK_INVALID);
	}
	for (size_t i = 0; i < ARRAY_SIZE(command_addresses); i++) {
		struct monaco_clock c = prepare(&f, &timer);

		sail_check(monaco_clock_rcg_update(&c, command_addresses[i], 7, 32) == 0);
		sail_check(f.writes == 2);
		sail_check(*reg(&f, command_addresses[i] + 4) == 0x71f);
		sail_check(monaco_clock_cbcr_enable(&c, command_addresses[i]) ==
			   MONACO_CLOCK_INVALID);
		sail_check(monaco_clock_cbcr_enable(&c, command_addresses[i] + 4) ==
		       MONACO_CLOCK_INVALID);
	}
}

static void allowlist_and_invalid(void)
{
	const uint32_t forbidden[] = {
		0,          0xf1105008, 0xf1105040, 0xf870418c, 0xf8704190, 0xf1106008,
		0xf110600c, 0xf1106040, 0xf8704194, 0xf8704198, 0xf0305084, 0xf0305088,
		0xf030508c, 0xf0305090, 0xf0300200, 0xf100000c, 0xf1000520, 0xf1000524,
		0xf0300058, 0xf0300074, 0xf140c000, 0xf1400010, 0xf03050c4, 0xf1000008,
		0xf1110700, 0xf1111700, 0xc1ff900c, 0xc1ffc000, 0xffffffff,
	};
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);

	for (unsigned int i = 0; i < ARRAY_SIZE(forbidden); i++) {
		sail_check(!monaco_clock_write_allowed(forbidden[i]));
	}
	sail_check(monaco_clock_cbcr_enable(&c, 0xf03050c4) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_rcg_update(&c, CMD, 8, 2) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 0) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 33) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_rcg_update(&c, 0xf03050c4, 3, 2) == MONACO_CLOCK_INVALID);
	c.max_poll_reads = 0;
	sail_check(monaco_clock_pll5_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_cbcr_enable(NULL, CBCR) == MONACO_CLOCK_INVALID);
	c.max_poll_reads = 4;
	timer.initialized = false;
	sail_check(monaco_clock_pll5_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_INVALID);
	sail_check(monaco_clock_pll6_enable(NULL) == MONACO_CLOCK_INVALID);
	sail_check(f.accesses == 0);
	sail_check(monaco_clock_write_allowed(CBCR) && monaco_clock_write_allowed(CFG));
}

static void branch_and_rcg(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);
	*reg(&f, CBCR) = 0x80244000;
	sail_check(monaco_clock_cbcr_enable(&c, CBCR) == 0);
	sail_check(f.log[first_write(&f, CBCR)].value == 0x80244001);
	c = prepare(&f, &timer);
	f.hold_cbcr = true;
	*reg(&f, CBCR) = 0xd0000002;
	sail_check(monaco_clock_cbcr_enable(&c, CBCR) == 0);
	sail_check(f.cbcr_reads == 2);
	c = prepare(&f, &timer);
	f.hold_cbcr = true;
	*reg(&f, CBCR) = 0x80000000;
	sail_check(monaco_clock_cbcr_enable(&c, CBCR) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.cbcr_reads == 1 + c.max_poll_reads && f.writes == 1);
	c = prepare(&f, &timer);
	*reg(&f, CMD) = 0x40000000;
	*reg(&f, CFG) = 0xa5ffefde;
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 2) == 0);
	sail_check(*reg(&f, CFG) == ((0xa5ffefdeU & 0xffffc8e0U) | 0x301U));
	sail_check(f.log[first_write(&f, CMD)].value == 0x40000001);
	reset_trace(&f);
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 2) == 0 && f.writes == 2);
	sail_check(f.log[first_write(&f, CMD)].value == 0x40000001);
	c = prepare(&f, &timer);
	f.hold_rcg = true;
	*reg(&f, CMD) = 1;
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 2) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.command_reads == c.max_poll_reads && f.writes == 0);
	c = prepare(&f, &timer);
	f.hold_rcg = true;
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 2) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.writes == 2 && f.command_reads == c.max_poll_reads + 2);
	c = prepare(&f, &timer);
	f.ignore_cfg = true;
	sail_check(monaco_clock_rcg_update(&c, CMD, 3, 2) == MONACO_CLOCK_STATE);
}

static void pll_and_parent_order(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);

	sail_check(monaco_clock_pll5_enable(&c) == 0);
	sail_check(*reg(&f, PLL5 + 0x28) == 0x040001ac);
	sail_check(*reg(&f, PLL5 + 0x34) == 0x40100000);
	sail_check(*reg(&f, PLL5 + 0x38) == 0x440001b2);
	for (unsigned int i = 0; i < f.events; i++) {
		sail_check(f.log[i].address != 0xf8704190);
	}
	c = prepare(&f, &timer);
	f.check_parent_order = true;
	*reg(&f, 0xf870418c) = 0x1234U << 5;
	*reg(&f, 0xf8704190) = 0xfeedc0de;
	sail_check(monaco_clock_demet_enable(&c) == 0);
	sail_check(*reg(&f, PLL5 + 0x28) == 0x04001234);
	sail_check(*reg(&f, PLL5 + 0x38) == 0x440001b3);
	sail_check(*reg(&f, PLL5 + 0x34) ==
	       (((0x40100100U & 0xffffc1ffU) | ((0xfeedc0deU >> 17) & 0x3800U)) & 0xfff83fffU) +
		       ((0xfeedc0deU >> 9) & 0x70000U));
	sail_check((*reg(&f, PLL5) & 0x80001005U) == 0x80001005U);
	sail_check(*reg(&f, PLL5 + 0x10) == 2);
	sail_check((*reg(&f, PLL5 + 0x14) & 0x7c3U) == 0x403);
	sail_check((*reg(&f, PLL5 + 0x18) & 0x400U) == 0);
	sail_check(first_write(&f, PLL5 + 4) < first_write(&f, CFG));
	sail_check(first_write(&f, CMD) < first_write(&f, CBCR));
	reset_trace(&f);
	sail_check(monaco_clock_pll5_enable(&c) == 0 && f.writes == 0);
	const struct reg mismatches[] = {
		{PLL5, 1},        {PLL5, 0x1000},      {PLL5, 0x200},    {PLL5 + 4, 1},
		{PLL5 + 0x10, 1}, {PLL5 + 0x14, 0x40}, {PLL5 + 0x14, 2}, {PLL5 + 0x18, 0x100000},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(mismatches); i++) {
		*reg(&f, mismatches[i].address) ^= mismatches[i].value;
		reset_trace(&f);
		sail_check(monaco_clock_pll5_enable(&c) == MONACO_CLOCK_STATE && f.writes == 0);
		*reg(&f, mismatches[i].address) ^= mismatches[i].value;
	}
	*reg(&f, PLL5 + 0x14) |= 0x80000000;
	reset_trace(&f);
	sail_check(monaco_clock_pll5_enable(&c) == 0 && f.writes == 0);
	sail_check((*reg(&f, PLL5 + 0x14) & 0x80000000U) != 0);
	for (unsigned int failure = 1; failure <= 5; failure++) {
		reset_trace(&f);
		f.fail_at = failure;
		sail_check(monaco_clock_pll5_enable(&c) == MONACO_CLOCK_IO);
		sail_check(f.accesses == failure && f.writes == 0);
	}
	c = prepare(&f, &timer);
	f.never_lock = true;
	*reg(&f, PLL5) = 4;
	sail_check(monaco_clock_pll5_enable(&c) == MONACO_CLOCK_STATE && f.writes == 0);
	c = prepare(&f, &timer);
	*reg(&f, PLL5 + 4) = 1;
	sail_check(monaco_clock_pll5_enable(&c) == MONACO_CLOCK_STATE && f.writes == 0);
	c = prepare(&f, &timer);
	f.busy_config = true;
	sail_check(monaco_clock_demet_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(first_write(&f, PLL5 + 4) == UINT32_MAX);
	sail_check(first_write(&f, CBCR) == UINT32_MAX);
	c = prepare(&f, &timer);
	f.never_lock = true;
	sail_check(monaco_clock_demet_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.mode_polls == c.max_poll_reads);
	sail_check(first_write(&f, CFG) == UINT32_MAX);
	c = prepare(&f, &timer);
	f.step = 0;
	sail_check(monaco_clock_demet_enable(&c) == MONACO_CLOCK_TIMEBASE);
	sail_check(first_write(&f, CFG) == UINT32_MAX);
	sail_check((*reg(&f, PLL5 + 0x18) & 0x400U) != 0);
}

static void injected_failures(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);

	f.check_parent_order = true;
	*reg(&f, 0xf870418c) = 0x1234U << 5;
	*reg(&f, 0xf8704190) = 0xfeedc0de;
	sail_check(monaco_clock_demet_enable(&c) == 0);
	unsigned int total = f.accesses;

	for (unsigned int failure = 1; failure <= total; failure++) {
		c = prepare(&f, &timer);
		f.check_parent_order = true;
		*reg(&f, 0xf870418c) = 0x1234U << 5;
		*reg(&f, 0xf8704190) = 0xfeedc0de;
		f.fail_at = failure;
		int result = monaco_clock_demet_enable(&c);

		sail_check(result ==
			   (f.failure_is_timer ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
		sail_check(f.accesses == failure);
	}
	/* Fail inside the delay used by a blocked branch, not only PLL's pulse. */
	c = prepare(&f, &timer);
	f.hold_cbcr = true;
	*reg(&f, CBCR) = 0x80000000;
	f.fail_at = 4;
	sail_check(monaco_clock_cbcr_enable(&c, CBCR) == MONACO_CLOCK_TIMEBASE);
	sail_check(f.accesses == 4);
	printf("Clock fault sweep: %u access positions checked\n", total);
}

static void pll6_profile_and_guards(void)
{
	static const struct reg initial[] = {
		{PLL6 + 0x1c, 0x60004f61}, {PLL6 + 0x20, 0x0001c808}, {PLL6 + 0x24, 0},
		{PLL6 + 0x28, 0x040001ac}, {PLL6 + 0x2c, 0},          {PLL6 + 0x30, 0x0081c6de},
		{PLL6 + 0x34, 0x40100000}, {PLL6 + 0x38, 0x440001b2}, {PLL6 + 0x3c, 0x3d8},
		{PLL6 + 0x14, 0},          {PLL6 + 0x18, 0x00250002},
	};
	static const struct reg mismatches[] = {
		{PLL6, 1},        {PLL6, 0x1000},      {PLL6, 0x200},    {PLL6 + 4, 1},
		{PLL6 + 0x10, 1}, {PLL6 + 0x14, 0x40}, {PLL6 + 0x14, 2}, {PLL6 + 0x18, 0x100000},
	};
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);
	/* Only low16 of this fuse contains the PLL6 trim. */
	*reg(&f, 0xf8704194) = 0xffff0000;
	sail_check(monaco_clock_pll6_enable(&c) == 0);
	sail_check(*reg(&f, PLL6 + 0x28) == 0x040001ac);
	sail_check(*reg(&f, PLL6 + 0x34) == 0x40100000);
	sail_check(*reg(&f, PLL6 + 0x38) == 0x440001b2);
	for (unsigned int i = 0; i < f.events; i++) {
		sail_check(f.log[i].address != 0xf8704198);
	}
	c = prepare(&f, &timer);
	*reg(&f, 0xf8704194) = 0xabcd1234;
	*reg(&f, 0xf8704198) = 0xfeedc0de;
	sail_check(monaco_clock_pll6_enable(&c) == 0);
	sail_check(*reg(&f, PLL6 + 0x28) == 0x04001234);
	sail_check(*reg(&f, PLL6 + 0x34) == 0x40162900);
	sail_check(*reg(&f, PLL6 + 0x38) == 0x440001b3);
	sail_check(*reg(&f, PLL6 + 0x10) == 2);
	sail_check((*reg(&f, PLL6 + 0x14) & 0x7c3U) == 0x403);
	sail_check((*reg(&f, PLL6 + 0x18) & 0x300400U) == 0x200000);
	sail_check((*reg(&f, PLL6) & 0x80001005U) == 0x80001005U);
	unsigned int written = 0, fuse_reads = 0;

	for (unsigned int i = 0; i < f.events; i++) {
		const struct event *e = &f.log[i];

		sail_check((e->address >= PLL6 && e->address < PLL6 + 0x40) ||
		       e->address == 0xf8704194 || e->address == 0xf8704198 ||
		       timer_address(e->address));
		if (e->address == 0xf8704198) {
			sail_check(!e->write && written == 0);
			fuse_reads++;
		}
		if (!e->write) {
			continue;
		}
		if (written < ARRAY_SIZE(initial)) {
			sail_check(e->address == initial[written].address);
			sail_check(e->value == initial[written].value);
		}
		written++;
	}
	sail_check(written > ARRAY_SIZE(initial) && fuse_reads == 1);
	reset_trace(&f);
	sail_check(monaco_clock_pll6_enable(&c) == 0 && f.writes == 0);
	for (size_t i = 0; i < ARRAY_SIZE(mismatches); i++) {
		*reg(&f, mismatches[i].address) ^= mismatches[i].value;
		reset_trace(&f);
		sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_STATE && f.writes == 0);
		*reg(&f, mismatches[i].address) ^= mismatches[i].value;
	}
	*reg(&f, PLL6 + 0x14) |= 0x80000000;
	reset_trace(&f);
	sail_check(monaco_clock_pll6_enable(&c) == 0 && f.writes == 0);
	sail_check((*reg(&f, PLL6 + 0x14) & 0x80000000U) != 0);
	for (unsigned int failure = 1; failure <= 5; failure++) {
		reset_trace(&f);
		f.fail_at = failure;
		sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_IO);
		sail_check(f.accesses == failure && f.writes == 0);
	}
	const uint32_t partial_modes[] = {1, 4, 0x200, 0x1000, 0x80000000};

	for (size_t i = 0; i < ARRAY_SIZE(partial_modes); i++) {
		c = prepare(&f, &timer);
		f.never_lock = true;
		*reg(&f, PLL6) = partial_modes[i];
		sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_STATE && f.writes == 0);
	}
	c = prepare(&f, &timer);
	*reg(&f, PLL6 + 4) = 1;
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_STATE && f.writes == 0);
}

static void pll6_limits_and_faults(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);

	f.busy_config = true;
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(first_write(&f, PLL6 + 4) == UINT32_MAX);
	c = prepare(&f, &timer);
	f.never_lock = true;
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.mode_polls == c.max_poll_reads);
	sail_check((*reg(&f, PLL6) & 0x1001U) == 0);
	c = prepare(&f, &timer);
	f.step = 0;
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_TIMEBASE);
	sail_check((*reg(&f, PLL6 + 0x18) & 0x400U) != 0);
	/* The fixed lock bound includes poll 1501, even with a larger caller budget. */
	c = prepare(&f, &timer);
	c.max_poll_reads = 2000;
	f.lock_after = 1501;
	sail_check(monaco_clock_pll6_enable(&c) == 0);
	c = prepare(&f, &timer);
	c.max_poll_reads = 2000;
	f.lock_after = 1502;
	sail_check(monaco_clock_pll6_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.mode_polls == 1501);
	sail_check((*reg(&f, PLL6) & 0x1001U) == 0);
	for (unsigned int trimmed = 0; trimmed <= 1; trimmed++) {
		c = prepare(&f, &timer);
		*reg(&f, 0xf8704194) = trimmed ? 0xabcd1234 : 0;
		*reg(&f, 0xf8704198) = 0xfeedc0de;
		sail_check(monaco_clock_pll6_enable(&c) == 0);
		unsigned int total = f.accesses;

		for (unsigned int failure = 1; failure <= total; failure++) {
			c = prepare(&f, &timer);
			*reg(&f, 0xf8704194) = trimmed ? 0xabcd1234 : 0;
			*reg(&f, 0xf8704198) = 0xfeedc0de;
			f.fail_at = failure;
			int result = monaco_clock_pll6_enable(&c);

			sail_check(result ==
			       (f.failure_is_timer ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
			sail_check(f.accesses == failure);
		}
		printf("PLL6 %s fault sweep: %u access positions checked\n",
		       trimmed ? "trimmed" : "untrimmed", total);
	}
}

static void se0_clock(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_clock c = prepare(&f, &timer);

	*reg(&f, SE0_CFG) = 0xa5ffefde;
	*reg(&f, SE0_CMD) = 0x40000000;
	*reg(&f, SE0_CBCR) = 0x80244000;
	sail_check(monaco_clock_se0_enable(&c) == 0);
	sail_check(*reg(&f, SE0_CFG) == ((0xa5ffefdeU & ~0x371fU) | 1U));
	sail_check(f.writes == 3);
	sail_check(f.log[0].address == SE0_DFS && !f.log[0].write);
	sail_check(f.log[first_write(&f, SE0_CMD)].value == 0x40000001);
	sail_check(f.log[first_write(&f, SE0_CBCR)].value == 0x80244001);
	sail_check(first_write(&f, SE0_CFG) < first_write(&f, SE0_CMD));
	sail_check(first_write(&f, SE0_CMD) < first_write(&f, SE0_CBCR));
	unsigned int accesses = f.accesses;

	for (unsigned int failure = 1; failure <= accesses; failure++) {
		c = prepare(&f, &timer);
		*reg(&f, SE0_CMD) = 0x40000000;
		*reg(&f, SE0_CBCR) = 0x80244000;
		f.fail_at = failure;
		sail_check(monaco_clock_se0_enable(&c) == MONACO_CLOCK_IO);
		sail_check(f.accesses == failure);
	}
	c = prepare(&f, &timer);
	*reg(&f, SE0_DFS) = 1;
	sail_check(monaco_clock_se0_enable(&c) == MONACO_CLOCK_STATE);
	sail_check(f.accesses == 1 && f.writes == 0);
	reset_trace(&f);
	sail_check(monaco_clock_rcg_update(&c, SE0_CMD, 0, 2) == MONACO_CLOCK_STATE);
	sail_check(f.accesses == 1 && f.writes == 0);
	c = prepare(&f, &timer);
	*reg(&f, SE0_DFS) = 0xfffffffe;
	sail_check(monaco_clock_se0_enable(&c) == 0);
	c = prepare(&f, &timer);
	f.hold_rcg = true;
	*reg(&f, SE0_CMD) = 1;
	sail_check(monaco_clock_se0_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.writes == 0);
	c = prepare(&f, &timer);
	f.hold_rcg = true;
	sail_check(monaco_clock_se0_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.writes == 2 && first_write(&f, SE0_CBCR) == UINT32_MAX);
	c = prepare(&f, &timer);
	f.ignore_cfg = true;
	sail_check(monaco_clock_se0_enable(&c) == MONACO_CLOCK_STATE);
	sail_check(first_write(&f, SE0_CBCR) == UINT32_MAX);
	c = prepare(&f, &timer);
	f.hold_cbcr = true;
	*reg(&f, SE0_CBCR) = 0x80000000;
	sail_check(monaco_clock_se0_enable(&c) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(f.cbcr_reads == 1 + c.max_poll_reads);
	sail_check(!monaco_clock_write_allowed(SE0_DFS));
	for (uint32_t address = 0xf1115048; address <= 0xf11150f8; address += 4) {
		sail_check(!monaco_clock_write_allowed(address));
	}
	sail_check(monaco_clock_se0_enable(NULL) == MONACO_CLOCK_INVALID);
	printf("SE0 clock: %u injected access failures\n", accesses);
}

int sail_model_test(void)
{
	complete_allowlist();
	allowlist_and_invalid();
	branch_and_rcg();
	pll_and_parent_order();
	injected_failures();
	pll6_profile_and_guards();
	pll6_limits_and_faults();
	se0_clock();
	puts("Monaco clocks: early allowlist, PLL5/6, ordering, guards, bounds and faults passed");
	return 0;
}
