/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2c.h"

#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define SE            UINT32_C(0xf1980000)
struct reg {
	uint32_t address, value;
};
struct fake {
	struct reg regs[80], writes[8192];
	unsigned int count, write_count, accesses, fail_at, high_reads, polls;
	unsigned int done_after, cancels, aborts;
	uint64_t ticks, step;
	uint8_t sent[4096], incoming[4096];
	size_t sent_count, received;
	uint32_t fault_irq, override_rx;
	bool active, never_done, cancel_fails, abort_fails, timer_failure;
};

static uint32_t *reg(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->count; i++) {
		if (f->regs[i].address == address) {
			return &f->regs[i].value;
		}
	}
	sail_check(f->count < ARRAY_SIZE(f->regs));
	f->regs[f->count].address = address;
	return &f->regs[f->count++].value;
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
			f->ticks += f->step;
		}
		return 0;
	}
	if (address == SE + 0x40) {
		*value = f->active ? 1U : 0U;
		return 0;
	}
	if (address == SE + 0x610 && f->active) {
		f->polls++;
		if (f->fault_irq) {
			*reg(f, address) |= f->fault_irq;
			if (f->fault_irq & 1U) {
				f->active = false;
			}
		} else if (!f->never_done && f->sent_count == *reg(f, SE + 0x26c) &&
			   *reg(f, SE + 0x270) - f->received <=
				   4U * ((*reg(f, SE + 0xe28) >> 16) & 0x3fU) &&
			   f->polls >= f->done_after) {
			*reg(f, address) |= 1;
			f->active = false;
		}
	}
	if (address == SE + 0x804) {
		if (f->override_rx && f->active) {
			*value = f->override_rx;
			return 0;
		}
		size_t size = *reg(f, SE + 0x270);
		size_t remaining = size - f->received;

		if (f->sent_count != *reg(f, SE + 0x26c) || !remaining) {
			*value = 0;
			return 0;
		}
		uint32_t depth = (*reg(f, SE + 0xe28) >> 16) & 0x3fU;
		uint32_t words = (uint32_t)((remaining + 3) / 4);

		if (words > depth) {
			*value = depth;
		} else {
			uint32_t last = remaining % 4 ? (uint32_t)(remaining % 4) : 4;

			*value = 0x80000000U | last << 28 | words;
		}
		return 0;
	}
	if (address == SE + 0x780) {
		*value = 0;
		for (unsigned int byte = 0; byte < 4 && f->received < *reg(f, SE + 0x270); byte++) {
			*value |= (uint32_t)f->incoming[f->received++] << (byte * 8);
		}
		return 0;
	}
	*value = *reg(f, address);
	return 0;
}

static int fake_write(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;

	sail_check(monaco_i2c_write_allowed(address));
	if (fail(f, address)) {
		return -1;
	}
	sail_check(f->write_count < ARRAY_SIZE(f->writes));
	f->writes[f->write_count++] = (struct reg){address, value};
	if (address == SE + 0x618) {
		*reg(f, SE + 0x610) &= ~value;
		return 0;
	}
	if (address == SE + 0x600) {
		sail_check(!f->active);
		sail_check(*reg(f, SE + 0x610) == 0);
		f->active = true;
		f->sent_count = f->received = 0;
		f->polls = 0;
	}
	if (address == SE + 0x700) {
		sail_check(f->active);
		for (unsigned int byte = 0; byte < 4 && f->sent_count < *reg(f, SE + 0x26c);
		     byte++) {
			f->sent[f->sent_count++] = (uint8_t)value;
			value >>= 8;
		}
		return 0;
	}
	if (address == SE + 0x604) {
		sail_check(value == 4 || value == 2);
		if (value == 4) {
			f->cancels++;
		} else {
			f->aborts++;
		}
		if ((value == 4 && !f->cancel_fails) || (value == 2 && !f->abort_fails)) {
			f->active = false;
			f->received = *reg(f, SE + 0x270);
			*reg(f, SE + 0x610) = value == 4 ? 0x10U : 0x20U;
		}
	}
	*reg(f, address) = value;
	return 0;
}

static struct monaco_timer_io prepare(struct fake *f, struct monaco_timer *timer)
{
	*f = (struct fake){.step = 20, .done_after = 1};
	*reg(f, SE + 0x68) = 0x303;
	*reg(f, SE + 0xe24) = 0x20020000;
	*reg(f, SE + 0xe28) = 0x20030000;
	*reg(f, SE + 0x254) = 0xdeadbeef;
	struct monaco_timer_io io = {f, fake_read, fake_write};

	sail_check(monaco_timer_init(timer, &io, 8) == 0);
	f->accesses = f->write_count = 0;
	for (unsigned int i = 0; i < sizeof(f->incoming); i++) {
		f->incoming[i] = (uint8_t)(i * 53U + 7U);
	}
	return io;
}

static void ready(struct fake *f, struct monaco_timer *timer, struct monaco_i2c *i2c)
{
	struct monaco_timer_io io = prepare(f, timer);

	sail_check(monaco_i2c_init(i2c, &io, timer, 1000) == 0);
	f->accesses = f->write_count = 0;
}

static void check_initialization(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_i2c i2c;
	struct monaco_timer_io io = prepare(&f, &timer);

	sail_check(monaco_i2c_init(&i2c, &io, &timer, 1000) == 0);
	const struct reg expected[] = {
		{0xf0100000, 0x207},      {0xf0101000, 0x207},   {SE + 0x24, 0},
		{SE + 0x80, 0},           {SE + 0x28, 0x7f},     {SE + 0x24, 0x7f},
		{SE + 0x258, 0},          {SE + 0x814, 1},       {SE + 0x20, 1},
		{SE + 0x28, 0x27f},       {SE + 0x28, 0x37f},    {SE + 0x2000, 1},
		{SE + 0x28, 0x17f},       {SE + 0x28, 0x7f},     {SE + 0x2004, 0},
		{SE + 0x2008, 0},         {SE + 0x7c, 0},        {SE + 0x254, 0},
		{SE + 0x260, 0x7f8fe},    {SE + 0x264, 0xffefe}, {SE + 0x284, 0x7f8fe},
		{SE + 0x288, 0xffefe},    {SE + 0x48, 0x11},     {SE + 0x278, 0x302412},
		{SE + 0x618, UINT32_MAX},
	};
	sail_check(f.write_count == ARRAY_SIZE(expected));
	sail_check(*reg(&f, SE + 0x254) == 0);
	for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
		sail_check(f.writes[i].address == expected[i].address);
		sail_check(f.writes[i].value == expected[i].value);
	}
	unsigned int accesses = f.accesses;

	for (unsigned int failure = 1; failure <= accesses; failure++) {
		io = prepare(&f, &timer);
		f.fail_at = failure;
		sail_check(monaco_i2c_init(&i2c, &io, &timer, 1000) == MONACO_I2C_IO);
		sail_check(!i2c.initialized && f.accesses == failure);
	}
	const struct reg bad[] = {
		{SE + 0x68, 0},           {SE + 0x68, 0x203},       {SE + 0xe24, 0x10020000},
		{SE + 0xe24, 0x20000000}, {SE + 0xe28, 0x20020000}, {SE + 0xe28, 0x10030000},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(bad); i++) {
		io = prepare(&f, &timer);
		*reg(&f, bad[i].address) = bad[i].value;
		sail_check(monaco_i2c_init(&i2c, &io, &timer, 1000) != 0);
		sail_check(!i2c.initialized && f.write_count == 0);
	}
	printf("I2C initialization: %u injected access failures\n", accesses);
}

static void transfers(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_i2c i2c;
	uint8_t tx[4096], rx[4096];
	const size_t sizes[] = {1, 2, 3, 4, 5, 8, 9, 12, 13, 65, 4096};

	for (size_t i = 0; i < sizeof(tx); i++) {
		tx[i] = (uint8_t)(i + 0x60U);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (unsigned int receive = 0; receive < 2; receive++) {
			ready(&f, &timer, &i2c);
			memset(rx, 0xa5, sizeof(rx));
			size_t rx_size = receive ? sizes[i] : 0;

			sail_check(monaco_i2c_transfer(&i2c, 0x28, tx, sizes[i], rx, rx_size,
						       20000) == 0);
			sail_check(f.sent_count == sizes[i] && memcmp(f.sent, tx, sizes[i]) == 0);
			sail_check(i2c.diagnostic.received == rx_size);
			sail_check(memcmp(rx, f.incoming, rx_size) == 0);
			sail_check(*reg(&f, SE + 0x600) ==
				   ((receive ? 3U : 1U) << 27 | 0x28U << 9));
			sail_check(!i2c.busy && i2c.initialized && !f.active && !f.cancels &&
			       !f.aborts);
			if (rx_size < sizeof(rx)) {
				sail_check(rx[rx_size] == 0xa5);
			}
		}
	}
	ready(&f, &timer, &i2c);
	*reg(&f, SE + 0x610) = 0x1401;
	sail_check(monaco_i2c_probe(&i2c, 0x12, 1000) == 0);
	sail_check(*reg(&f, SE + 0x600) == (4U << 27 | 0x12U << 9));
	sail_check(!f.sent_count && !i2c.diagnostic.received && !f.active);

	ready(&f, &timer, &i2c);
	sail_check(monaco_i2c_transfer(&i2c, 0x29, tx, 3, rx, 2, 1000) == 0);
	unsigned int accesses = f.accesses;

	for (unsigned int failure = 1; failure <= accesses; failure++) {
		ready(&f, &timer, &i2c);
		f.fail_at = failure;
		int result = monaco_i2c_transfer(&i2c, 0x29, tx, 3, rx, 2, 1000);

		sail_check(result == (f.timer_failure ? MONACO_I2C_TIMEBASE : MONACO_I2C_IO));
		sail_check(!i2c.busy && i2c.diagnostic.result == result);
	}
	printf("I2C transfer: %u injected access failures\n", accesses);
}

static void errors_and_recovery(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_i2c i2c;
	uint8_t tx = 0x66, rx[2];
	const struct {
		uint32_t irq;
		int result;
	} cases[] = {
		{0x401, MONACO_I2C_NACK},         {0x1001, MONACO_I2C_PROTOCOL},
		{0x2001, MONACO_I2C_ARBITRATION}, {0x30000001, MONACO_I2C_FIFO},
		{0x03000001, MONACO_I2C_FIFO},    {3, MONACO_I2C_COMMAND},
		{5, MONACO_I2C_COMMAND},          {9, MONACO_I2C_COMMAND},
		{0x3401, MONACO_I2C_ARBITRATION}, {0x1401, MONACO_I2C_NACK},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		ready(&f, &timer, &i2c);
		f.fault_irq = cases[i].irq;
		sail_check(monaco_i2c_probe(&i2c, 0x28, 1000) == cases[i].result);
		sail_check(i2c.initialized && !i2c.busy && !i2c.diagnostic.cleanup_result);
	}
	for (unsigned int mode = 0; mode < 3; mode++) {
		ready(&f, &timer, &i2c);
		f.never_done = true;
		f.cancel_fails = mode != 0;
		f.abort_fails = mode == 2;
		sail_check(monaco_i2c_transfer(&i2c, 0x28, &tx, 1, rx, 2, 100) ==
			   MONACO_I2C_TIMEOUT);
		sail_check(!i2c.busy && f.cancels == 1 && f.aborts == (mode ? 1U : 0U));
		sail_check(i2c.initialized == (mode != 2));
		sail_check((i2c.diagnostic.cleanup_result == 0) == (mode != 2));
		if (mode == 2) {
			sail_check(monaco_i2c_probe(&i2c, 0x28, 1000) == MONACO_I2C_STATE);
		}
	}
	ready(&f, &timer, &i2c);
	f.never_done = true;
	i2c.max_poll_reads = 3;
	sail_check(monaco_i2c_probe(&i2c, 0x28, 10000) == MONACO_I2C_TIMEOUT);
	sail_check(i2c.diagnostic.polls == 3 && f.cancels == 1);

	ready(&f, &timer, &i2c);
	f.step = 0;
	f.never_done = true;
	i2c.max_poll_reads = 3;
	sail_check(monaco_i2c_probe(&i2c, 0x28, 1000) == MONACO_I2C_TIMEOUT);
	sail_check(i2c.diagnostic.polls == 3);

	const uint32_t bad_rx[] = {0x80000000, 0x80000001, 0xd0000001, 4, 0xc0000001};

	for (unsigned int i = 0; i < ARRAY_SIZE(bad_rx); i++) {
		ready(&f, &timer, &i2c);
		f.override_rx = bad_rx[i];
		sail_check(monaco_i2c_transfer(&i2c, 0x28, &tx, 1, rx, 2, 1000) == MONACO_I2C_FIFO);
		sail_check(!i2c.busy && f.polls && f.sent_count == 1);
		sail_check(i2c.diagnostic.address == SE + 0x804);
	}
	ready(&f, &timer, &i2c);
	f.fault_irq = 1;
	sail_check(monaco_i2c_transfer(&i2c, 0x28, &tx, 1, rx, 2, 1000) == MONACO_I2C_SHORT);
	sail_check(f.sent_count == 0);
}

static void invalid_inputs(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_i2c i2c;
	uint8_t tx = 0, rx;

	ready(&f, &timer, &i2c);
	sail_check(monaco_i2c_probe(NULL, 0x28, 1000) == MONACO_I2C_INVALID);
	sail_check(monaco_i2c_probe(&i2c, 0x80, 1000) == MONACO_I2C_INVALID);
	sail_check(monaco_i2c_probe(&i2c, 0x28, 3) == MONACO_I2C_INVALID);
	sail_check(monaco_i2c_transfer(&i2c, 0x28, NULL, 1, &rx, 1, 1000) == MONACO_I2C_INVALID);
	sail_check(monaco_i2c_transfer(&i2c, 0x28, &tx, 1, NULL, 1, 1000) == MONACO_I2C_INVALID);
	sail_check(monaco_i2c_transfer(&i2c, 0x28, &tx, 0, &rx, 1, 1000) == MONACO_I2C_INVALID);
	sail_check(monaco_i2c_transfer(&i2c, 0x28, &tx, 4097, &rx, 1, 1000) == MONACO_I2C_INVALID);
	i2c.busy = true;
	sail_check(monaco_i2c_probe(&i2c, 0x28, 1000) == MONACO_I2C_STATE);
	sail_check(f.accesses == 0 && f.write_count == 0);
	const uint32_t forbidden[] = {0xf0305084,  0xf0305088, 0xf030508c, 0xf0305090,  0xf100000c,
				      0xf1000520,  0xf1000524, 0xc1ffc000, SE + 0x1000, SE + 0x1004,
				      SE + 0x1010, SE + 0x100, 0xf0102000};
	for (unsigned int i = 0; i < ARRAY_SIZE(forbidden); i++) {
		sail_check(!monaco_i2c_write_allowed(forbidden[i]));
	}
}

int sail_model_test(void)
{
	check_initialization();
	transfers();
	errors_and_recovery();
	invalid_inputs();
	puts("I2C controller tests passed");
	return 0;
}
