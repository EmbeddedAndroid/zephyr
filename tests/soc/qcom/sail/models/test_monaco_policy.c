/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "monaco_policy.h"
#include "sail_check.h"
#include <stdio.h>
#include <zephyr/sys/util.h>


struct step {
	char kind;
	uint32_t address, value;
	int error;
};
struct fake {
	struct step steps[400];
	unsigned int count, used, times, fail_time, regress_time;
	uint64_t time;
	bool freeze, short_delay;
};

static void add(struct fake *f, char kind, uint32_t address, uint32_t value)
{
	sail_check(f->count < ARRAY_SIZE(f->steps));
	f->steps[f->count++] = (struct step){kind, address, value, 0};
}

#define R(a, v) add(f, 'R', (a), (v))
#define M(v)    add(f, 'M', 0, (v))
#define D(v)    add(f, 'D', 0, (v))

static struct step *take(struct fake *f, char kind, uint32_t address)
{
	sail_check(f->used < f->count);
	struct step *s = &f->steps[f->used++];

	if (s->kind != kind || s->address != address) {
		fprintf(stderr, "step%u: expected %c %08x, got %c %08x\n", f->used - 1, s->kind,
			s->address, kind, address);
		sail_check(0);
	}
	return s;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct step *s = take(context, 'R', address);
	*value = s->value;
	return s->error;
}

static int mpidr(void *context, uint32_t *value)
{
	struct step *s = take(context, 'M', 0);
	*value = s->value;
	return s->error;
}

static int time_us(void *context, uint64_t *value)
{
	struct fake *f = context;

	if (++f->times == f->fail_time) {
		return -1;
	}
	if (f->times == f->regress_time) {
		f->time--;
	}
	*value = f->time;
	return 0;
}

static int delay_us(void *context, uint32_t value)
{
	struct fake *f = context;
	struct step *s = take(f, 'D', 0);

	sail_check(value == s->value);
	if (!f->freeze) {
		f->time += f->short_delay ? value - 1 : value;
	}
	return s->error;
}

static struct monaco_policy_io io(struct fake *f)
{
	return (struct monaco_policy_io){f, read32, mpidr, time_us, delay_us};
}

static struct monaco_policy_limits limits(void)
{
	return (struct monaco_policy_limits){.presence_timeout_us = 100000,
					     .ready_timeout_us = 3000000,
					     .max_presence_checks = 101,
					     .max_ready_waits = 3};
}

static void initialize(struct fake *f)
{
	*f = (struct fake){0};
}

static void ready_and_fuses(struct fake *f, uint32_t mode, uint32_t skip)
{
	/* Three distinct A4 reads on the normal immediate-ready path. */
	R(0xf03050a4, 0x13);
	R(0xf03050a4, 0x13);
	R(0xf03050a4, 0x13);
	R(0xf8706100, mode);
	R(0xc0786064, skip);
}

static void happy_prepare(struct fake *f, uint32_t mode, uint32_t skip)
{
	M(0x80000000);
	R(0xf03050c0, 0xcb);
	ready_and_fuses(f, mode, skip);
}

static void happy_finish(struct fake *f)
{
	R(0xc1ff900c, 3);
	R(0xf03050b0, 0xcb);
	R(0xf03050ac, 0);
	R(0xf03050a8, 0);
	R(0xf03050a4, 0x13);
}

static int prepare(struct fake *f, struct monaco_policy_limits *l, struct monaco_policy_snapshot *s,
		   struct monaco_policy_diagnostic *d)
{
	struct monaco_policy_io backend = io(f);

	return monaco_policy_prepare(&backend, l, s, d);
}

static int finish(struct fake *f, struct monaco_policy_snapshot *s,
		  struct monaco_policy_diagnostic *d)
{
	struct monaco_policy_io backend = io(f);

	return monaco_policy_finish(&backend, s, d);
}

static void verify_consumed(struct fake *f)
{
	sail_check(f->used == f->count);
}

static void policy_precedence(struct fake *f)
{
	struct monaco_policy_snapshot s;
	struct monaco_policy_diagnostic d;
	struct monaco_policy_limits l = limits();

	for (unsigned int mode = 0; mode < 4; mode++) {
		for (unsigned int skip = 0; skip < 2; skip++) {
			initialize(f);
			happy_prepare(f, mode << 11, skip << 8);
			sail_check(prepare(f, &l, &s, &d) == 0 && s.prepared);
			sail_check(!s.bist.snapshot_valid && s.bist.md_presence_known &&
			       s.bist.md_present);
			R(0xc1ff900c, 0xf);
			sail_check(finish(f, &s, &d) == MONACO_POLICY_WARM);
			sail_check(s.warm_cookie_valid && s.command_parts_valid == 0);
			sail_check(!s.bist.snapshot_valid);
			verify_consumed(f);

			initialize(f);
			happy_prepare(f, mode << 11, skip << 8);
			sail_check(prepare(f, &l, &s, &d) == 0);
			R(0xc1ff900c, 3);
			R(0xf03050b0, 0xfe);
			R(0xf03050ac, 0x11);
			R(0xf03050a8, 0x11);
			R(0xf03050a4, 0x11);
			sail_check(finish(f, &s, &d) == MONACO_POLICY_EDL);
			sail_check(s.bist.md_boot_word == 0xfe111111 && !s.bist.snapshot_valid);
			verify_consumed(f);

			initialize(f);
			happy_prepare(f, mode << 11, skip << 8);
			sail_check(prepare(f, &l, &s, &d) == 0);
			if (mode == 1 && !skip) {
				happy_finish(f);
				sail_check(finish(f, &s, &d) == 0 && s.bist.snapshot_valid);
				sail_check(md_bist_assess_policy(&s.bist) == MD_BIST_OK);
				sail_check(s.command_parts_valid == 15 &&
				       s.bist.md_boot_word == 0xcb000013);
			} else {
				R(0xc1ff900c, 3);
				R(0xf03050b0, 0xcb);
				sail_check(finish(f, &s, &d) == (mode == 2 && !skip
								     ? MONACO_POLICY_MODE2
								     : MONACO_POLICY_SKIP));
				sail_check(s.command_parts_valid == 1 && !s.bist.snapshot_valid);
			}
			verify_consumed(f);
			sail_check(finish(f, &s, &d) == MONACO_POLICY_INVALID);
			verify_consumed(f);
		}
	}
	/* Extra fuse bits are retained in the snapshot but excluded from policy. */
	initialize(f);
	happy_prepare(f, 0xffffc800, 0xfffffeff);
	happy_finish(f);
	sail_check(prepare(f, &l, &s, &d) == 0);
	sail_check(s.bist.mode == 1 && !s.bist.skip);
	sail_check(finish(f, &s, &d) == 0);
	verify_consumed(f);
	/* Short-circuit mismatch at AC or A8 reads no later bytes on skipped mode. */
	for (unsigned int mismatch = 1; mismatch <= 2; mismatch++) {
		initialize(f);
		happy_prepare(f, 0, 0);
		sail_check(prepare(f, &l, &s, &d) == 0);
		R(0xc1ff900c, 3);
		R(0xf03050b0, 0xfe);
		R(0xf03050ac, mismatch == 1 ? 0 : 0x11);
		if (mismatch == 2) {
			R(0xf03050a8, 0);
		}
		sail_check(finish(f, &s, &d) == MONACO_POLICY_SKIP);
		sail_check(s.command_parts_valid == (mismatch == 1 ? 3 : 7));
		verify_consumed(f);
	}
}

static void presence_boundaries(struct fake *f)
{
	struct monaco_policy_snapshot s;
	struct monaco_policy_diagnostic d;
	struct monaco_policy_limits l = limits();

	for (unsigned int last_ready = 0; last_ready <= 1; last_ready++) {
		initialize(f);
		M(0x80000000);
		R(0xf03050c0, 0);
		R(0xf03050b0, 0);
		for (unsigned int n = 0; n < 101; n++) {
			R(0xf03050c0, n == 100 && last_ready ? 0xcb : 0);
			if (n < 100) {
				D(1000);
				R(0xf03050b0, 0);
			}
		}
		if (last_ready) {
			R(0xf03050b0, 0xcb);
			R(0xf03050c0, 0xcb);
			R(0xf03050c0, 0xcb);
			ready_and_fuses(f, 0x800, 0);
		}
		sail_check(prepare(f, &l, &s, &d) == (last_ready ? 0 : MONACO_POLICY_MISSING_MD));
		sail_check(d.presence_checks == 101 && f->time == 100000);
		sail_check(!s.bist.snapshot_valid);
		verify_consumed(f);
	}
	/* Presence may arrive through B0, checked after each one-ms delay. */
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0);
	R(0xf03050b0, 0);
	R(0xf03050c0, 0);
	D(1000);
	R(0xf03050b0, 0xcb);
	R(0xf03050b0, 0xcb);
	R(0xf03050c0, 0xcb);
	R(0xf03050c0, 0xcb);
	ready_and_fuses(f, 0x800, 0);
	sail_check(prepare(f, &l, &s, &d) == 0 && d.presence_checks == 1);
	verify_consumed(f);
	/* Timer deadline and iteration limit independently stop missing-MD paths. */
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0);
	R(0xf03050b0, 0);
	R(0xf03050c0, 0);
	D(1000);
	R(0xf03050b0, 0);
	R(0xf03050c0, 0);
	l.presence_timeout_us = 999;
	sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_MISSING_MD);
	verify_consumed(f);
	l = limits();
	l.max_presence_checks = 1;
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0);
	R(0xf03050b0, 0);
	R(0xf03050c0, 0);
	sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_MISSING_MD && f->time == 0);
	verify_consumed(f);
}

static void ready_and_state_cases(struct fake *f)
{
	struct monaco_policy_snapshot s;
	struct monaco_policy_diagnostic d;
	struct monaco_policy_limits l = limits();
	/* Each classifier sample is a separate register read, including repeats. */
	for (unsigned int unsupported = 0; unsupported < 2; unsupported++) {
		initialize(f);
		M(0x80000000);
		R(0xf03050c0, 0);
		R(0xf03050b0, 0xcb);
		R(0xf03050b0, 0xcb);
		R(0xf03050c0, 0xcc);
		R(0xf03050c0, unsupported ? 0xd1 : 0xd0);
		R(0xf03050a4, 0x13);
		R(0xf03050a4, 0x13);
		R(0xf03050a4, 0x13);
		if (!unsupported) {
			R(0xf8706100, 0x800);
			R(0xc0786064, 0);
		}
		sail_check(prepare(f, &l, &s, &d) == (unsupported ? MONACO_POLICY_BOOT_STATE : 0));
		verify_consumed(f);
	}
	/* An FE/11/11 prefix bypasses the C0 range check, but A4 must end at 13. */
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0);
	R(0xf03050b0, 0xfe);
	R(0xf03050b0, 0xfe);
	R(0xf03050ac, 0x11);
	R(0xf03050a8, 0x11);
	ready_and_fuses(f, 0x800, 0);
	sail_check(prepare(f, &l, &s, &d) == 0);
	verify_consumed(f);
	for (unsigned int final = 0; final < 2; final++) {
		initialize(f);
		M(0x80000000);
		R(0xf03050c0, 0xcb);
		R(0xf03050a4, 0x11);
		R(0xf03050a4, final ? 0x13 : 0x11);
		if (final) {
			R(0xf8706100, 0x800);
			R(0xc0786064, 0);
		}
		sail_check(prepare(f, &l, &s, &d) == (final ? 0 : MONACO_POLICY_BOOT_STATE));
		verify_consumed(f);
	}
	/* A4 reading 13 and then 12 is rejected: the final value counts. */
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0xcb);
	R(0xf03050a4, 0x13);
	R(0xf03050a4, 0x13);
	R(0xf03050a4, 0x12);
	sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_BOOT_STATE);
	verify_consumed(f);
	/* One-second wait: the post-delay read tests 11, the next read tests 13. */
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0xcb);
	R(0xf03050a4, 0);
	R(0xf03050a4, 0);
	D(1000000);
	R(0xf03050a4, 0);
	R(0xf03050a4, 0x13);
	R(0xf03050a4, 0x13);
	R(0xf8706100, 0x800);
	R(0xc0786064, 0);
	sail_check(prepare(f, &l, &s, &d) == 0 && d.ready_waits == 1);
	verify_consumed(f);
	for (unsigned int timed = 0; timed < 2; timed++) {
		l = limits();
		l.max_ready_waits = 1;
		if (timed) {
			l.ready_timeout_us = 999999;
		}
		initialize(f);
		M(0x80000000);
		R(0xf03050c0, 0xcb);
		R(0xf03050a4, 0);
		R(0xf03050a4, 0);
		D(1000000);
		if (!timed) {
			R(0xf03050a4, 0);
			R(0xf03050a4, 0);
		}
		sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_READY_TIMEOUT);
		sail_check(!s.prepared && !s.bist.snapshot_valid);
		verify_consumed(f);
	}
}

static void failures(struct fake *f)
{
	struct monaco_policy_snapshot s;
	struct monaco_policy_diagnostic d;
	struct monaco_policy_limits l = limits();

	for (unsigned int failure = 0; failure < 7; failure++) {
		initialize(f);
		happy_prepare(f, 0x800, 0);
		f->steps[failure].error = -1;
		sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_IO);
		sail_check(f->used == failure + 1 && !s.prepared && !s.bist.snapshot_valid);
		sail_check(finish(f, &s, &d) == MONACO_POLICY_INVALID);
	}
	for (unsigned int failure = 0; failure < 5; failure++) {
		initialize(f);
		happy_prepare(f, 0x800, 0);
		sail_check(prepare(f, &l, &s, &d) == 0);
		unsigned int start = f->count;

		happy_finish(f);
		f->steps[start + failure].error = -1;
		sail_check(finish(f, &s, &d) == MONACO_POLICY_IO);
		sail_check(f->used == start + failure + 1 && s.consumed && !s.bist.snapshot_valid);
	}
	initialize(f);
	M(0x80000000);
	f->fail_time = 1;
	sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_IO);
	verify_consumed(f);
	initialize(f);
	M(0x80000000);
	R(0xf03050c0, 0);
	R(0xf03050b0, 0);
	R(0xf03050c0, 0);
	f->time = 10000;
	f->regress_time = 2;
	sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_TIMEBASE);
	verify_consumed(f);
	for (unsigned int kind = 0; kind < 3; kind++) {
		initialize(f);
		M(0x80000000);
		R(0xf03050c0, 0);
		R(0xf03050b0, 0);
		R(0xf03050c0, 0);
		D(1000);
		f->freeze = kind == 0;
		f->short_delay = kind == 1;
		if (kind == 2) {
			f->steps[f->count - 1].error = -1;
		}
		sail_check(prepare(f, &l, &s, &d) ==
		       (kind == 2 ? MONACO_POLICY_IO : MONACO_POLICY_TIMEBASE));
		verify_consumed(f);
	}
	const uint32_t cores[] = {0x80000001, 0x80000100, 0x80010000};

	for (unsigned int n = 0; n < 3; n++) {
		initialize(f);
		M(cores[n]);
		sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_CORE && !d.reads);
		verify_consumed(f);
	}
	/* Full-width comparison: FE in low byte with extra bits is not EDL. */
	initialize(f);
	happy_prepare(f, 0x800, 0);
	sail_check(prepare(f, &l, &s, &d) == 0);
	R(0xc1ff900c, 3);
	R(0xf03050b0, 0x100fe);
	R(0xf03050ac, 0x11);
	R(0xf03050a8, 0x11);
	R(0xf03050a4, 0x11);
	sail_check(finish(f, &s, &d) == MONACO_POLICY_BOOT_STATE);
	verify_consumed(f);
	initialize(f);
	happy_prepare(f, 0x800, 0);
	sail_check(prepare(f, &l, &s, &d) == 0);
	happy_finish(f);
	f->steps[f->count - 1].value = 7;
	sail_check(finish(f, &s, &d) == MONACO_POLICY_BOOT_STATE && !s.bist.snapshot_valid);
	verify_consumed(f);
	/* Invalid inputs perform no IO and erase a stale prepare snapshot. */
	initialize(f);
	l.max_presence_checks = 102;
	sail_check(prepare(f, &l, &s, &d) == MONACO_POLICY_INVALID && !s.prepared);
	verify_consumed(f);
	l = limits();
	initialize(f);
	happy_prepare(f, 0x800, 0);
	sail_check(prepare(f, &l, &s, &d) == 0 && s.prepared);
	struct monaco_policy_io backend = io(f);

	sail_check(monaco_policy_prepare(&backend, &l, &s, NULL) == MONACO_POLICY_INVALID);
	sail_check(!s.prepared && !s.bist.snapshot_valid);
	sail_check(finish(f, &s, &d) == MONACO_POLICY_INVALID);
	verify_consumed(f);
}

int sail_model_test(void)
{
	struct fake f;

	policy_precedence(&f);
	presence_boundaries(&f);
	ready_and_state_cases(&f);
	failures(&f);
	puts("PASS: read-only policy; sampling and short-circuit order; warm/EDL/skip/mode "
	     "precedence; "
	     "101-check presence boundary; bounded ready wait; transient states; fuse masks; "
	     "MPIDR/IO/timer/delay failures; once-only snapshot; reviewed BIST-policy "
	     "compatibility");
	return 0;
}
