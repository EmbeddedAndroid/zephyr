/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "md_bist.h"
#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>


enum scenario {
	HAPPY,
	INITIAL_TIMEOUT,
	PHASE1_TIMEOUT,
	PHASE2_TIMEOUT,
	RESULT1_STUCK,
	RESULT2_STUCK,
	ISOLATION_STUCK,
	CLOCKS_OFF_STUCK,
	CLOCKS_ON_STUCK,
	CLOCKS_OFF_EARLY_EXIT
};

struct cell {
	uint32_t address, value;
};
struct sample {
	uint32_t address, value;
};
struct fake {
	struct cell cells[20];
	size_t count;
	enum scenario scenario;
	uint64_t now;
	unsigned int reads, writes, ready_reads, invalidations, events, delays;
	bool phase2, freeze, regress, bad_delay;
	unsigned int fail_invalidation;
	unsigned int time_calls, fail_time_call, fail_delay_call;
	int fail_event;
	uint32_t fail_read_address, fail_write_address;
	/* Read scripts are fixed in advance and do not react to driver writes. */
	struct sample off_samples[800], on_samples[520];
	size_t off_count, off_used, on_count, on_used;
	bool off_active, on_active;
	uint32_t isolated[4];
	size_t isolation_count, isolation_used;
	uint32_t poll_address, poll_samples[4];
	size_t poll_count, poll_used;
	uint64_t clock_samples[12];
	size_t clock_count, clock_used;
	char trace[65536];
	size_t used;
};

static void record(struct fake *f, char op, uint32_t address, uint32_t value)
{
	int n;

	sail_check(f->used + 40 < sizeof(f->trace));
	if (op == 'R' || op == 'W') {
		n = snprintf(f->trace + f->used, sizeof(f->trace) - f->used, "%c %08x %08x\n", op,
			     address, value);
	} else {
		n = snprintf(f->trace + f->used, sizeof(f->trace) - f->used, "%c %u\n", op, value);
	}
	sail_check(n > 0);
	f->used += (size_t)n;
}

static struct cell *cell(struct fake *f, uint32_t address)
{
	for (size_t n = 0; n < f->count; n++) {
		if (f->cells[n].address == address) {
			return &f->cells[n];
		}
	}
	sail_check(f->count < ARRAY_SIZE(f->cells));
	f->cells[f->count] = (struct cell){address, 0};
	return &f->cells[f->count++];
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;

	f->reads++;
	if (address == f->fail_read_address) {
		return -1;
	}
	*value = cell(f, address)->value;
	if (address == 0xf1000008 && f->scenario == INITIAL_TIMEOUT) {
		*value = 8;
	}
	if (address == 0xf03050c4) {
		f->ready_reads++;
		*value = f->phase2 ? 5 : 1;
		if (f->scenario == PHASE1_TIMEOUT) {
			*value = 0;
		}
		if (f->scenario == PHASE2_TIMEOUT && f->phase2) {
			*value = 1;
		}
	}
	if (address == 0xf111101c) {
		f->off_active = false;
	}
	if (address == 0xf1000008 && f->isolation_used < f->isolation_count) {
		*value = f->isolated[f->isolation_used++];
	}
	if (address == f->poll_address && f->poll_used < f->poll_count) {
		*value = f->poll_samples[f->poll_used++];
	}
	if (f->off_active &&
	    (address == 0xf1110700 || address == 0xf1111700 || address == 0xf111d020)) {
		sail_check(f->off_used < f->off_count);
		sail_check(f->off_samples[f->off_used].address == address);
		*value = f->off_samples[f->off_used++].value;
	}
	if (f->on_active && (address == 0xf1110700 || address == 0xf1111700)) {
		sail_check(f->on_used < f->on_count);
		sail_check(f->on_samples[f->on_used].address == address);
		*value = f->on_samples[f->on_used++].value;
	}
	record(f, 'R', address, *value);
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	/* Checked on every success and failure trace. */
	sail_check(md_bist_write_allowed(address));
	if (address == f->fail_write_address) {
		return -1;
	}
	f->writes++;
	record(f, 'W', address, value);
	if ((address == 0xc1ff900c && f->scenario == RESULT1_STUCK) ||
	    (address == 0xc1ffa01c && f->scenario == RESULT2_STUCK)) {
		return 0;
	}
	cell(f, address)->value = value;
	if (address == 0xf111d020 && f->off_count) {
		f->off_active = true;
	}
	if (address == 0xf1110700 && (value & 1) && f->on_count) {
		f->on_active = true;
	}
	if (address == 0xf1000034) {
		f->on_active = false;
	}
	if (address == 0xf1000000) {
		cell(f, 0xf1000008)->value =
			(f->scenario != ISOLATION_STUCK && (value & 2)) ? 8 : 0;
	}
	if ((address == 0xf1111700 || address == 0xf1110700) && f->scenario == CLOCKS_ON_STUCK &&
	    (value & 1)) {
		cell(f, address)->value &= ~UINT32_C(0x80000000);
	}
	return 0;
}

static int time_us(void *context, uint64_t *value)
{
	struct fake *f = context;

	if (++f->time_calls == f->fail_time_call) {
		return -1;
	}
	if (f->clock_used < f->clock_count) {
		f->now = f->clock_samples[f->clock_used++];
	} else if (f->regress && f->now) {
		f->now--;
	} else if (!f->freeze) {
		f->now += 1000;
	}
	*value = f->now;
	return 0;
}

static int delay_us(void *context, uint32_t delay)
{
	struct fake *f = context;

	f->delays++;
	record(f, 'D', 0, delay);
	if (f->delays == f->fail_delay_call) {
		return -1;
	}
	if (!f->freeze && !f->bad_delay) {
		f->now += delay;
	}
	return 0;
}

static int invalidate(void *context)
{
	struct fake *f = context;

	f->invalidations++;
	record(f, 'K', 0, f->invalidations);
	/* Readiness changes only through the script, never through the protocol. */
	f->phase2 = true;
	return f->invalidations == f->fail_invalidation ? -1 : 0;
}

static int observe(void *context, enum md_bist_event event)
{
	struct fake *f = context;

	f->events++;
	record(f, 'E', 0, (unsigned int)event);
	return (int)event == f->fail_event ? -1 : 0;
}

/* Synthetic protocol sink; hardware byte publication is tested separately. */
static int publish(void *context, enum md_bist_event event)
{
	(void)context;
	(void)event;
	return 0;
}

static struct md_bist_policy policy(void)
{
	return (struct md_bist_policy){.snapshot_valid = true,
				       .md_presence_known = true,
				       .md_present = true,
				       .warm_cookie_valid = true,
				       .warm_cookie = 3,
				       .md_boot_word = 0xcb000000,
				       .mode = 1,
				       .core = 0};
}

static struct md_bist_prerequisites prerequisites(void)
{
	return (struct md_bist_prerequisites){.access_contract =
						      MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE,
					      .clocks_initialized = true,
					      .timer_validated = true,
					      .md_clock_model_adapter_validated = true};
}

static void initialize(struct fake *f, enum scenario scenario)
{
	*f = (struct fake){.scenario = scenario, .fail_event = -1};
	cell(f, 0xf1111700)->value = 0x80000001;
	cell(f, 0xf1110700)->value = 0x80000001;
	cell(f, 0xf111101c)->value = 0x10;
	cell(f, 0xf1110050)->value = 0x20;
	cell(f, 0xf0301004)->value = 0x80;
	cell(f, 0xf0305014)->value = 0xa7;
	cell(f, 0xf0305010)->value = 0xff;
	if (scenario == CLOCKS_OFF_STUCK) {
		cell(f, 0xf111d020)->value = 0x80000000;
	}
	if (scenario == CLOCKS_OFF_EARLY_EXIT) {
		cell(f, 0xf1111700)->value = 1;
	}
}

static int run(struct fake *f, struct md_bist_diagnostic *d)
{
	struct md_bist_policy p = policy();
	struct md_bist_prerequisites pre = prerequisites();
	struct md_bist_limits limits = {.max_poll_reads = 2048};
	struct md_bist_io io = {f,        read32,     write32, time_us,
				delay_us, invalidate, publish, observe};
	return md_bist_run(&p, &pre, &io, &limits, d);
}

static void fixture(const char *path, const char *actual)
{
	char expected[65536];
	FILE *fp = fopen(path, "rb");

	sail_check(fp);
	size_t n = fread(expected, 1, sizeof(expected) - 1, fp);

	sail_check(!ferror(fp) && feof(fp));
	fclose(fp);
	expected[n] = 0;
	if (strcmp(actual, expected)) {
		fprintf(stderr, "Trace differs from %s\nACTUAL:\n%s", path, actual);
		sail_check(0);
	}
}

static void add_sample(struct sample *samples, size_t *count, uint32_t address, uint32_t value)
{
	samples[(*count)++] = (struct sample){address, value};
}

static void add_final_clock_off_samples(struct fake *f, uint32_t b, uint32_t a)
{
	add_sample(f->off_samples, &f->off_count, 0xf1110700, b);
	if (!b) {
		add_sample(f->off_samples, &f->off_count, 0xf1111700, a);
		if (!a) {
			add_sample(f->off_samples, &f->off_count, 0xf111d020, 0);
		}
	}
}

/* Expected read sequences at the 249/250/251 poll count boundaries. Below
 * 250 a transition of either B or D is sufficient; from 250 both are checked
 * again, short-circuiting in the order B, D[, M].
 */
static void clock_boundaries(struct fake *f, struct md_bist_diagnostic *d)
{
	const uint32_t high = UINT32_C(0x80000000);

	for (unsigned int initial = 0; initial <= 1; initial++) {
		for (unsigned int final = 249; final <= 251; final++) {
			for (unsigned int transition = 0; transition < 3; transition++) {
				initialize(f, HAPPY);
				if (initial) {
					const uint32_t values[] = {0, 0, 0, 8};

					memcpy(f->isolated, values, sizeof(values));
					f->isolation_count = 4;
				}
				unsigned int iterations = final - initial;
				uint32_t b = transition == 2 ? high : 0;
				uint32_t a = transition == 1 ? high : 0;

				for (unsigned int n = 0; n <= iterations; n++) {
					add_sample(f->off_samples, &f->off_count, 0xf1110700,
						   n == iterations ? b : high);
					add_sample(f->off_samples, &f->off_count, 0xf1111700,
						   n == iterations ? a : high);
					add_sample(f->off_samples, &f->off_count, 0xf111d020,
						   n == iterations ? 0 : high);
				}
				if (final >= 250) {
					add_final_clock_off_samples(f, b, a);
				}
				int expected = final >= 250 && transition ? MD_BIST_CLOCK_STATE : 0;

				sail_check(run(f, d) == expected);
				sail_check(f->off_used == f->off_count);
				if (expected) {
					sail_check(d->stage == MD_STAGE_CLOCKS_OFF);
				}
			}
		}
	}
	for (unsigned int final = 249; final <= 251; final++) {
		for (unsigned int transition = 0; transition < 3; transition++) {
			initialize(f, HAPPY);
			uint32_t b = transition == 2 ? 0 : high;
			uint32_t a = transition == 1 ? 0 : high;

			for (unsigned int n = 0; n <= final; n++) {
				add_sample(f->on_samples, &f->on_count, 0xf1110700,
					   n == final ? b : 0);
				add_sample(f->on_samples, &f->on_count, 0xf1111700,
					   n == final ? a : 0);
			}
			if (final >= 250) {
				add_sample(f->on_samples, &f->on_count, 0xf1110700, b);
				if (b) {
					add_sample(f->on_samples, &f->on_count, 0xf1111700, a);
				}
			}
			int expected = final >= 250 && transition ? MD_BIST_CLOCK_STATE : 0;

			sail_check(run(f, d) == expected);
			sail_check(f->on_used == f->on_count);
			if (expected) {
				sail_check(d->stage == MD_STAGE_CLOCKS_ON);
			}
		}
	}
}

static void timer_boundaries(struct fake *f, struct md_bist_diagnostic *d)
{
	for (unsigned int result = 0; result <= 1; result++) {
		for (unsigned int after = 0; after <= 1; after++) {
			initialize(f, HAPPY);
			f->poll_address = result ? 0xc1ff900c : 0xf1000008;
			if (result) {
				/* Includes the preceding RMW read, then the poll. */
				f->poll_samples[0] = 0;
				f->poll_samples[1] = 0;
				f->poll_samples[2] = 1;
				f->poll_count = 3;
				f->clock_samples[0] = 0; /* initial isolation poll start */
				f->clock_samples[1] = 0; /* phase1-ready poll start */
				f->clock_samples[2] = 0; /* result readback poll start */
				f->clock_samples[3] = 10000 + after;
				f->clock_count = 4;
			} else {
				f->poll_samples[0] = 8;
				f->poll_samples[1] = 0;
				f->poll_count = 2;
				f->clock_samples[0] = 0;
				f->clock_samples[1] = 1000000 + after;
				f->clock_count = 2;
			}
			sail_check(run(f, d) == (after ? MD_BIST_TIMEOUT : 0));
			sail_check(f->poll_used == f->poll_count - after);
			sail_check(f->clock_used == f->clock_count);
			if (after) {
				sail_check(d->stage == (result ? MD_STAGE_PHASE1_RESULT
							   : MD_STAGE_INITIAL_ISOLATION));
			}
		}
	}
}

int sail_model_test(void)
{
	static struct fake f;
	struct md_bist_diagnostic d;
	struct md_bist_policy p;

	initialize(&f, HAPPY);
	sail_check(run(&f, &d) == MD_BIST_OK && d.stage == MD_STAGE_COMPLETE);
	sail_check(f.invalidations == 2 && f.events == 5);
	fixture(SAIL_TEST_FIXTURE_DIR "/active-mode1.trace", f.trace);
	sail_check(cell(&f, 0xf0305010)->value == 0xf0);
	sail_check(cell(&f, 0xf0305014)->value == 0xa4);
	sail_check(cell(&f, 0xf03050c8)->value == 5);

	const struct {
		enum scenario scenario;
		int result;
		enum md_bist_stage stage;
	} cases[] = {
		{INITIAL_TIMEOUT, MD_BIST_TIMEOUT, MD_STAGE_INITIAL_ISOLATION},
		{PHASE1_TIMEOUT, MD_BIST_TIMEOUT, MD_STAGE_PHASE1_READY},
		{PHASE2_TIMEOUT, MD_BIST_TIMEOUT, MD_STAGE_PHASE2_READY},
		{RESULT1_STUCK, MD_BIST_TIMEOUT, MD_STAGE_PHASE1_RESULT},
		{RESULT2_STUCK, MD_BIST_TIMEOUT, MD_STAGE_PHASE2_RESULT},
		{ISOLATION_STUCK, MD_BIST_ISOLATION, MD_STAGE_ISOLATION_ENTRY},
		{CLOCKS_OFF_STUCK, MD_BIST_CLOCK_STATE, MD_STAGE_CLOCKS_OFF},
		{CLOCKS_ON_STUCK, MD_BIST_CLOCK_STATE, MD_STAGE_CLOCKS_ON},
	};
	for (size_t n = 0; n < ARRAY_SIZE(cases); n++) {
		initialize(&f, cases[n].scenario);
		sail_check(run(&f, &d) == cases[n].result && d.stage == cases[n].stage);
		sail_check(!strstr(f.trace, "E 4\n"));
		sail_check(cell(&f, 0xf03050c8)->value != 5);
		if (cases[n].scenario == INITIAL_TIMEOUT || cases[n].scenario == PHASE1_TIMEOUT) {
			sail_check(f.writes == 0);
		}
		if (cases[n].scenario == PHASE2_TIMEOUT) {
			sail_check(!strstr(f.trace, "D 25000\n"));
		}
	}
	initialize(&f, CLOCKS_OFF_EARLY_EXIT);
	sail_check(run(&f, &d) == 0); /* Clocks already off: early exit with success. */
	initialize(&f, INITIAL_TIMEOUT);
	f.freeze = true;
	sail_check(run(&f, &d) == MD_BIST_TIMEBASE && f.reads == 2048 && !f.writes);
	initialize(&f, INITIAL_TIMEOUT);
	f.regress = true;
	sail_check(run(&f, &d) == MD_BIST_TIMEBASE && !f.writes);
	initialize(&f, HAPPY);
	f.fail_read_address = 0xf0305014;
	sail_check(run(&f, &d) == MD_BIST_IO && d.address == 0xf0305014);
	initialize(&f, HAPPY);
	f.fail_write_address = 0xf111101c;
	sail_check(run(&f, &d) == MD_BIST_IO && d.address == 0xf111101c);
	for (unsigned int i = 1; i <= 2; i++) {
		initialize(&f, HAPPY);
		f.fail_invalidation = i;
		sail_check(run(&f, &d) == MD_BIST_CALLBACK && !strstr(f.trace, "E 4\n"));
	}
	initialize(&f, HAPPY);
	f.fail_event = MD_EVENT_PHASE1_READY;
	sail_check(run(&f, &d) == MD_BIST_CALLBACK && !f.writes);
	initialize(&f, HAPPY);
	f.fail_time_call = 1;
	sail_check(run(&f, &d) == MD_BIST_IO && !f.reads && !f.writes);
	initialize(&f, HAPPY);
	f.fail_delay_call = 1;
	sail_check(run(&f, &d) == MD_BIST_IO && d.stage == MD_STAGE_CONTROL_SEQUENCE);
	initialize(&f, HAPPY);
	f.bad_delay = true;
	sail_check(run(&f, &d) == MD_BIST_TIMEBASE && d.stage == MD_STAGE_PHASE2_READY);
	sail_check(!strstr(f.trace, "E 3\n") && !strstr(f.trace, "E 4\n"));

	/* After the one-delay path, isolation uses the second sample. The immediate
	 * path takes a second, diagnostic read but branches on the first.
	 */
	const uint32_t isolation_cases[][4] = {{0, 0, 8, 0}, {0, 0, 0, 8}, {0, 8, 0, 0}};

	for (size_t n = 0; n < 3; n++) {
		initialize(&f, HAPPY);
		memcpy(f.isolated, isolation_cases[n], sizeof(f.isolated));
		f.isolation_count = n == 2 ? 3 : 4;
		sail_check(run(&f, &d) == (n == 0 ? MD_BIST_ISOLATION : 0));
		sail_check(f.isolation_used == f.isolation_count);
	}
	clock_boundaries(&f, &d);
	timer_boundaries(&f, &d);

	/* Guard precedence and unsupported paths perform no IO. */
	p = policy();
	p.warm_cookie = 0xf;
	p.md_boot_word = 0xfe111111;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_SKIP_WARM);
	p.warm_cookie = 3;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_SKIP_EDL);
	p = policy();
	p.skip = true;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_SKIP_POLICY);
	for (unsigned int m = 0; m < 4; m++) {
		p = policy();
		p.mode = (uint8_t)m;
		sail_check(md_bist_assess_policy(&p) == (m == 1   ? 0
						     : m == 2 ? MD_BIST_UNSUPPORTED
							      : MD_BIST_SKIP_POLICY));
	}
	p = policy();
	p.core = 1;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_UNSUPPORTED);
	p = policy();
	p.md_present = false;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_PREREQUISITE);
	p = policy();
	p.snapshot_valid = false;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_PREREQUISITE);
	p = policy();
	p.mode = 4;
	sail_check(md_bist_assess_policy(&p) == MD_BIST_INVALID);
	p = policy();
	sail_check(md_bist_run(&p, NULL, NULL, NULL, &d) == MD_BIST_PREREQUISITE);
	p.skip = true;
	sail_check(md_bist_run(&p, NULL, NULL, NULL, &d) == MD_BIST_SKIP_POLICY);

	/* Active operation cannot silently fall back to the observer. */
	{
		struct md_bist_prerequisites pre = prerequisites();
		struct md_bist_limits limits = {.max_poll_reads = 2048};
		struct md_bist_io io = {&f,       read32,     write32, time_us,
					delay_us, invalidate, NULL,    observe};
		p = policy();
		initialize(&f, HAPPY);
		sail_check(md_bist_run(&p, &pre, &io, &limits, &d) == MD_BIST_INVALID);
		sail_check(!f.reads && !f.writes && !f.events);
		io.publish = publish;
		io.observe = NULL;
		sail_check(md_bist_run(&p, &pre, &io, &limits, &d) == 0);
		sail_check(!f.events && f.invalidations == 2);
		for (int contract = -1; contract <= 2; contract++) {
			if (contract == MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE) {
				continue;
			}
			initialize(&f, HAPPY);
			pre.access_contract = (enum md_bist_access_contract)contract;
			sail_check(md_bist_run(&p, &pre, &io, &limits, &d) == MD_BIST_PREREQUISITE);
			sail_check(!f.reads && !f.writes);
		}
	}

	/* The allowlist excludes every protected register and the whole GPIO bank. */
	const uint32_t forbidden[] = {
		0xf0305084, 0xf0305088, 0xf030508c, 0xf0305090, 0xf100000c, 0xf1000520, 0xf1000524,
		0xf0300200, 0xf1000014, 0xf1149020, 0xf1149420, 0xf0155000, 0xc2f7100,  0xc2f7104,
		0xc2f7108,  0xc2f0020,  0xc264000,  0x01ffd008, 0x01ffd00c, 0x01ffd010, 0x01ffd014};
	for (size_t n = 0; n < ARRAY_SIZE(forbidden); n++) {
		sail_check(!md_bist_write_allowed(forbidden[n]));
	}
	for (uint32_t a = 0xf0300058; a <= 0xf0300074; a += 4) {
		sail_check(!md_bist_write_allowed(a));
	}
	for (uint32_t a = 0xf1800000; a < 0xf1820000; a += 4) {
		sail_check(!md_bist_write_allowed(a));
	}
	puts("PASS: golden IO trace; policy guards; bounded timeout/frozen/backwards timer; "
	     "249/250/251 clock transitions; isolation transitions; inclusive poll deadlines; "
	     "readback/IO/time/delay/callback errors; protected-register allowlist");
	return 0;
}
