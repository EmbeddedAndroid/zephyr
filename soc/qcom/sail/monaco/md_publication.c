/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "md_publication.h"
#include <zephyr/sys/util.h>

bool md_publication_write_allowed(uint32_t address, uint32_t value)
{
	switch (address) {
	case UINT32_C(0xf0305084):
		return value == 0 || value == 4 || value == 2 || value == 5 || value == 3;
	case UINT32_C(0xf0305088):
		return value == 0;
	case UINT32_C(0xf030508c):
		return value == 1;
	case UINT32_C(0xf0305090):
		return value == 0xba;
	default:
		return false;
	}
}

static int fail(struct md_publication *p, int error)
{
	if (!p->error) {
		p->error = error;
	}
	return p->error;
}

static bool binding_valid(const struct md_publication *p)
{
	const struct monaco_clock *clock = p->clock;
	const struct monaco_clock *saved = &p->clock_binding;
	const struct monaco_timer *timer;

	if (!clock || clock->timer != saved->timer || !clock->timer ||
	    clock->io.context != saved->io.context || clock->io.read32 != saved->io.read32 ||
	    clock->io.write32 != saved->io.write32 ||
	    clock->max_poll_reads != saved->max_poll_reads) {
		return false;
	}
	timer = clock->timer;
	return timer->initialized && timer->frequency == p->timer_frequency &&
	       timer->read_budget == p->timer_read_budget &&
	       timer->io.context == p->timer_binding.context &&
	       timer->io.read32 == p->timer_binding.read32 &&
	       timer->io.write32 == p->timer_binding.write32;
}

int md_publication_init(struct md_publication *p, void *context,
			int (*write32)(void *context, uint32_t address, uint32_t value),
			const struct monaco_clock *clock)
{
	if (!p) {
		return MD_PUBLICATION_INVALID;
	}
	if (p->started) {
		return MD_PUBLICATION_STATE;
	}
	p->started = true;
	if (!write32 || !clock || !clock->io.read32 || !clock->io.write32 || !clock->timer ||
	    !clock->timer->initialized || !clock->timer->io.read32 || !clock->timer->io.write32 ||
	    clock->timer->frequency != 19200000 || !clock->timer->read_budget ||
	    !clock->max_poll_reads) {
		return fail(p, MD_PUBLICATION_INVALID);
	}
	p->context = context;
	p->write32 = write32;
	p->clock = clock;
	p->clock_binding = *clock;
	p->timer_binding = clock->timer->io;
	p->timer_frequency = clock->timer->frequency;
	p->timer_read_budget = clock->timer->read_budget;
	return 0;
}

int md_publication_event(void *context, enum md_bist_event event)
{
	static const uint32_t words[] = {
		UINT32_C(0xba010000), UINT32_C(0xba010004), UINT32_C(0xba010002),
		UINT32_C(0xba010005), UINT32_C(0xba010003),
	};
	struct md_publication *p = context;
	unsigned int index = (unsigned int)event;
	unsigned int required_invalidations;

	if (!p || !p->started) {
		return MD_PUBLICATION_INVALID;
	}
	if (p->error) {
		return p->error;
	}
	if (p->publishing) {
		return fail(p, MD_PUBLICATION_STATE);
	}
	if (index >= ARRAY_SIZE(words) || index != p->events_completed) {
		return fail(p, MD_PUBLICATION_EVENT);
	}
	required_invalidations = index < 2 ? 0U : (index < 4 ? 1U : 2U);
	if (p->invalidations_completed != required_invalidations) {
		return fail(p, MD_PUBLICATION_STATE);
	}
	if (!binding_valid(p)) {
		return fail(p, MD_PUBLICATION_CLOCK_BINDING);
	}
	p->event = event;
	p->word = words[index];
	p->publishing = true;
	for (unsigned int byte = 0; byte < 4; byte++) {
		int rc;

		p->byte_index = byte;
		p->address = UINT32_C(0xf0305084) + 4U * byte;
		p->value = (p->word >> (8U * byte)) & UINT32_C(0xff);
		p->writes_attempted++;
		rc = p->write32(p->context, p->address, p->value);
		p->callback_status = rc;
		if (rc != 0) {
			p->publishing = false;
			return fail(p, rc < 0 ? rc : MD_PUBLICATION_CALLBACK);
		}
		p->writes_completed++;
		if (p->error) {
			p->publishing = false;
			return p->error;
		}
	}
	p->publishing = false;
	p->events_completed++;
	return 0;
}

int md_publication_invalidate_stateless_clocks(void *context)
{
	struct md_publication *p = context;

	if (!p || !p->started) {
		return MD_PUBLICATION_INVALID;
	}
	if (p->error) {
		return p->error;
	}
	if (p->publishing) {
		return fail(p, MD_PUBLICATION_STATE);
	}
	if ((p->invalidations_completed == 0 && p->events_completed != 2) ||
	    (p->invalidations_completed == 1 && p->events_completed != 4) ||
	    p->invalidations_completed >= 2) {
		return fail(p, MD_PUBLICATION_STATE);
	}
	if (!binding_valid(p)) {
		return fail(p, MD_PUBLICATION_CLOCK_BINDING);
	}
	p->invalidations_completed++;
	return 0;
}

bool md_publication_is_complete(const struct md_publication *p)
{
	return p && p->started && !p->publishing && !p->error && !p->callback_status &&
	       p->write32 && p->events_completed == 5 && p->invalidations_completed == 2 &&
	       p->writes_attempted == 20 && p->writes_completed == 20 &&
	       p->event == MD_EVENT_COMPLETE && p->word == UINT32_C(0xba010003) &&
	       p->address == UINT32_C(0xf0305090) && p->value == 0xba && p->byte_index == 3 &&
	       binding_valid(p);
}
