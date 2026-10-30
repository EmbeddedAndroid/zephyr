/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_MD_PUBLICATION_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_MD_PUBLICATION_H_

#include "clock.h"
#include "md_bist.h"

struct md_publication_backend {
	void *context;
	/* Ordered Device writes, limited to what md_publication_write_allowed() accepts. */
	int (*write32)(void *context, uint32_t address, uint32_t value);
};

/* True only for the byte writes that make up the five BA0100xx status words. */
bool md_publication_write_allowed(uint32_t address, uint32_t value);

enum md_publication_error {
	MD_PUBLICATION_INVALID = -3000,
	MD_PUBLICATION_STATE = -3001,
	MD_PUBLICATION_EVENT = -3002,
	MD_PUBLICATION_CALLBACK = -3003,
	MD_PUBLICATION_CLOCK_BINDING = -3004,
};

struct md_publication {
	bool started, publishing;
	void *context;
	/* Ordered Device writes; not retried or read back. */
	int (*write32)(void *context, uint32_t address, uint32_t value);
	const struct monaco_clock *clock;
	struct monaco_clock clock_binding;
	struct monaco_timer_io timer_binding;
	uint32_t timer_frequency, timer_read_budget;
	unsigned int events_completed, invalidations_completed;
	unsigned int writes_attempted, writes_completed;
	enum md_bist_event event;
	uint32_t word, address, value;
	unsigned int byte_index;
	int callback_status, error;
};

/**
 * Bind a publication state to a write callback and the running clock.
 * p must be zeroed and must not move; the clock, timer and context must outlive
 * it. Single use: a second init fails, even after a failed publication.
 *
 * @retval 0 on success.
 * @retval MD_PUBLICATION_STATE p was already initialized.
 * @retval MD_PUBLICATION_INVALID bad arguments or timer not at 19.2 MHz.
 */
int md_publication_init(struct md_publication *p, void *context,
			int (*write32)(void *context, uint32_t address, uint32_t value),
			const struct monaco_clock *clock);

/**
 * Publish the status word for one md_bist event. Use as md_bist_io.publish.
 * Events must arrive in order. Each word is written as four byte-wide writes
 * at ascending addresses. The first failure is sticky; a failed write may
 * already have reached the hardware.
 *
 * @retval 0 on success.
 * @retval MD_PUBLICATION_EVENT or MD_PUBLICATION_STATE out-of-order call.
 * @retval MD_PUBLICATION_CLOCK_BINDING clock or timer changed since init.
 * @retval negative callback error, or MD_PUBLICATION_CALLBACK.
 */
int md_publication_event(void *context, enum md_bist_event event);

/**
 * md_bist_io.invalidate_md_clocks for a monaco_clock, which caches no clock
 * state, so there is nothing to invalidate. Checks the clock binding and that
 * the call comes after the second and fourth events. If monaco_clock gains a
 * cache, this must be replaced.
 *
 * @retval 0 on success.
 * @retval MD_PUBLICATION_STATE called out of order.
 * @retval MD_PUBLICATION_CLOCK_BINDING clock or timer changed since init.
 */
int md_publication_invalidate_stateless_clocks(void *context);

/* True once all five words and both invalidations completed without error. */
bool md_publication_is_complete(const struct md_publication *publication);

#endif
