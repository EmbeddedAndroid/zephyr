/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
/* BIST implementation with a separate MMIO fixture and protocol publisher. */
#include "md_publication.h"
#define sail_model_test retained_md_bist_test_main
#include "test_md_bist.c"
#undef sail_model_test

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const uint32_t expected_words[] = {
	0xba010000, 0xba010004, 0xba010002, 0xba010005, 0xba010003,
};
static const uint32_t expected_bytes[][4] = {
	{0, 0, 1, 0xba}, {4, 0, 1, 0xba}, {2, 0, 1, 0xba}, {5, 0, 1, 0xba}, {3, 0, 1, 0xba},
};

struct trial {
	struct fake model;
	struct md_publication publication;
	struct monaco_timer timer;
	struct monaco_clock clock;
	struct sample stores[20];
	unsigned int attempted, effected, fail_at, invalidate_calls, break_binding_at;
	int fault;
	bool fail_after_effect, reenter;
};

static int clock_read_forbidden(void *context, uint32_t address, uint32_t *value)
{
	(void)context;
	(void)address;
	(void)value;
	sail_check(!"stateless invalidation must not read hardware");
	return -99;
}

static int clock_write_forbidden(void *context, uint32_t address, uint32_t value)
{
	(void)context;
	(void)address;
	(void)value;
	sail_check(!"stateless invalidation must not write hardware");
	return -99;
}

static int publication_write(void *context, uint32_t address, uint32_t value)
{
	struct trial *t = context;
	unsigned int index = t->attempted++;
	bool failing = t->attempted == t->fail_at;

	sail_check(index < 20);
	sail_check(address == UINT32_C(0xf0305084) + (index % 4) * 4);
	sail_check(value == expected_bytes[index / 4][index % 4]);
	sail_check(value <= UINT8_MAX);
	sail_check(!md_bist_write_allowed(address)); /* Not in the BIST write allowlist. */
	if (t->reenter) {
		sail_check(md_publication_event(&t->publication, MD_EVENT_BEGIN) ==
		       MD_PUBLICATION_STATE);
	}
	if (failing && !t->fail_after_effect) {
		return t->fault;
	}
	t->stores[t->effected++] = (struct sample){address, value};
	record(&t->model, 'W', address, value);
	/* External fake MD responds only after the complete phase2 request. */
	if (index == 11) {
		t->model.phase2 = true;
	}
	return failing ? t->fault : 0;
}

static void trial_init(struct trial *t)
{
	*t = (struct trial){.fault = -77};
	initialize(&t->model, HAPPY);
	t->timer = (struct monaco_timer){
		.io = {&t->model, clock_read_forbidden, clock_write_forbidden},
		.frequency = 19200000,
		.read_budget = 2048,
		.initialized = true,
	};
	t->clock = (struct monaco_clock){
		.io = {t, clock_read_forbidden, clock_write_forbidden},
		.timer = &t->timer,
		.max_poll_reads = 2048,
	};
	sail_check(md_publication_init(&t->publication, t, publication_write, &t->clock) == 0);
}

static int bridge_read(void *context, uint32_t address, uint32_t *value)
{
	struct trial *t = context;

	return read32(&t->model, address, value);
}

static int bridge_write(void *context, uint32_t address, uint32_t value)
{
	struct trial *t = context;

	return write32(&t->model, address, value);
}

static int bridge_time(void *context, uint64_t *value)
{
	struct trial *t = context;

	return time_us(&t->model, value);
}

static int bridge_delay(void *context, uint32_t value)
{
	struct trial *t = context;

	return delay_us(&t->model, value);
}

static int bridge_event(void *context, enum md_bist_event event)
{
	struct trial *t = context;

	return md_publication_event(&t->publication, event);
}

static int bridge_invalidate(void *context)
{
	struct trial *t = context;

	if (++t->invalidate_calls == t->break_binding_at) {
		t->clock.max_poll_reads++;
	}
	record(&t->model, 'K', 0, t->invalidate_calls);
	return md_publication_invalidate_stateless_clocks(&t->publication);
}

static int run_proposed(struct trial *t, struct md_bist_diagnostic *d)
{
	struct md_bist_policy p = policy();
	struct md_bist_prerequisites pre = prerequisites();
	struct md_bist_limits limits = {.max_poll_reads = 2048};
	struct md_bist_io io = {t,
				bridge_read,
				bridge_write,
				bridge_time,
				bridge_delay,
				bridge_invalidate,
				bridge_event,
				NULL};

	return md_bist_run(&p, &pre, &io, &limits, d);
}

static void publish_prefix(struct trial *t, unsigned int events)
{
	for (unsigned int e = 0; e < events; e++) {
		if (e == 2 || e == 4) {
			sail_check(md_publication_invalidate_stateless_clocks(&t->publication) ==
				   0);
		}
		sail_check(md_publication_event(&t->publication, (enum md_bist_event)e) == 0);
	}
}

static void test_events(void)
{
	struct trial t;
	struct md_bist_diagnostic d;

	trial_init(&t);
	sail_check(run_proposed(&t, &d) == 0 && d.result == 0);
	sail_check(d.stage == MD_STAGE_COMPLETE && t.publication.events_completed == 5);
	sail_check(t.publication.invalidations_completed == 2 && t.invalidate_calls == 2);
	sail_check(t.attempted == 20 && t.effected == 20 && t.publication.writes_completed == 20);
	sail_check(md_publication_is_complete(&t.publication));
	for (unsigned int e = 0; e < 5; e++) {
		uint32_t assembled = 0;

		for (unsigned int b = 0; b < 4; b++) {
			assembled |= t.stores[4 * e + b].value << (8 * b);
		}
		sail_check(assembled == expected_words[e]);
	}
	/* Order relative to the hardware operation trace. */
	sail_check(strstr(t.model.trace, "W f0305090 000000ba\nR f03050c4"));
	sail_check(strstr(t.model.trace, "W f0305090 000000ba\nR c1ff900c"));
	sail_check(strstr(t.model.trace, "W f1000034 000003ff\nK 1\nW f0305084 00000002"));
	sail_check(strstr(t.model.trace, "D 25000\nW f0305084 00000005"));
	sail_check(strstr(t.model.trace, "K 2\nW f0305084 00000003"));
	sail_check(md_publication_event(&t.publication, MD_EVENT_COMPLETE) == MD_PUBLICATION_EVENT);
	sail_check(t.attempted == 20);

	for (unsigned int failure = 1; failure <= 20; failure++) {
		for (unsigned int after = 0; after < 2; after++) {
			for (unsigned int positive = 0; positive < 2; positive++) {
				trial_init(&t);
				t.fail_at = failure;
				t.fail_after_effect = after != 0;
				t.fault = positive ? 9 : -77;
				sail_check(run_proposed(&t, &d) == MD_BIST_CALLBACK);
				sail_check(d.result == MD_BIST_CALLBACK &&
				       d.stage != MD_STAGE_COMPLETE);
				sail_check(t.publication.error ==
				       (positive ? MD_PUBLICATION_CALLBACK : -77));
				sail_check(t.publication.callback_status == t.fault);
				sail_check(t.attempted == failure &&
					   t.effected == failure - 1 + after);
				sail_check(t.publication.writes_completed == failure - 1);
				sail_check(t.publication.events_completed == (failure - 1) / 4);
				sail_check(
					md_publication_event(&t.publication, MD_EVENT_COMPLETE) ==
					t.publication.error);
				sail_check(md_publication_invalidate_stateless_clocks(
						   &t.publication) == t.publication.error);
				sail_check(md_publication_init(&t.publication, &t,
							       publication_write,
							       &t.clock) == MD_PUBLICATION_STATE);
				sail_check(t.attempted == failure);
			}
		}
	}
	trial_init(&t);
	t.reenter = true;
	sail_check(run_proposed(&t, &d) == MD_BIST_CALLBACK);
	sail_check(t.publication.error == MD_PUBLICATION_STATE && t.attempted == 1);
	sail_check(t.publication.writes_completed == 1 && t.publication.events_completed == 0);
}

static void test_order_and_binding(void)
{
	struct trial t;
	struct md_bist_diagnostic d;
	struct md_publication zero = {0};

	sail_check(md_publication_event(NULL, MD_EVENT_BEGIN) == MD_PUBLICATION_INVALID);
	sail_check(md_publication_event(&zero, MD_EVENT_BEGIN) == MD_PUBLICATION_INVALID);
	sail_check(md_publication_invalidate_stateless_clocks(NULL) == MD_PUBLICATION_INVALID);
	sail_check(md_publication_invalidate_stateless_clocks(&zero) == MD_PUBLICATION_INVALID);
	sail_check(md_publication_init(NULL, NULL, NULL, NULL) == MD_PUBLICATION_INVALID);
	for (unsigned int fault = 0; fault < 12; fault++) {
		const struct monaco_clock *selected;
		int (*writer)(void *, uint32_t, uint32_t) = publication_write;

		trial_init(&t);
		t.publication = (struct md_publication){0};
		selected = &t.clock;
		switch (fault) {
		case 0:
			writer = NULL;
			break;
		case 1:
			selected = NULL;
			break;
		case 2:
			t.clock.io.read32 = NULL;
			break;
		case 3:
			t.clock.io.write32 = NULL;
			break;
		case 4:
			t.clock.timer = NULL;
			break;
		case 5:
			t.clock.max_poll_reads = 0;
			break;
		case 6:
			t.timer.initialized = false;
			break;
		case 7:
			t.timer.frequency = 0;
			break;
		case 8:
			t.timer.frequency++;
			break;
		case 9:
			t.timer.read_budget = 0;
			break;
		case 10:
			t.timer.io.read32 = NULL;
			break;
		case 11:
			t.timer.io.write32 = NULL;
			break;
		default:
			sail_check(false);
		}
		sail_check(md_publication_init(&t.publication, &t, writer, selected) ==
		       MD_PUBLICATION_INVALID);
		sail_check(md_publication_event(&t.publication, MD_EVENT_BEGIN) ==
		       MD_PUBLICATION_INVALID);
		sail_check(md_publication_invalidate_stateless_clocks(&t.publication) ==
		       MD_PUBLICATION_INVALID);
		sail_check(md_publication_init(&t.publication, &t, publication_write, &t.clock) ==
		       MD_PUBLICATION_STATE);
		sail_check(!t.attempted);
	}
	for (unsigned int completed = 0; completed < 5; completed++) {
		for (int e = -1; e < 7; e++) {
			if (e == (int)completed) {
				continue;
			}
			trial_init(&t);
			publish_prefix(&t, completed);
			sail_check(md_publication_event(&t.publication, (enum md_bist_event)e) ==
			       MD_PUBLICATION_EVENT);
			sail_check(t.attempted == completed * 4);
		}
	}
	for (unsigned int bad_position = 0; bad_position < 4; bad_position++) {
		trial_init(&t);
		if (bad_position < 2) {
			publish_prefix(&t, bad_position);
			sail_check(md_publication_invalidate_stateless_clocks(&t.publication) ==
			       MD_PUBLICATION_STATE);
		} else {
			publish_prefix(&t, bad_position == 2 ? 2 : 4);
			sail_check(md_publication_event(&t.publication,
						    bad_position == 2 ? MD_EVENT_PHASE2_REQUEST
								      : MD_EVENT_COMPLETE) ==
			       MD_PUBLICATION_STATE);
		}
	}
	trial_init(&t);
	publish_prefix(&t, 5);
	sail_check(md_publication_invalidate_stateless_clocks(&t.publication) ==
		   MD_PUBLICATION_STATE);
	for (unsigned int invocation = 1; invocation <= 2; invocation++) {
		trial_init(&t);
		t.break_binding_at = invocation;
		sail_check(run_proposed(&t, &d) == MD_BIST_CALLBACK);
		sail_check(d.stage == MD_STAGE_CLOCK_MODEL && d.result == MD_BIST_CALLBACK);
		sail_check(t.publication.error == MD_PUBLICATION_CLOCK_BINDING);
		sail_check(t.attempted == invocation * 8);
	}
	for (unsigned int fault = 0; fault < 11; fault++) {
		trial_init(&t);
		publish_prefix(&t, 2);
		switch (fault) {
		case 0:
			t.clock.io.context = NULL;
			break;
		case 1:
			t.clock.io.read32 = NULL;
			break;
		case 2:
			t.clock.io.write32 = NULL;
			break;
		case 3:
			t.clock.timer = NULL;
			break;
		case 4:
			t.clock.max_poll_reads++;
			break;
		case 5:
			t.timer.initialized = false;
			break;
		case 6:
			t.timer.frequency++;
			break;
		case 7:
			t.timer.read_budget++;
			break;
		case 8:
			t.timer.io.context = NULL;
			break;
		case 9:
			t.timer.io.read32 = NULL;
			break;
		case 10:
			t.timer.io.write32 = NULL;
			break;
		default:
			sail_check(false);
		}
		sail_check(md_publication_invalidate_stateless_clocks(&t.publication) ==
		       MD_PUBLICATION_CLOCK_BINDING);
		sail_check(t.attempted == 8 && t.publication.invalidations_completed == 0);
	}
}

static void test_original_failures(void)
{
	struct trial t;
	struct md_bist_diagnostic d;
	static const enum scenario cases[] = {
		INITIAL_TIMEOUT, PHASE1_TIMEOUT, PHASE2_TIMEOUT,
		RESULT1_STUCK,   RESULT2_STUCK,  ISOLATION_STUCK,
	};
	static const int results[] = {
		MD_BIST_TIMEOUT, MD_BIST_TIMEOUT, MD_BIST_TIMEOUT,
		MD_BIST_TIMEOUT, MD_BIST_TIMEOUT, MD_BIST_ISOLATION,
	};
	static const unsigned int events[] = {0, 1, 3, 2, 4, 2};

	for (unsigned int i = 0; i < ARRAY_COUNT(cases); i++) {
		trial_init(&t);
		t.model.scenario = cases[i];
		sail_check(run_proposed(&t, &d) == results[i] && d.result == results[i]);
		sail_check(t.publication.error == 0 && t.publication.events_completed == events[i]);
		sail_check(t.attempted == events[i] * 4);
		sail_check(d.stage != MD_STAGE_COMPLETE);
	}
	for (unsigned int kind = 0; kind < 4; kind++) {
		trial_init(&t);
		if (kind == 0) {
			t.model.fail_read_address = 0xf1000008;
		}
		if (kind == 1) {
			t.model.fail_write_address = 0xc1ff900c;
		}
		if (kind == 2) {
			t.model.fail_time_call = 1;
		}
		if (kind == 3) {
			t.model.fail_delay_call = 1;
		}
		sail_check(run_proposed(&t, &d) == MD_BIST_IO && d.result == MD_BIST_IO);
		sail_check(!t.publication.error && t.publication.events_completed < 5);
	}
}

static void test_allowlist(void)
{
	for (uint32_t address = UINT32_C(0xf030507c); address <= UINT32_C(0xf0305098); address++) {
		for (uint32_t value = 0; value <= 0x1ff; value++) {
			bool allowed =
				(address == 0xf0305084 && (value == 0 || value == 4 || value == 2 ||
							   value == 5 || value == 3)) ||
				(address == 0xf0305088 && value == 0) ||
				(address == 0xf030508c && value == 1) ||
				(address == 0xf0305090 && value == 0xba);
			sail_check(md_publication_write_allowed(address, value) == allowed);
		}
	}
	const uint32_t forbidden[] = {0xef,       0xaa,       0xab,       0xaa000000,
				      0xab000000, 0xef000000, 0xba010000, UINT32_MAX,
				      0x100,      0x10000,    0x1000000};
	for (unsigned int a = 0; a < 4; a++) {
		for (unsigned int i = 0; i < ARRAY_COUNT(forbidden); i++) {
			sail_check(!md_publication_write_allowed(0xf0305084 + a * 4, forbidden[i]));
		}
	}
	sail_check(!md_publication_write_allowed(0, 0));
	sail_check(!md_publication_write_allowed(UINT32_MAX, 0));
	sail_check(!md_publication_is_complete(NULL));
}

int sail_model_test(void)
{
	sail_check(retained_md_bist_test_main() == 0);
	test_allowlist();
	test_events();
	test_order_and_binding();
	test_original_failures();
	puts("MD publication: exact 20 stores, 80 callback faults, "
	     "real BIST ordering/errors, terminal state and stateless binding passed");
	return 0;
}
