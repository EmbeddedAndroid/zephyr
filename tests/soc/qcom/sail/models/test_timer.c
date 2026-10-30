/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timer.h"

#include "sail_check.h"
#include <stdio.h>
#include <string.h>

struct fake {
	uint64_t ticks;
	uint64_t step;
	uint32_t frequency;
	unsigned int accesses;
	unsigned int writes;
	unsigned int high_reads;
	unsigned int fail_at;
	bool rollover_once;
	bool always_incoherent;
	bool ignore_frequency_write;
};

static int fake_read(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;

	if (++f->accesses == f->fail_at) {
		return -1;
	}
	switch (address) {
	case 0xf1400010:
		*value = f->frequency;
		return 0;
	case 0xf1400000:
		*value = (uint32_t)f->ticks;
		if (f->rollover_once) {
			f->ticks++;
			f->rollover_once = false;
		}
		if (f->always_incoherent) {
			f->ticks += UINT64_C(1) << 32;
		}
		return 0;
	case 0xf1400004:
		*value = (uint32_t)(f->ticks >> 32);
		if (++f->high_reads % 2 == 0) {
			f->ticks += f->step;
		}
		return 0;
	default:
		sail_check(false);
		return -1;
	}
}

static int fake_write(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;

	sail_check(address == 0xf140c000 && value == 19200000);
	if (++f->accesses == f->fail_at) {
		return -1;
	}
	f->writes++;
	if (!f->ignore_frequency_write) {
		f->frequency = value;
	}
	return 0;
}

static struct monaco_timer_io io(struct fake *f)
{
	return (struct monaco_timer_io){f, fake_read, fake_write};
}

static void prepare(struct fake *f, struct monaco_timer *timer, uint32_t budget)
{
	struct monaco_timer_io ops = io(f);

	sail_check(monaco_timer_init(timer, &ops, budget) == 0);
	sail_check(timer->initialized && timer->frequency == f->frequency);
}

int sail_model_test(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_timer_io ops;
	uint64_t value;

	f = (struct fake){.ticks = 100, .step = 20, .frequency = 19200000};
	prepare(&f, &timer, 32);
	sail_check(f.writes == 0);
	f.ticks = timer.origin + 1920000 + 19;
	sail_check(monaco_timer_time_us(&timer, &value) == 0 && value == 100000);

	f = (struct fake){.ticks = 100, .step = 1};
	prepare(&f, &timer, 32);
	sail_check(f.writes == 1 && timer.frequency == 19200000);

	f = (struct fake){.step = 1, .frequency = 24000000};
	prepare(&f, &timer, 32);
	sail_check(f.writes == 0 && timer.frequency == 24000000);
	f.ticks = timer.origin + 24000000;
	sail_check(monaco_timer_time_us(&timer, &value) == 0 && value == 1000000);

	f = (struct fake){.step = 1, .ignore_frequency_write = true};
	ops = io(&f);
	sail_check(monaco_timer_init(&timer, &ops, 4) == MONACO_TIMER_INVALID);
	sail_check(!timer.initialized);

	/* Every access in the fallback initialization path can independently fail. */
	for (unsigned int failure = 1; failure <= 9; failure++) {
		f = (struct fake){.ticks = 100, .step = 1, .fail_at = failure};
		ops = io(&f);
		sail_check(monaco_timer_init(&timer, &ops, 4) == MONACO_TIMER_IO);
		sail_check(!timer.initialized);
	}

	f = (struct fake){
		.ticks = UINT32_MAX, .step = 1, .frequency = 19200000, .rollover_once = true};
	prepare(&f, &timer, 4);
	sail_check(timer.origin == (UINT64_C(1) << 32) + 1);

	f = (struct fake){.frequency = 19200000, .always_incoherent = true};
	ops = io(&f);
	sail_check(monaco_timer_init(&timer, &ops, 4) == MONACO_TIMER_INCOHERENT);
	sail_check(f.accesses == 25 && !timer.initialized);

	f = (struct fake){.ticks = 100, .frequency = 19200000};
	ops = io(&f);
	sail_check(monaco_timer_init(&timer, &ops, 4) == MONACO_TIMER_STOPPED);
	sail_check(f.accesses == 16 && !timer.initialized);

	f = (struct fake){.ticks = 100, .step = 1, .frequency = 19200000};
	prepare(&f, &timer, 20);
	sail_check(monaco_timer_delay_us(&timer, 1) == 0);
	f.step = 0;
	sail_check(monaco_timer_delay_us(&timer, 1) == MONACO_TIMER_BUDGET);
	sail_check(monaco_timer_delay_us(&timer, 0) == 0);
	f.ticks = 0;
	sail_check(monaco_timer_time_us(&timer, &value) == MONACO_TIMER_BACKWARD);
	sail_check(monaco_timer_delay_us(&timer, 1) == MONACO_TIMER_BACKWARD);

	f = (struct fake){.step = 1, .frequency = 19200000};
	prepare(&f, &timer, 19);
	sail_check(monaco_timer_delay_us(&timer, 1) == MONACO_TIMER_BUDGET);

	for (unsigned int failure = 1; failure <= 3; failure++) {
		f = (struct fake){.step = 1, .frequency = 19200000};
		prepare(&f, &timer, 4);
		f.fail_at = f.accesses + failure;
		sail_check(monaco_timer_time_us(&timer, &value) == MONACO_TIMER_IO);
	}

	/* Includes the initial sample and failures after a successful delay poll. */
	for (unsigned int failure = 1; failure <= 9; failure++) {
		f = (struct fake){.step = 1, .frequency = 19200000};
		prepare(&f, &timer, 32);
		f.fail_at = f.accesses + failure;
		sail_check(monaco_timer_delay_us(&timer, 1) == MONACO_TIMER_IO);
		sail_check(f.accesses == f.fail_at && f.writes == 0);
	}

	f = (struct fake){.step = 1, .frequency = 1};
	prepare(&f, &timer, 4);
	f.ticks = UINT64_MAX;
	sail_check(monaco_timer_time_us(&timer, &value) == MONACO_TIMER_RANGE);

	sail_check(monaco_timer_init(&timer, NULL, 1) == MONACO_TIMER_INVALID);
	sail_check(!timer.initialized);
	sail_check(monaco_timer_time_us(&timer, &value) == MONACO_TIMER_INVALID);
	sail_check(monaco_timer_delay_us(&timer, 1) == MONACO_TIMER_INVALID);
	puts("Monaco timer: progress, coherent reads, conversion and failure tests passed");
	return 0;
}
