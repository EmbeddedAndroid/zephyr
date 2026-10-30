/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timer.h"

#define COUNTER_LOW       UINT32_C(0xf1400000)
#define COUNTER_HIGH      UINT32_C(0xf1400004)
#define COUNTER_FREQUENCY UINT32_C(0xf1400010)
#define ACCESS_FREQUENCY  UINT32_C(0xf140c000)
#define XO_FREQUENCY      UINT32_C(19200000)
#define COHERENCE_READS   8U
#define USEC_PER_SECOND   UINT64_C(1000000)

static int read_counter(struct monaco_timer *timer, uint64_t *value)
{
	uint32_t hi, low, after;

	for (unsigned int i = 0; i < COHERENCE_READS; i++) {
		if (timer->io.read32(timer->io.context, COUNTER_HIGH, &hi) ||
		    timer->io.read32(timer->io.context, COUNTER_LOW, &low) ||
		    timer->io.read32(timer->io.context, COUNTER_HIGH, &after)) {
			return MONACO_TIMER_IO;
		}
		if (hi == after) {
			*value = ((uint64_t)hi << 32) | low;
			return 0;
		}
	}
	return MONACO_TIMER_INCOHERENT;
}

static int sample(struct monaco_timer *timer, uint64_t *value)
{
	int rc = read_counter(timer, value);

	if (rc != 0) {
		return rc;
	}
	if (*value < timer->last) {
		return MONACO_TIMER_BACKWARD;
	}
	timer->last = *value;
	return 0;
}

int monaco_timer_init(struct monaco_timer *timer, const struct monaco_timer_io *io,
		      uint32_t read_budget)
{
	uint64_t value;
	int rc;

	if (timer) {
		timer->initialized = false;
	}
	if (!timer || !io || !io->read32 || !io->write32 || !read_budget) {
		return MONACO_TIMER_INVALID;
	}
	*timer = (struct monaco_timer){.io = *io, .read_budget = read_budget};
	if (io->read32(io->context, COUNTER_FREQUENCY, &timer->frequency)) {
		return MONACO_TIMER_IO;
	}
	if (!timer->frequency) {
		if (io->write32(io->context, ACCESS_FREQUENCY, XO_FREQUENCY) ||
		    io->read32(io->context, COUNTER_FREQUENCY, &timer->frequency)) {
			return MONACO_TIMER_IO;
		}
	}
	if (!timer->frequency) {
		return MONACO_TIMER_INVALID;
	}
	rc = read_counter(timer, &timer->origin);
	if (rc != 0) {
		return rc;
	}
	timer->last = timer->origin;
	for (uint32_t i = 0; i < read_budget; i++) {
		rc = sample(timer, &value);
		if (rc != 0) {
			return rc;
		}
		if (value != timer->origin) {
			timer->initialized = true;
			return 0;
		}
	}
	return MONACO_TIMER_STOPPED;
}

int monaco_timer_time_us(void *context, uint64_t *value)
{
	struct monaco_timer *timer = context;
	uint64_t ticks, seconds, fraction;
	int rc;

	if (!timer || !timer->initialized || !value) {
		return MONACO_TIMER_INVALID;
	}
	rc = sample(timer, &ticks);
	if (rc != 0) {
		return rc;
	}
	ticks -= timer->origin;
	seconds = ticks / timer->frequency;
	fraction = (ticks % timer->frequency) * USEC_PER_SECOND / timer->frequency;
	if (seconds > (UINT64_MAX - fraction) / USEC_PER_SECOND) {
		return MONACO_TIMER_RANGE;
	}
	*value = seconds * USEC_PER_SECOND + fraction;
	return 0;
}

int monaco_timer_delay_us(void *context, uint32_t delay)
{
	struct monaco_timer *timer = context;
	uint64_t start, value, target;
	int rc;

	if (!timer || !timer->initialized) {
		return MONACO_TIMER_INVALID;
	}
	if (!delay) {
		return 0;
	}
	/* Both multiplicands are 32-bit; adding this rounding term cannot overflow. */
	target = ((uint64_t)delay * timer->frequency + USEC_PER_SECOND - 1) / USEC_PER_SECOND;
	rc = sample(timer, &start);
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < timer->read_budget; i++) {
		rc = sample(timer, &value);
		if (rc != 0) {
			return rc;
		}
		if (value - start >= target) {
			return 0;
		}
	}
	/* The read budget ran out before the delay elapsed; the counter may still be running. */
	return MONACO_TIMER_BUDGET;
}
